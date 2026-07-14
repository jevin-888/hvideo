/**
 * @file Layer21LibassContext.cpp（文件名）
 * @brief Layer 21 内部独占 libass 单实例上下文（1 Library + 1 渲染器）
 */

#include "lyric/Layer21LibassContext.h"
#include "core/PathConfig.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

#include <cstring>
#include <string>

extern "C" {
#include "ass.h"
}

namespace hsvj {

Layer21LibassContext &Layer21LibassContext::getInstance() {
  static Layer21LibassContext instance;
  return instance;
}

Layer21LibassContext::Layer21LibassContext() {
  library_ = ass_library_init();
  if (!library_) {
    LOG_ERROR("[Lyric] libass 初始化失败 ass_library_init");
    return;
  }
  ass_set_extract_fonts(library_, 1);

  renderer_ = ass_renderer_init(library_);
  if (!renderer_) {
    LOG_ERROR("[Lyric] libass 初始化失败 ass_renderer_init");
    ass_library_done(library_);
    library_ = nullptr;
    return;
  }

  ass_set_cache_limits(renderer_, 1000, 8);
  ass_set_pixel_aspect(renderer_, 1.0);
  ass_set_hinting(renderer_, ASS_HINTING_LIGHT);
  ass_set_fonts(renderer_, nullptr, "sans-serif", ASS_FONTPROVIDER_NONE, nullptr,
                1);

}

Layer21LibassContext::~Layer21LibassContext() {
  fontDataBuffers_.clear();
  if (renderer_) {
    ass_renderer_done(renderer_);
    renderer_ = nullptr;
  }
  if (library_) {
    ass_library_done(library_);
    library_ = nullptr;
  }
}

void Layer21LibassContext::setFrameSize(int width, int height) {
  if (!renderer_) {
    return;
  }
  // 跳过相同尺寸：ass_set_frame_size 会使 libass 字形缓存失效，每帧调用会导致严重卡顿
  if (width == lastFrameWidth_ && height == lastFrameHeight_) {
    return;
  }
  lastFrameWidth_ = width;
  lastFrameHeight_ = height;
  ass_set_storage_size(renderer_, width, height);
  ass_set_frame_size(renderer_, width, height);
}

bool Layer21LibassContext::ensureAppFontsLoaded(const std::string &fontDir,
                                                const std::string &appFontsDir) {
  if (fontsLoaded_ || !library_ || !renderer_) {
    return fontsLoaded_;
  }

  std::string ttfDir = appFontsDir.empty() ? fontDir : appFontsDir;
  if (ttfDir.empty()) {
    ttfDir = FONT_DIR;
  }
  if (!ttfDir.empty() && ttfDir.back() == '/') {
    ttfDir.pop_back();
  }

  // 仅加载前 2 个字体，避免 ttf 目录大量字体
  constexpr int kMaxFontsToLoad = 2;
  std::vector<std::string> fontFiles = FileUtils::listFiles(ttfDir, ".ttf");
  std::string firstLoadedPath;
  int loadedCount = 0;
  for (const std::string &path : fontFiles) {
    if (loadedCount >= kMaxFontsToLoad) {
      break;
    }
    if (FileUtils::getExtension(path) != "ttf") {
      continue;
    }
    std::vector<char> data = FileUtils::readBinaryFile(path);
    if (data.empty()) {
      continue;
    }
    size_t size = data.size();
    if (size >= 100 * 1024 * 1024u) {
      continue;
    }
    fontDataBuffers_.push_back(std::move(data));
    char *ptr = fontDataBuffers_.back().data();
    std::string fontName = FileUtils::getFilename(path);
    size_t dot = fontName.rfind('.');
    if (dot != std::string::npos) {
      fontName = fontName.substr(0, dot);
    }
    ass_add_font(library_, fontName.c_str(), ptr, static_cast<int>(size));
    if (firstLoadedPath.empty()) {
      ass_add_font(library_, "lyric", ptr, static_cast<int>(size));
      ass_add_font(library_, "Lyric", ptr, static_cast<int>(size));
      ass_add_font(library_, "Default", ptr, static_cast<int>(size));
      firstLoadedPath = path;
    }
    loadedCount++;
  }

  if (firstLoadedPath.empty()) {
    LOG_ERROR("[Lyric] 未在 ttf 目录加载到任何字体 path=%s，请确保目录存在且包含 .ttf 文件", ttfDir.c_str());
    return false;
  }
  ass_set_fonts(renderer_, firstLoadedPath.c_str(), "lyric",
                ASS_FONTPROVIDER_NONE, nullptr, 1);
  fontsLoaded_ = true;
  LOG_INFO("[Lyric] 字体加载 %d/%zu 个 path=%s", loadedCount,
           fontFiles.size(), firstLoadedPath.c_str());
  return true;
}

} // 命名空间 hsvj
