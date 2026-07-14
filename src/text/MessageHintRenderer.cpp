/**
 * @file MessageHintRenderer.cpp（文件名）
 * @brief 消息提示 渲染器 实现
 */

#include "text/MessageHintRenderer.h"
#include "text/VulkanTextOverlayBridge.h"
#include "renderer/VulkanRenderer.h"
#include "core/PathConfig.h"
#include "utils/Logger.h"
#include "stb_image.h"
#include "utils/FileUtils.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <mutex>
#include <cmath>

namespace {

static bool nextCodepoint(const char*& p, const char* end, uint32_t& cp) {
  if (p >= end) return false;
  unsigned char c0 = static_cast<unsigned char>(*p);
  ++p;

  if (c0 < 0x80) {
    cp = c0;
    return true;
  }

  if (c0 < 0xe0) {
    if (p >= end) return false;
    unsigned char c1 = static_cast<unsigned char>(*p);
    ++p;
    cp = ((c0 & 0x1f) << 6) | (c1 & 0x3f);
    return true;
  }

  if (c0 < 0xf0) {
    if (p + 1 >= end) return false;
    unsigned char c1 = static_cast<unsigned char>(*p);
    ++p;
    unsigned char c2 = static_cast<unsigned char>(*p);
    ++p;
    cp = ((c0 & 0x0f) << 12) |
         ((c1 & 0x3f) << 6) |
         (c2 & 0x3f);
    return true;
  }

  if (c0 < 0xf8) {
    if (p + 2 >= end) return false;
    unsigned char c1 = static_cast<unsigned char>(*p);
    ++p;
    unsigned char c2 = static_cast<unsigned char>(*p);
    ++p;
    unsigned char c3 = static_cast<unsigned char>(*p);
    ++p;
    cp = ((c0 & 0x07) << 18) |
         ((c1 & 0x3f) << 12) |
         ((c2 & 0x3f) << 6) |
         (c3 & 0x3f);
    return true;
  }

  return false;
}

static int utf8CodepointCount(const std::string& s) {
  int n = 0;
  const char* p = s.c_str();
  const char* end = p + s.size();
  uint32_t cp;
  while (nextCodepoint(p, end, cp)) ++n;
  return n;
}

static std::string utf8SubstringByCodepoints(const std::string& s, int start, int count) {
  const char* from = s.c_str();
  const char* end = from + s.size();
  const char* q = from;
  uint32_t cp;
  for (int i = 0; i < start && q < end; ++i) nextCodepoint(q, end, cp);
  from = q;
  for (int i = 0; i < count && q < end; ++i) nextCodepoint(q, end, cp);
  return std::string(from, q);
}

// 列表行格式为 "N．" + 歌名，只对歌名部分做滚动；返回 (前缀, 歌名)，若未找到 "．" 则前缀为空、整行为名
static void splitListLinePrefixAndName(const std::string& line, std::string& prefix, std::string& name) {
  const unsigned char dot[] = { 0xEF, 0xBC, 0x8E };
  const size_t dotLen = sizeof(dot);
  prefix.clear();
  name = line;
  if (line.size() < dotLen) return;
  for (size_t i = 0; i + dotLen <= line.size(); ++i) {
    if (static_cast<unsigned char>(line[i]) == dot[0] &&
        static_cast<unsigned char>(line[i + 1]) == dot[1] &&
        static_cast<unsigned char>(line[i + 2]) == dot[2]) {
      prefix = line.substr(0, i + dotLen);
      name = line.substr(i + dotLen);
      return;
    }
  }
}

// 图层 41 提示图标命名规范（与 app/src/main/assets/res 下的文件名一一对应）：
// - PLAY -> play.png 映射
// - PAUSE -> pause.png 映射
// - NEXT -> next.png 映射
// - PREV -> replay.png 映射
// - VOLUME_UP -> volume_up.png 映射
// - VOLUME_DOWN -> volume_down.png 映射
// - MUTE -> mute.png 映射
// - UNMUTE -> unmute.png 映射
// - AUDIO_TRACK -> audio_track.png 映射
// - BACKING_TRACK -> backing_track.png 映射
//
// 所有图标文件必须放在 RES_DIR（当前 ROOT_PATH/res/）下，并使用以上文件名。
const char *getIconFileForType(hsvj::HintType type) {
  using hsvj::HintType;
  switch (type) {
  case HintType::PLAY:
    return "play.png";
  case HintType::PAUSE:
    return "pause.png";
  case HintType::NEXT:
    return "next.png";
  case HintType::PREV:
    return "replay.png";
  case HintType::VOLUME_UP:
    return "volume_up.png";
  case HintType::VOLUME_DOWN:
    return "volume_down.png";
  case HintType::MUTE:
    return "mute.png";
  case HintType::UNMUTE:
    return "unmute.png";
  case HintType::AUDIO_TRACK:
    return "audio_track.png";
  case HintType::BACKING_TRACK:
    return "backing_track.png";
  default:
    return nullptr;
  }
}

// 操作提示文字预热（与 preloadHintIcons 分帧执行，降低首帧 Vulkan 压力）
static const char* const kMessageHintWarmupTexts[] = {
    "播放", "暂停", "下一首", "重播",
    "音量调大", "音量调小", "静音", "取消静音",
    "原唱", "伴唱",
    "音量 5",  "音量 10", "音量 15", "音量 20", "音量 25",
    "音量 30", "音量 35", "音量 40", "音量 45", "音量 50",
    "音量 55", "音量 60", "音量 65", "音量 70", "音量 75",
    "音量 80", "音量 85", "音量 90", "音量 95", "音量 100",
    "未找到视频文件，请检查视频文件服务器",
    nullptr
};
static constexpr int kWarmupTextsPerFrame = 4;

static constexpr int kMaxHintTextBytes = 512;
static constexpr int kMaxHintBitmapDim = 4096;
static constexpr size_t kMaxHintBitmapBytes = 4u * 1024u * 1024u;

static float normalizeHintFontSize(float size) {
    if (!std::isfinite(size) || size <= 0.0f) {
        return 36.0f;
    }
    return size;
}

static float derivePlaylistFontSize(float operationFontSize) {
    return operationFontSize * (28.0f / 36.0f);
}

static std::string makeTextCacheKey(const std::string& text, float fontSize) {
    std::ostringstream oss;
    oss << std::lround(fontSize * 10.0f) << '|' << text;
    return oss.str();
}

static std::string trimHintLine(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

static std::vector<std::string> splitHintLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line, '\n')) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        line = trimHintLine(std::move(line));
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

static std::string joinHintLines(const std::vector<std::string>& lines) {
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            text += '\n';
        }
        text += lines[i];
    }
    return text;
}

static bool isValidHintBitmap(int width, int height, size_t rgbaBytes) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width > kMaxHintBitmapDim || height > kMaxHintBitmapDim) {
        return false;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    return expected > 0 && expected <= kMaxHintBitmapBytes && expected == rgbaBytes;
}

} // 命名空间

namespace hsvj {

MessageHintRenderer::MessageHintRenderer()
    : initialized_(false)
    , dirty_(true)
    , renderWidth_(1920)
    , renderHeight_(1080)
    , playlistShowCount_(3)
    , playlistTotalRemaining_(0)
    , playlistHintVisible_(false)
    , operationHintActive_(false)
    , protectedOperationHint_(false)
    , currentTextureType_(0)
    , currentTextureWidth_(0)
    , currentTextureHeight_(0)
    , lastOperationText_("")
    , lastOperationCacheKey_("")
    , lastPlaylistText_("")
    , lastOperationHintType_(HintType::NONE)
    , lastAppliedAlign_(-1)
    , fontSize_(36.0f)
    , playlistFontSize_(derivePlaylistFontSize(fontSize_))  // 播放列表缩小以配合每行 12 字，减少歌名滚动
    , displayAlign_(0)
    , textColorR_(1.0f), textColorG_(1.0f), textColorB_(1.0f), textColorA_(1.0f)
    , bgColorR_(0.0f), bgColorG_(0.0f), bgColorB_(0.0f), bgAlpha_(0.6f)
    , operationHintDuration_(0.0f) {  // 默认0，由外部LayerText设置
}

MessageHintRenderer::~MessageHintRenderer() {
    shutdown();
}

bool MessageHintRenderer::initialize(VulkanTextOverlayBridge* sharedBridge, int width, int height) {
    if (initialized_) return true;
    renderWidth_ = width;
    renderHeight_ = height;
    
    if (!sharedBridge || !sharedBridge->isInitialized()) {
        LOG_ERROR("[MHR] 初始化失败: 共享桥接器未就绪");
        return false;
    }
    
    // 核心优化：创建自己的实例，但从共享桥接器借用 FreeType 句柄（解决 Layer 40/41 纹理冲突）
    ownedVtoBridge_ = std::make_unique<VulkanTextOverlayBridge>();
    bool sharedSuccess = false;
    if (sharedBridge && sharedBridge->isInitialized()) {
        sharedSuccess = ownedVtoBridge_->initializeShared(sharedBridge);
    }

    if (!sharedSuccess) {
        LOG_WARN("[MHR] 共享字体句柄失败或无共享实例，降级为独立加载逻辑");
        std::vector<std::string> paths;
        paths.push_back(FONT_DIR + "lyric.ttf");
        paths.push_back(FONT_DIR + "custom.ttf");
        
        bool loaded = false;
        for (const auto& p : paths) {
            if (!p.empty() && hsvj::FileUtils::exists(p)) {
                if (ownedVtoBridge_->initialize(p, fontSize_)) {
                    loaded = true;
                    break;
                }
            }
        }
        if (!loaded) {
            LOG_ERROR("[MHR] 独立加载字体也失败了，消息提示将无法显示");
            ownedVtoBridge_.reset();
            return false;
        }
    }

    vtoBridgePtr_ = ownedVtoBridge_.get();
    initialized_ = true;
    return true;
}

void MessageHintRenderer::shutdown() {
    vtoBridgePtr_ = nullptr;
    ownedVtoBridge_.reset();
    initialized_ = false;
}

void MessageHintRenderer::releaseGpuResources(VulkanRenderer* renderer) {
    if (renderer) {
        for (auto& pair : textCache_) {
            if (pair.second.textureId != 0) {
                renderer->requestDestroyTexture(pair.second.textureId, 3);
                pair.second.textureId = 0;
            }
        }
        auto releaseIcon = [renderer](IconTextureInfo& icon) {
            if (icon.textureId != 0) {
                renderer->requestDestroyTexture(icon.textureId, 3);
                icon.textureId = 0;
            }
        };
        releaseIcon(playIcon_);
        releaseIcon(pauseIcon_);
        releaseIcon(nextIcon_);
        releaseIcon(prevIcon_);
        releaseIcon(volumeUpIcon_);
        releaseIcon(volumeDownIcon_);
        releaseIcon(muteIcon_);
        releaseIcon(unmuteIcon_);
        releaseIcon(audioTrackIcon_);
        releaseIcon(backingTrackIcon_);
    }
    dropGpuCachesWithoutDestroy();
}

void MessageHintRenderer::dropGpuCachesWithoutDestroy() {
    auto resetIcon = [](IconTextureInfo &i) {
        i.textureId = 0;
        i.width = i.height = 0;
        i.loaded = false;
        i.tried = false;
    };
    {
        std::lock_guard<std::mutex> lk(uploadMutex_);
        pendingUploads_.clear();
    }
    textCache_.clear();
    textCacheOrder_.clear();
    resetIcon(playIcon_);
    resetIcon(pauseIcon_);
    resetIcon(nextIcon_);
    resetIcon(prevIcon_);
    resetIcon(volumeUpIcon_);
    resetIcon(volumeDownIcon_);
    resetIcon(muteIcon_);
    resetIcon(unmuteIcon_);
    resetIcon(audioTrackIcon_);
    resetIcon(backingTrackIcon_);
    iconsPreloaded_ = false;
    textWarmupNextIndex_ = -1;
    dirty_ = true;
    currentTextureType_ = 0;
    currentTextureWidth_ = 0;
    currentTextureHeight_ = 0;
    lastOperationText_.clear();
    lastOperationCacheKey_.clear();
    lastPlaylistText_.clear();
}

void MessageHintRenderer::setFontSize(float size) {
    const float normalized = normalizeHintFontSize(size);
    const float playlistSize = derivePlaylistFontSize(normalized);
    if (fontSize_ == normalized && playlistFontSize_ == playlistSize) {
        return;
    }

    fontSize_ = normalized;
    playlistFontSize_ = playlistSize;
    {
        std::lock_guard<std::mutex> lk(uploadMutex_);
        pendingUploads_.clear();
    }
    lastOperationCacheKey_.clear();
    lastPlaylistText_.clear();
    currentTextureWidth_ = 0;
    currentTextureHeight_ = 0;
    textWarmupNextIndex_ = iconsPreloaded_ ? 0 : -1;
    dirty_ = true;
}

void MessageHintRenderer::update(float deltaTime) {
    if (!initialized_) return;

    std::lock_guard<std::mutex> lock(hintMutex_);
    if (operationHintActive_ && !currentOperationHint_.isPermanent) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - currentOperationHint_.startTime).count();
        if (elapsed >= currentOperationHint_.displayDuration) {
            operationHintActive_ = false;
            currentTextureType_ = 0;
            // 列表可见性由 Engine_播放列表::update播放列表HintLayer 按时间控制，此处不恢复
        }
    }
    if (playlistHintVisible_ && !playlistItems_.empty()) {
        playlistScrollTimeAccum_ += deltaTime;
        const float scrollInterval = 0.5f;
        const int visibleChars = 12;
        int maxScrollRange = 0;
        for (size_t i = 0; i < static_cast<size_t>(playlistShowCount_) && i < playlistItems_.size(); ++i) {
            int nameLen = utf8CodepointCount(playlistItems_[i].name);
            if (nameLen > visibleChars)
                maxScrollRange = std::max(maxScrollRange, nameLen - visibleChars + 1);
        }
        if (playlistScrollTimeAccum_ >= scrollInterval) {
            playlistScrollTimeAccum_ -= scrollInterval;
            if (playlistScrollRestAtZero_) {
                playlistScrollRestAtZero_ = false;
            } else {
                ++playlistScrollCharOffset_;
                if (maxScrollRange > 0 && (playlistScrollCharOffset_ % maxScrollRange) == 0)
                    playlistScrollRestAtZero_ = true;
                dirty_ = true;
            }
        }
    }
}

void MessageHintRenderer::updateTextures(VulkanRenderer* renderer, int width, int height) {
    if (!initialized_ || !renderer || !vtoBridgePtr_) {
        return;
    }
    if (renderer->isRenderPassStarted()) {
        return;
    }

    // 首次调用仅预加载 PNG 图标；文字纹理从下一帧起分帧预热，避免与视频首帧同帧挤爆 Vulkan
    if (!iconsPreloaded_) {
        preloadHintIcons(renderer);
        iconsPreloaded_ = true;
        textWarmupNextIndex_ = vtoBridgePtr_ ? 0 : -1;
    } else if (textWarmupNextIndex_ >= 0 && vtoBridgePtr_) {
        int processed = 0;
        while (processed < kWarmupTextsPerFrame) {
            const char* wt = kMessageHintWarmupTexts[textWarmupNextIndex_];
            if (!wt) {
                textWarmupNextIndex_ = -1;
                LOG_INFO("[MHR] warmupTextCache: %zu text entries cached", textCache_.size());
                break;
            }
            const std::string t = wt;
            const std::string cacheKey = makeTextCacheKey(t, fontSize_);
            if (!textCache_.count(cacheKey)) {
                if (vtoBridgePtr_->prepareText(renderer, t, 1.0f, 1.0f, 1.0f, 1.0f, fontSize_,
                                               0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f)) {
                    CachedTextEntry entry;
                    entry.width     = vtoBridgePtr_->getTextureWidth();
                    entry.height    = vtoBridgePtr_->getTextureHeight();
                    entry.textureId = vtoBridgePtr_->detachTexture();
                    insertTextCache(renderer, cacheKey, entry);
                }
            }
            ++textWarmupNextIndex_;
            ++processed;
        }
    }

    // 消费后台异步光栅化结果：把 RGBA 数据上传 GPU 存入 textCache_
    {
        std::vector<PendingUpload> toUpload;
        {
            std::lock_guard<std::mutex> lk(uploadMutex_);
            toUpload.swap(pendingUploads_);
        }
        for (auto& pu : toUpload) {
            if (textCache_.count(pu.cacheKey)) continue; // 已有缓存则丢弃
            if (!isValidHintBitmap(pu.width, pu.height, pu.rgba.size())) {
                LOG_WARN("[MHR] drop invalid pending text bitmap: text='%s' size=%dx%d bytes=%zu",
                         pu.text.c_str(), pu.width, pu.height, pu.rgba.size());
                continue;
            }
            uint32_t texId = renderer->allocateTextureId();
            size_t sz = static_cast<size_t>(pu.width) * static_cast<size_t>(pu.height) * 4u;
            bool ok = (sz <= 2u * 1024u * 1024u)
                ? renderer->createTextureFromRGBAStaged(pu.rgba.data(), pu.width, pu.height, texId)
                : renderer->createTextureFromRGBADirect(pu.rgba.data(), pu.width, pu.height, texId);
            if (ok) {
                CachedTextEntry entry;
                entry.textureId = texId;
                entry.width     = pu.width;
                entry.height    = pu.height;
                insertTextCache(renderer, pu.cacheKey, entry);
                LOG_INFO("[MHR] async upload done: '%s' tid=%u", pu.text.c_str(), texId);
                dirty_ = true;  // 新纹理上传完成，标脏确保下帧 updateTextures 被调用
            }
        }
    }

    bool opActive = false;
    HintType hintType = HintType::NONE;
    std::string hintText;
    bool plVisible = false;
    std::vector<PlaylistItemInfo> plItems;
    int plShowCount = 0;
    int plTotalRemaining = 0;
    int plListType = 0;
    {
        std::lock_guard<std::mutex> lock(hintMutex_);
        opActive = operationHintActive_;
        hintType = currentOperationHint_.type;
        hintText = currentOperationHint_.text;
        plVisible = playlistHintVisible_;
        plItems = playlistItems_;
        plShowCount = playlistShowCount_;
        plTotalRemaining = playlistTotalRemaining_;
        plListType = playlistHintListType_;
    }
    if (opActive) {
        std::string label;
        switch (hintType) {
        case HintType::PLAY: label = "播放"; break;
        case HintType::PAUSE: label = "暂停"; break;
        case HintType::NEXT: label = "下一首"; break;
        case HintType::PREV: label = "重播"; break;
        case HintType::VOLUME_UP: label = "音量调大"; break;
        case HintType::VOLUME_DOWN: label = "音量调小"; break;
        case HintType::MUTE: label = "静音"; break;
        case HintType::UNMUTE: label = "取消静音"; break;
        case HintType::AUDIO_TRACK: label = "原唱"; break;
        case HintType::BACKING_TRACK: label = "伴唱"; break;
        case HintType::PLAYLIST: label = "播放列表"; break;
        default: break;
        }
        std::string displayText = hintText.empty() ? label : hintText;
        std::vector<std::string> displayLines = splitHintLines(displayText);
        if (displayLines.empty() && !label.empty()) {
            displayLines.push_back(label);
        }
        displayText = joinHintLines(displayLines);
        if (displayText.empty()) displayText = label;
        if (displayText.size() > kMaxHintTextBytes) {
            displayText.resize(kMaxHintTextBytes);
            LOG_WARN("[MHR] operation hint text truncated to %d bytes", kMaxHintTextBytes);
            displayLines = splitHintLines(displayText);
            if (displayLines.empty() && !label.empty()) {
                displayLines.push_back(label);
            }
            displayText = joinHintLines(displayLines);
        }
        const bool isMultilineOperationHint = displayLines.size() > 1;

        // 内容缓存：文字和类型都没变则跳过 FreeType 光栅化，避免每帧卡顿
        lastOperationText_ = displayText;
        lastOperationCacheKey_ = makeTextCacheKey(displayText, fontSize_);
        lastOperationHintType_ = hintType;
        {
            std::lock_guard<std::mutex> lock(hintMutex_);
            currentTextureType_ = 1;
        }
        // 多纹理缓存：每种文字独立纹理，切换时直接用缓存，不重新光栅化
        if (textCache_.find(lastOperationCacheKey_) == textCache_.end()) {
            if (isMultilineOperationHint && vtoBridgePtr_) {
                const float lineH = fontSize_ * 1.25f;
                if (vtoBridgePtr_->prepareTextLines(renderer, displayLines,
                        textColorR_, textColorG_, textColorB_, textColorA_,
                        fontSize_, lineH,
                        0.0f, 0.0f, 0.0f, 0.0f,
                        0, 0, 0, true, 0, -1)) {
                    CachedTextEntry entry;
                    entry.width = vtoBridgePtr_->getTextureWidth();
                    entry.height = vtoBridgePtr_->getTextureHeight();
                    entry.textureId = vtoBridgePtr_->detachTexture();
                    insertTextCache(renderer, lastOperationCacheKey_, entry);
                } else {
                    if (dirty_) dirty_ = false;
                    return;
                }
            } else {
                // 缓存未命中：异步光栅化，本帧跳过渲染，下帧有缓存后再显示（避免同步卡顿）
                rasterizeAsync(displayText, lastOperationCacheKey_, fontSize_);
                if (dirty_) dirty_ = false;
                return;
            }
        }

        // 在 render pass 外加载 / 更新操作图标纹理（参考 Lyric渲染器 的 countdown 实现）
        IconTextureInfo *iconInfo = nullptr;
        switch (hintType) {
        case HintType::PLAY:
          iconInfo = &playIcon_;
          break;
        case HintType::PAUSE:
          iconInfo = &pauseIcon_;
          break;
        case HintType::NEXT:
          iconInfo = &nextIcon_;
          break;
        case HintType::PREV:
          iconInfo = &prevIcon_;
          break;
        case HintType::VOLUME_UP:
          iconInfo = &volumeUpIcon_;
          break;
        case HintType::VOLUME_DOWN:
          iconInfo = &volumeDownIcon_;
          break;
        case HintType::MUTE:
          iconInfo = &muteIcon_;
          break;
        case HintType::UNMUTE:
          iconInfo = &unmuteIcon_;
          break;
        case HintType::AUDIO_TRACK:
          iconInfo = &audioTrackIcon_;
          break;
        case HintType::BACKING_TRACK:
          iconInfo = &backingTrackIcon_;
          break;
        default:
          break;
        }

        if (iconInfo && !iconInfo->tried) {
          iconInfo->tried = true;
          const char *fileName = getIconFileForType(hintType);
          if (fileName && !RES_DIR.empty()) {
            std::string path = RES_DIR + std::string(fileName);
            std::string normalized = hsvj::FileUtils::normalizePath(path);
            if (hsvj::FileUtils::exists(normalized)) {
              int w = 0, h = 0, n = 0;
              stbi_uc *data = stbi_load(normalized.c_str(), &w, &h, &n, 4);
              if (data) {
                uint32_t texId = renderer->allocateTextureId();
                bool ok = false;
                size_t rgbaSize =
                    static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
                if (rgbaSize <= 2u * 1024u * 1024u) {
                  ok = renderer->createTextureFromRGBAStaged(
                      data, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                      texId);
                } else {
                  ok = renderer->createTextureFromRGBADirect(
                      data, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                      texId);
                }
                stbi_image_free(data);
                if (ok) {
                  iconInfo->textureId = texId;
                  iconInfo->width = w;
                  iconInfo->height = h;
                  iconInfo->loaded = true;
                  LOG_INFO("MessageHintRenderer: loaded icon %s from %s",
                           fileName, normalized.c_str());
                } else {
                  LOG_WARN(
                      "MessageHintRenderer: failed to upload icon texture %s",
                      normalized.c_str());
                }
              } else {
                LOG_WARN("MessageHintRenderer: stbi_load failed for %s",
                         normalized.c_str());
              }
            } else {
              LOG_DEBUG("MessageHintRenderer: icon file not found: %s",
                        normalized.c_str());
            }
          }
        }
        
        if (dirty_) dirty_ = false;
        return;  // 不再处理播放列表提示
    }
    
    if (plVisible && !plItems.empty()) {
        const int visibleChars = 12;  // 每行固定显示 12 个字，超出部分整行歌名滚动，行号不滚动
        static const char* const listDot = "\xEF\xBC\x8E";  // "．" 行号与文字无间距
        std::vector<std::string> fullLines;
        fullLines.push_back(plListType == 1 ? "——已选列表——" : "——即将播放——");
        int count = std::min(static_cast<int>(plItems.size()), plShowCount);
        for (int i = 0; i < count; i++) {
            fullLines.push_back(std::to_string(i + 1) + listDot + plItems[i].name);
        }
        if (plTotalRemaining > 0) {
            fullLines.push_back("剩余 " + std::to_string(plTotalRemaining) + " 首");
        }
        int maxScrollRange = 0;
        for (size_t i = 1; i <= static_cast<size_t>(count); ++i) {
            if (i >= fullLines.size()) break;
            std::string pre, name;
            splitListLinePrefixAndName(fullLines[i], pre, name);
            int nameLen = utf8CodepointCount(name);
            if (nameLen > visibleChars) maxScrollRange = std::max(maxScrollRange, nameLen - visibleChars + 1);
        }
        int scrollOffset = (maxScrollRange > 0) ? (playlistScrollCharOffset_ % maxScrollRange) : 0;
        std::vector<std::string> lines;
        lines.push_back(fullLines[0]);
        for (size_t i = 1; i < fullLines.size(); ++i) {
            if (i <= static_cast<size_t>(count)) {
                std::string prefix, name;
                splitListLinePrefixAndName(fullLines[i], prefix, name);
                int nameLen = utf8CodepointCount(name);
                if (nameLen <= visibleChars) {
                    lines.push_back(fullLines[i]);
                } else {
                    lines.push_back(prefix + utf8SubstringByCodepoints(name, scrollOffset, visibleChars));
                }
            } else {
                lines.push_back(fullLines[i]);
            }
        }
        std::string newText;
        for (const auto& s : lines) newText += s + "\n";

        // 内容缓存：若与上次渲染结果相同则跳过 prepareTextLines，减少 CPU 开销
        if (newText == lastPlaylistText_ && width == currentTextureWidth_ && height == currentTextureHeight_ && displayAlign_ == lastAppliedAlign_) {
            std::lock_guard<std::mutex> lock(hintMutex_);
            currentTextureType_ = 2;
            if (dirty_) dirty_ = false;
        } else {
            lastPlaylistText_ = std::move(newText);
            currentTextureWidth_ = width;
            currentTextureHeight_ = height;
            lastAppliedAlign_ = displayAlign_;
            {
                std::lock_guard<std::mutex> lock(hintMutex_);
                currentTextureType_ = 2;
            }
            float useBgAlpha = (bgAlpha_ > 0.01f) ? bgAlpha_ : 0.6f;
            float lineH = playlistFontSize_ * 1.4f;
            int padH = static_cast<int>(playlistFontSize_ * 0.55f);
            int padV = static_cast<int>(playlistFontSize_ * 0.45f);
            if (padH < 12) padH = 12;
            if (padV < 10) padV = 10;
            int maxW = static_cast<int>(playlistFontSize_ * 11.0f);
            if (maxW < 260) maxW = 260;
            if (maxW > 520) maxW = 520;
            int cornerRadius = static_cast<int>(playlistFontSize_ * 0.5f);
            if (cornerRadius < 8) cornerRadius = 8;
            if (cornerRadius > 24) cornerRadius = 24;
            // centerFirstLine=false；rightAlignFromLineIndex=-1：标题与列表编号、歌名均靠左显示
            vtoBridgePtr_->prepareTextLines(renderer, lines, textColorR_, textColorG_, textColorB_, textColorA_, playlistFontSize_, lineH,
                                        0.0f, 0.0f, 0.0f, useBgAlpha, padH, padV, maxW, false, cornerRadius, -1);
        }
    } else {
        std::lock_guard<std::mutex> lock(hintMutex_);
        if (currentTextureType_ != 0) currentTextureType_ = 0;
    }
    if (dirty_) dirty_ = false;
}

void MessageHintRenderer::render(VulkanRenderer* renderer, int x, int y, int width, int height, float alpha, int displayAlign) {
    if (!initialized_ || !renderer) {
        return;
    }
    int effectiveAlign = (displayAlign >= 0 && displayAlign <= 2) ? displayAlign : displayAlign_;
    bool opActive = false;
    bool plVisible = false;
    bool plHasItems = false;
    int texType = 0;
    {
        std::lock_guard<std::mutex> lock(hintMutex_);
        opActive = operationHintActive_;
        plVisible = playlistHintVisible_;
        plHasItems = !playlistItems_.empty();
        texType = currentTextureType_;
    }
    if (opActive) {
        if (texType != 1) {
            return;
        }
        renderOperationHint(renderer, x, y, width, height, alpha);
        return;
    }
    if (plVisible && plHasItems && texType == 2) {
        renderPlaylistHint(renderer, x, y, width, height, alpha, effectiveAlign);
        return;
    }
}

void MessageHintRenderer::renderPlaylistHint(VulkanRenderer* renderer, int x, int y, int width, int height, float alpha, int displayAlign) {
    if (!vtoBridgePtr_ || vtoBridgePtr_->getTextureId() == 0) return;
    int rw = vtoBridgePtr_->getTextureWidth();
    int rh = vtoBridgePtr_->getTextureHeight();
    if (rw <= 0 || rh <= 0) return;
    int align = (displayAlign >= 0 && displayAlign <= 2) ? displayAlign : displayAlign_;
    int px;
    if (align == 0) {
        px = x;
    } else if (align == 2) {
        px = x + width - rw;
    } else {
        px = x + (width - rw) / 2;
    }
    int py = y + (height - rh) / 2;
    renderer->renderLayer(vtoBridgePtr_->getTextureId(), px, py, rw, rh, 0.0f, 1.0f, alpha);
}

void MessageHintRenderer::renderOperationHint(VulkanRenderer* renderer, int x, int y, int width, int height, float alpha) {
    if (!renderer) return;
    bool active = false;
    { std::lock_guard<std::mutex> lock(hintMutex_); active = operationHintActive_; }
    if (!active) return;

    // 从多纹理缓存取文字纹理
    uint32_t textTexId = 0;
    int textWidth = 0, textHeight = 0;
    auto it = textCache_.find(lastOperationCacheKey_);
    if (it != textCache_.end() && it->second.textureId != 0) {
        textTexId  = it->second.textureId;
        textWidth  = it->second.width;
        textHeight = it->second.height;
    }
    if (textWidth <= 0) textWidth = 200;
    if (textHeight <= 0) textHeight = 48;

    IconTextureInfo* iconInfo = nullptr;
    switch (lastOperationHintType_) {
    case HintType::PLAY:          iconInfo = &playIcon_; break;
    case HintType::PAUSE:         iconInfo = &pauseIcon_; break;
    case HintType::NEXT:          iconInfo = &nextIcon_; break;
    case HintType::PREV:          iconInfo = &prevIcon_; break;
    case HintType::VOLUME_UP:     iconInfo = &volumeUpIcon_; break;
    case HintType::VOLUME_DOWN:   iconInfo = &volumeDownIcon_; break;
    case HintType::MUTE:          iconInfo = &muteIcon_; break;
    case HintType::UNMUTE:        iconInfo = &unmuteIcon_; break;
    case HintType::AUDIO_TRACK:   iconInfo = &audioTrackIcon_; break;
    case HintType::BACKING_TRACK: iconInfo = &backingTrackIcon_; break;
    default: break;
    }

    if (iconInfo && iconInfo->loaded && iconInfo->textureId != 0) {
        float maxIconWidth = 80.0f;
        float scale = maxIconWidth / static_cast<float>(std::max(iconInfo->width, 1));
        int iconW = static_cast<int>(iconInfo->width * scale);
        int iconH = static_cast<int>(iconInfo->height * scale);
        const int gap = 8;
        int blockH = iconH + gap + textHeight;
        int blockY = y + (height - blockH) / 2;
        int iconX = x + (width - iconW) / 2;
        renderer->renderLayer(iconInfo->textureId, iconX, blockY, iconW, iconH, 0.0f, 1.0f, alpha);
        if (textTexId != 0) {
            int drawW = textWidth, drawH = textHeight;
            if (drawW > width) {
                float s = static_cast<float>(width) / drawW;
                drawW = width; drawH = static_cast<int>(drawH * s);
            }
            int textX = x + (width - drawW) / 2;
            int textY = blockY + iconH + gap;
            renderer->renderLayer(textTexId, textX, textY, drawW, drawH, 0.0f, 1.0f, alpha);
        }
        return;
    }

    // 无图标：只显示文字，垂直居中
    if (textTexId != 0) {
        int drawW = textWidth, drawH = textHeight;
        if (drawW > width) {
            float s = static_cast<float>(width) / drawW;
            drawW = width; drawH = static_cast<int>(drawH * s);
        }
        int textX = x + (width - drawW) / 2;
        int textY = y + (height - drawH) / 2;
        renderer->renderLayer(textTexId, textX, textY, drawW, drawH, 0.0f, 1.0f, alpha);
    }
}

void MessageHintRenderer::setPlaylistHint(const std::vector<PlaylistItemInfo>& items, int showCount, int totalRemaining) {
    std::lock_guard<std::mutex> lock(hintMutex_);
    const int safeShowCount = std::clamp(showCount, 0, 10);
    std::vector<PlaylistItemInfo> safeItems = items;
    if (safeItems.size() > static_cast<size_t>(safeShowCount)) {
        safeItems.resize(static_cast<size_t>(safeShowCount));
    }
    for (auto& item : safeItems) {
        if (item.name.size() > kMaxHintTextBytes) {
            item.name.resize(kMaxHintTextBytes);
        }
        if (item.artist.size() > kMaxHintTextBytes) {
            item.artist.resize(kMaxHintTextBytes);
        }
    }
    if (playlistItems_ != safeItems || playlistShowCount_ != safeShowCount || playlistTotalRemaining_ != totalRemaining) {
        playlistItems_ = std::move(safeItems);
        playlistShowCount_ = safeShowCount;
        playlistTotalRemaining_ = totalRemaining;
        dirty_ = true;
    }
}

void MessageHintRenderer::clearPlaylistHint() {
    std::lock_guard<std::mutex> lock(hintMutex_);
    playlistItems_.clear();
    playlistTotalRemaining_ = 0;
    playlistHintVisible_ = false;
    if (currentTextureType_ == 2) {
        currentTextureType_ = 0;
    }
    dirty_ = true;
}

void MessageHintRenderer::setPlaylistHintVisible(bool visible) {
    std::lock_guard<std::mutex> lock(hintMutex_);
    if (protectedOperationHint_ && visible) {
        return;
    }
    if (playlistHintVisible_ != visible) {
        playlistHintVisible_ = visible;
        dirty_ = true;
        if (!visible && currentTextureType_ == 2) {
            currentTextureType_ = 0;
        }
    }
}

void MessageHintRenderer::showOperationHint(HintType type, const std::string& customText) {
    std::lock_guard<std::mutex> lock(hintMutex_);
    if (protectedOperationHint_) {
        LOG_DEBUG("MessageHintRenderer: protected operation hint active, ignore type=%d text='%s'",
                  static_cast<int>(type), customText.c_str());
        return;
    }

    bool wasActive = operationHintActive_;
    bool wasPermanent = currentOperationHint_.isPermanent;
    HintType oldType = currentOperationHint_.type;
    std::string oldText = currentOperationHint_.text;

    if (wasActive && wasPermanent && shouldHidePermanentHint(type)) {
        dirty_ = true;
    }

    bool contentChanged = (type != oldType || customText != oldText);
    bool permanentHint = (type == HintType::MUTE || type == HintType::PAUSE);
    currentOperationHint_.type = type;
    currentOperationHint_.text = customText;
    currentOperationHint_.startTime = std::chrono::steady_clock::now();
    currentOperationHint_.isPermanent = permanentHint;
    currentOperationHint_.displayDuration = permanentHint ? 0.0f : operationHintDuration_;
    playlistHintVisible_ = false;
    operationHintActive_ = true;
    // 只有内容变化时才标脏，避免重复触发 FreeType 光栅化
    if (contentChanged || !wasActive) dirty_ = true;
}

void MessageHintRenderer::showProtectedOperationHint(HintType type, const std::string& customText) {
    std::lock_guard<std::mutex> lock(hintMutex_);
    bool contentChanged = (type != currentOperationHint_.type || customText != currentOperationHint_.text);
    bool wasActive = operationHintActive_;

    currentOperationHint_.type = type;
    currentOperationHint_.text = customText;
    currentOperationHint_.startTime = std::chrono::steady_clock::now();
    currentOperationHint_.isPermanent = true;
    currentOperationHint_.displayDuration = 0.0f;
    protectedOperationHint_ = true;
    playlistHintVisible_ = false;
    operationHintActive_ = true;

    if (contentChanged || !wasActive || currentTextureType_ != 1) {
        dirty_ = true;
    }
}

void MessageHintRenderer::clearOperationHint() {
    std::lock_guard<std::mutex> lock(hintMutex_);
    protectedOperationHint_ = false;
    operationHintActive_ = false;
    if (currentTextureType_ == 1) {
        currentTextureType_ = 0;
    }
    dirty_ = true;
}

bool MessageHintRenderer::isOperationHintActive() const {
    std::lock_guard<std::mutex> lock(hintMutex_);
    return operationHintActive_;
}

bool MessageHintRenderer::hasActiveHint() const {
    std::lock_guard<std::mutex> lock(hintMutex_);
    return playlistHintVisible_ || operationHintActive_;
}

bool MessageHintRenderer::needsTextureUpdate() const {
    if (hasActiveHint() && dirty_) return true;
    // 有待上传的异步光栅化结果时，也需要调用 updateTextures 来消费
    std::lock_guard<std::mutex> lk(uploadMutex_);
    return !pendingUploads_.empty();
}

bool MessageHintRenderer::isPlaylistHintVisible() const {
    std::lock_guard<std::mutex> lock(hintMutex_);
    return playlistHintVisible_;
}

void MessageHintRenderer::rasterizeAsync(const std::string& text, const std::string& cacheKey, float fontSize) {
    if (text.empty() || text.size() > kMaxHintTextBytes) return;
    // 已在缓存或已在待上传队列中则跳过
    if (textCache_.count(cacheKey)) return;
    {
        std::lock_guard<std::mutex> lk(uploadMutex_);
        for (const auto& p : pendingUploads_) if (p.cacheKey == cacheKey) return;
    }
    PendingUpload upload;
    upload.cacheKey = cacheKey;
    upload.text = text;
    if (!vtoBridgePtr_ || !vtoBridgePtr_->rasterizeText(text, fontSize,
                               1.0f, 1.0f, 1.0f, 1.0f,
                               upload.rgba, upload.width, upload.height)) {
        return;
    }
    if (!isValidHintBitmap(upload.width, upload.height, upload.rgba.size())) {
        LOG_WARN("[MHR] rasterize invalid text bitmap: text='%s' size=%dx%d bytes=%zu",
                 text.c_str(), upload.width, upload.height, upload.rgba.size());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(uploadMutex_);
        for (const auto& p : pendingUploads_) if (p.cacheKey == cacheKey) return;
        pendingUploads_.push_back(std::move(upload));
    }
}

void MessageHintRenderer::insertTextCache(VulkanRenderer* renderer, const std::string& key, CachedTextEntry entry) {
    // 已存在则直接覆盖（不重复计入顺序）
    if (textCache_.count(key)) {
        if (renderer && textCache_[key].textureId != 0) {
            renderer->requestDestroyTexture(textCache_[key].textureId, 3);
        }
        textCache_[key] = entry;
        return;
    }
    // 超限：淘汰最旧的一条
    if (static_cast<int>(textCacheOrder_.size()) >= kTextCacheMax) {
        const std::string& oldest = textCacheOrder_.front();
        auto it = textCache_.find(oldest);
        if (it != textCache_.end()) {
            if (renderer && it->second.textureId != 0) {
                renderer->requestDestroyTexture(it->second.textureId, 3);
            }
            textCache_.erase(it);
        }
        textCacheOrder_.erase(textCacheOrder_.begin());
    }
    textCache_[key] = entry;
    textCacheOrder_.push_back(key);
}

bool MessageHintRenderer::hasPlaylistItems() const {
    std::lock_guard<std::mutex> lock(hintMutex_);
    return !playlistItems_.empty();
}

void MessageHintRenderer::preloadHintIcons(VulkanRenderer* renderer) {
    if (!initialized_ || !renderer || renderer->isRenderPassStarted()) return;
    static const HintType kAllTypes[] = {
        HintType::PLAY, HintType::PAUSE, HintType::NEXT, HintType::PREV,
        HintType::VOLUME_UP, HintType::VOLUME_DOWN, HintType::MUTE, HintType::UNMUTE,
        HintType::AUDIO_TRACK, HintType::BACKING_TRACK
    };
    for (HintType t : kAllTypes) {
        IconTextureInfo* iconInfo = nullptr;
        switch (t) {
        case HintType::PLAY:          iconInfo = &playIcon_; break;
        case HintType::PAUSE:         iconInfo = &pauseIcon_; break;
        case HintType::NEXT:          iconInfo = &nextIcon_; break;
        case HintType::PREV:          iconInfo = &prevIcon_; break;
        case HintType::VOLUME_UP:     iconInfo = &volumeUpIcon_; break;
        case HintType::VOLUME_DOWN:   iconInfo = &volumeDownIcon_; break;
        case HintType::MUTE:          iconInfo = &muteIcon_; break;
        case HintType::UNMUTE:        iconInfo = &unmuteIcon_; break;
        case HintType::AUDIO_TRACK:   iconInfo = &audioTrackIcon_; break;
        case HintType::BACKING_TRACK: iconInfo = &backingTrackIcon_; break;
        default: break;
        }
        if (!iconInfo || iconInfo->tried) continue;
        iconInfo->tried = true;
        const char* fileName = getIconFileForType(t);
        if (!fileName || RES_DIR.empty()) continue;
        std::string path = hsvj::FileUtils::normalizePath(RES_DIR + std::string(fileName));
        if (!hsvj::FileUtils::exists(path)) continue;
        int w = 0, h = 0, n = 0;
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!data) continue;
        uint32_t texId = renderer->allocateTextureId();
        size_t sz = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        bool ok = (sz <= 2u * 1024u * 1024u)
            ? renderer->createTextureFromRGBAStaged(data, w, h, texId)
            : renderer->createTextureFromRGBADirect(data, w, h, texId);
        stbi_image_free(data);
        if (ok) {
            iconInfo->textureId = texId;
            iconInfo->width = w;
            iconInfo->height = h;
            iconInfo->loaded = true;
        }
    }
}

uint32_t MessageHintRenderer::getTextureId() const {
    return (currentTextureType_ != 0 && vtoBridgePtr_) ? vtoBridgePtr_->getTextureId() : 0;
}

bool MessageHintRenderer::shouldHidePermanentHint(HintType newType) const {
    // 这些操作应隐藏静音提示
    if (currentOperationHint_.type == HintType::MUTE) {
        switch (newType) {
            case HintType::PLAY:
            case HintType::NEXT:
            case HintType::PREV:
            case HintType::UNMUTE:
            case HintType::VOLUME_UP:
            case HintType::VOLUME_DOWN:
                return true;
            default:
                return false;
        }
    }
    // 这些操作应隐藏暂停提示
    if (currentOperationHint_.type == HintType::PAUSE) {
        switch (newType) {
            case HintType::PLAY:
            case HintType::NEXT:
            case HintType::PREV:
                return true;
            default:
                return false;
        }
    }
    return false;
}

} // 命名空间 hsvj
