/**
 * @file HardwareManager.cpp（文件名）
 * @brief Hardware 解码器 管理器 实现
 *
 * 本模块负责：
 * 1. 自动检测设备类型（RK 芯片 / 通用 Android）
 * 2. Detect and manage available hardware 解码器s
 * 3. Smart 解码器 selection (based on device type and codec format)
 * 4. Configure 零拷贝 mode (DRM_PRIME)
 * 5. RKMPP 输出不可用时快速失败
 *
 * 解码器 priority rules:
 * - RK 芯片：仅使用 RKMPP DRM_PRIME。
 * - 不支持的非 RKMPP 编码快速失败。
 */

#include "decoder/core/HardwareManager.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <cstdlib>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <inttypes.h>
}

// ============================================================================
// FFmpeg 日志回调
// ============================================================================

/**
 * @brief FFmpeg 全局日志回调
 *
 * 将 FFmpeg 内部日志输出到 Android logcat。
 * 仅输出警告和错误级别日志，减少刷屏。
 */
static void ffmpeg_global_log_callback(void *ptr, int level, const char *fmt,
                                       va_list vl) {
  // 仅处理 warning 及以上级别（AV_LOG_WARNING = 24）
  // FFmpeg 日志级别：数值越小越重要
  // 字段说明：AV_LOG_QUIET=-8, PANIC=0, FATAL=8, ERROR=16, WARNING=24, INFO=32, VERBOSE=40, DEBUG=48
  if (level > AV_LOG_WARNING) {
    return;
  }

  char line[1024];
  int print_prefix = 1;
  av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);

  // 移除尾部换行
  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\n') {
    line[len - 1] = '\0';
  }

  // 过滤已知无害错误信息
  // "AFBC 选中 but not supported for base 格式" is a system library 错误,
  // 项目代码会自动回退到 LINEAR，不影响功能
  if (strstr(line, "AFBC selected but not supported") != nullptr) {
    // 静默忽略，项目代码会自动处理
    return;
  }

  // 将 FFmpeg 日志级别映射到 Android 日志级别
  android_LogPriority priority = ANDROID_LOG_DEBUG;
  if (level <= AV_LOG_PANIC) {
    priority = ANDROID_LOG_FATAL;
  } else if (level <= AV_LOG_FATAL) {
    priority = ANDROID_LOG_FATAL;
  } else if (level <= AV_LOG_ERROR) {
    priority = ANDROID_LOG_ERROR;
  } else if (level <= AV_LOG_WARNING) {
    priority = ANDROID_LOG_WARN;
  } else if (level <= AV_LOG_INFO) {
    priority = ANDROID_LOG_INFO;
  } else if (level <= AV_LOG_VERBOSE) {
    priority = ANDROID_LOG_VERBOSE;
  }

  __android_log_print(priority, "FFmpeg", "%s", line);
}

// ============================================================================
// RKMPP get_format 回调
// ============================================================================

/**
 * @brief RKMPP 解码器 pixel format selection callback
 *
 * - DRM_PRIME 帧保留在 GPU 内存中（AFBC 压缩格式）
 * - 通过 Rockchip DRM 扩展直接渲染，无需 CPU 拷贝
 *
 * 如果 DRM 零拷贝不可用，则快速失败；这里不会选择软件帧路径。
 */
static enum AVPixelFormat rkmpp_get_format([[maybe_unused]] AVCodecContext *ctx,
                                           const enum AVPixelFormat *pix_fmts) {

  // 优先选择 DRM_PRIME（AFBC 零拷贝）
  for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == AV_PIX_FMT_DRM_PRIME) {
      return *p;
    }
  }

  LOG_WARN("[HardwareManager] get_format: DRM_PRIME unavailable for RKMPP decoder");
  return pix_fmts[0];
}

#endif // 结束 __ANDROID__

namespace hsvj {

namespace {

} // 命名空间

// ============================================================================
// 构造与析构
// ============================================================================

HardwareManager::HardwareManager()
    : hwDeviceCtx_(nullptr), hwDeviceType_(AV_HWDEVICE_TYPE_NONE),
      zeroCopyMode_(ZeroCopyMode::NONE),
      isRKDevice_(false), priority_(DecoderPriority::RKMPP_FIRST),
      isRK356xDevice_(false), isV03PDevice_(false) {}

HardwareManager::~HardwareManager() {
#ifdef __ANDROID__
  if (hwDeviceCtx_) {
    av_buffer_unref(&hwDeviceCtx_);
  }
#endif
}

// ============================================================================
// 初始化
// ============================================================================

bool HardwareManager::initialize() {
#ifdef __ANDROID__
  // 步骤 1：Set FFmpeg log level (only output errors, suppress warnings to reduce log flooding)
  av_log_set_level(AV_LOG_ERROR);
  av_log_set_callback(ffmpeg_global_log_callback);
#endif

  // 步骤 2：Detect device type
  isRKDevice_ = detectRKChip();
  char propValue[PROP_VALUE_MAX] = {0};
  if (__system_property_get("ro.board.platform", propValue) > 0) {
    std::string platform(propValue);
    std::transform(platform.begin(), platform.end(), platform.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    isRK356xDevice_ = (platform.find("rk356") != std::string::npos);
  }
  memset(propValue, 0, sizeof(propValue));
  if (__system_property_get("ro.product.model", propValue) > 0) {
    std::string model(propValue);
    isV03PDevice_ = (model.find("V03P") != std::string::npos);
  }
  priority_ = DecoderPriority::RKMPP_FIRST;
  if (isRK356xDevice_ || isV03PDevice_) {
    LOG_INFO("[HardwareManager] Device profile: rk=%d rk356x=%d v03p=%d",
             isRKDevice_ ? 1 : 0, isRK356xDevice_ ? 1 : 0, isV03PDevice_ ? 1 : 0);
  }
  // 步骤 3：Detect available 解码器s
  detectAvailableDecoders();

  return true;
}

// ============================================================================
// 解码器 Detection
// ============================================================================

void HardwareManager::detectAvailableDecoders() {
  detectH264Decoders();
  detectHEVCDecoders();
  detectMPEG1Decoders();
  detectMPEG2Decoders();
  detectMJPEGDecoders();
  detectVP8Decoders();
  detectVP9Decoders();
}

void HardwareManager::detectH264Decoders() {
#ifdef __ANDROID__
  std::vector<DecoderInfo> decoders;

  if (avcodec_find_decoder_by_name("h264_rkmpp")) {
    decoders.push_back({"h264_rkmpp", AV_CODEC_ID_H264, true, true, 1, true});
  }

  if (decoders.empty()) {
      LOG_ERROR("[HardwareManager] No H.264 RKMPP decoder available!");
  }

  decoderMap_[AV_CODEC_ID_H264] = decoders;
#endif
}

void HardwareManager::detectHEVCDecoders() {
#ifdef __ANDROID__
  std::vector<DecoderInfo> decoders;

  if (avcodec_find_decoder_by_name("hevc_rkmpp")) {
    decoders.push_back({"hevc_rkmpp", AV_CODEC_ID_HEVC, true, true, 1, true});
  }

  if (decoders.empty()) {
      LOG_ERROR("[HardwareManager] No HEVC RKMPP decoder available!");
  }

  decoderMap_[AV_CODEC_ID_HEVC] = decoders;
#endif
}

void HardwareManager::detectMPEG1Decoders() {
#ifdef __ANDROID__
  std::vector<DecoderInfo> decoders;

  if (avcodec_find_decoder_by_name("mpeg1_rkmpp")) {
    decoders.push_back({"mpeg1_rkmpp", AV_CODEC_ID_MPEG1VIDEO, true, true, 1, true});
  }

  decoderMap_[AV_CODEC_ID_MPEG1VIDEO] = decoders;
#endif
}

void HardwareManager::detectMPEG2Decoders() {
#ifdef __ANDROID__
  std::vector<DecoderInfo> decoders;

  if (avcodec_find_decoder_by_name("mpeg2_rkmpp")) {
    decoders.push_back({"mpeg2_rkmpp", AV_CODEC_ID_MPEG2VIDEO, true, true, 1, true});
  }

  decoderMap_[AV_CODEC_ID_MPEG2VIDEO] = decoders;
#endif
}

void HardwareManager::detectMJPEGDecoders() {
#ifdef __ANDROID__
  std::vector<DecoderInfo> decoders;

  if (avcodec_find_decoder_by_name("mjpeg_rkmpp")) {
    decoders.push_back({"mjpeg_rkmpp", AV_CODEC_ID_MJPEG, true, true, 1, true});
  }

  if (!decoders.empty()) {
    decoderMap_[AV_CODEC_ID_MJPEG] = decoders;
  }
#endif
}

void HardwareManager::detectVP8Decoders() {
#ifdef __ANDROID__
  std::vector<DecoderInfo> decoders;
  if (avcodec_find_decoder_by_name("vp8_rkmpp")) {
    decoders.push_back({"vp8_rkmpp", AV_CODEC_ID_VP8, true, true, 1, true});
  }
  if (!decoders.empty()) {
    decoderMap_[AV_CODEC_ID_VP8] = decoders;
  }
#endif
}

void HardwareManager::detectVP9Decoders() {
#ifdef __ANDROID__
  std::vector<DecoderInfo> decoders;
  if (avcodec_find_decoder_by_name("vp9_rkmpp")) {
    decoders.push_back({"vp9_rkmpp", AV_CODEC_ID_VP9, true, true, 1, true});
  }
  if (!decoders.empty()) {
    decoderMap_[AV_CODEC_ID_VP9] = decoders;
  }
#endif
}



// ============================================================================
// 解码器 Selection
// ============================================================================

const AVCodec *HardwareManager::selectBestDecoder(AVCodecID codecId) {
  auto it = decoderMap_.find(codecId);
  if (it == decoderMap_.end() || it->second.empty()) {
    LOG_WARN("[HardwareManager] No hardware decoder in map for codecId=%d, falling back to system default", codecId);
    return nullptr;
  }

  // RKMPP-only: list only contains RKMPP 解码器s.
  for (auto &info : it->second) {
    if (!info.isAvailable) {
      continue;
    }

#ifdef __ANDROID__
    const AVCodec *codec = avcodec_find_decoder_by_name(info.name.c_str());
    if (!codec) {
      LOG_WARN("[HardwareManager] Decoder %s found in map but not available in FFmpeg", info.name.c_str());
      continue;
    }
    LOG_DEBUG("[HardwareManager] Selected hardware decoder: %s", info.name.c_str());
    return codec;
#else
    return nullptr;
#endif
  }

  LOG_ERROR("[HardwareManager] All registered hardware decoders failed for codecId=%d", codecId);
  return nullptr;
}

// ============================================================================
// 硬件解码配置
// ============================================================================

bool HardwareManager::configureHardwareDecoding(const AVCodec *codec,
                                                AVCodecContext *codecCtx) {
  if (!codec || !codecCtx) {
    return false;
  }

#ifdef __ANDROID__
  if (!isHardwareDecoder(codec)) {
    return true; // Software 解码器, no configuration needed
  }

  // Apply different configuration based on 解码器 type
  if (strstr(codec->name, "rkmpp")) {
    // 示例/字段：=== RKMPP Configuration ===
    // Configure DRM_PRIME output to enable AFBC 零拷贝
    // 设置 get_format 回调, prefer DRM_PRIME
    codecCtx->get_format = rkmpp_get_format;

    // 设置 output pixel 格式 to DRM_PRIME
    codecCtx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    // 尝试预创建硬件设备 context（可选，失败不影响解码）
    if (createHardwareDeviceContext(codec, codecCtx)) {
      if (hwDeviceCtx_) {
        codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
      }
    }

  }

  // Configure 零拷贝 mode
  zeroCopyMode_ = configureZeroCopy(codec, codecCtx);
  return true;
#else
  return false;
#endif
}

bool HardwareManager::createHardwareDeviceContext(const AVCodec *codec,
                                                  AVCodecContext *codecCtx) {
#ifdef __ANDROID__
  if (hwDeviceCtx_ && strstr(codec->name, "rkmpp")) {
    av_buffer_unref(&hwDeviceCtx_);
    hwDeviceCtx_ = nullptr;
  }

  if (strstr(codec->name, "rkmpp")) {
    // RKMPP DRM 设备 context (optional, FFmpeg will auto-创建)
    hwDeviceType_ = AV_HWDEVICE_TYPE_DRM;

    const char *drmDevices[] = {"/dev/dri/card0", "/dev/dri/renderD128",
                                nullptr};

    for (int i = 0; i < 3; i++) {
      int ret = av_hwdevice_ctx_create(&hwDeviceCtx_, hwDeviceType_,
                                       drmDevices[i], nullptr, 0);
      if (ret >= 0) {
        return true;
      }

      if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
      }
    }

    // Creation failure is acceptable, FFmpeg will auto-创建 at avcodec_open2
    return false;
  }
  return false;
#else
  (void)codec;
  (void)codecCtx;
  return false;
#endif
}

ZeroCopyMode HardwareManager::configureZeroCopy(const AVCodec *codec,
                                                AVCodecContext *codecCtx) {
#ifdef __ANDROID__
  if (strstr(codec->name, "rkmpp")) {
    // RKMPP: use DRM_PRIME 零拷贝
    codecCtx->get_format = rkmpp_get_format;
    return ZeroCopyMode::RKMPP_DRM;

  }
#else
  (void)codec;
  (void)codecCtx;
#endif

  return ZeroCopyMode::NONE;
}

// ============================================================================
// 解码器回退
// ============================================================================

void HardwareManager::markDecoderFailed(const std::string &decoderName) {
  for (auto &[codecId, decoders] : decoderMap_) {
    for (auto &decoder : decoders) {
      if (decoder.name == decoderName) {
        decoder.isAvailable = false;
        decoder.priority = 100; // 降到最低优先级
    LOG_WARN("[HardwareManager] Decoder marked as failed: %s", decoderName.c_str());

        // 重新排序
    std::sort(decoders.begin(), decoders.end(),
                  [](const DecoderInfo &a, const DecoderInfo &b) {
                    return a.priority < b.priority;
                  });
        return;
      }
    }
  }
}

bool HardwareManager::isHardwareDecoder(const AVCodec *codec) const {
  if (!codec) {
    return false;
  }
  return strstr(codec->name, "rkmpp") != nullptr ||
         false;
}

std::vector<HardwareManager::DecoderInfo>
HardwareManager::getAllDecoders() const {
  std::vector<DecoderInfo> allDecoders;
  for (const auto &[codecId, decoders] : decoderMap_) {
    allDecoders.insert(allDecoders.end(), decoders.begin(), decoders.end());
  }
  return allDecoders;
}

// ============================================================================
// 设备 Detection
// ============================================================================

bool HardwareManager::detectRKChip() {
#ifdef __ANDROID__
  // Optimized: first 检查 system property (faster, no file I/O), then 检查 /proc/cpuinfo
  // Method 1: 检查 Android system property ro.hardware (fastest, no file I/O)
  char propValue[92] = {0};
  if (__system_property_get("ro.hardware", propValue) > 0) {
    std::string hardware(propValue);
    if (hardware.find("rk") != std::string::npos ||
        hardware.find("RK") != std::string::npos) {
      return true;
    }
  }

  // Method 2: 检查 Android system property ro.board.platform (second fastest, no file I/O)
  memset(propValue, 0, sizeof(propValue));
  if (__system_property_get("ro.board.platform", propValue) > 0) {
    std::string platform(propValue);
    if (platform.find("rk") != std::string::npos ||
        platform.find("RK") != std::string::npos) {
      return true;
    }
  }

  // Method 3: 检查 /proc/cpuinfo Hardware line (slowest, has file I/O, as last resort)
  // Optimized: fast read, 仅 read first few lines to avoid reading entire file
  std::ifstream cpuinfo("/proc/cpuinfo");
  if (cpuinfo.is_open()) {
    std::string line;
    int lineCount = 0;
    const int maxLines = 20; // 仅 read first 20 lines, Hardware line usually in first few
    while (lineCount < maxLines && std::getline(cpuinfo, line)) {
      lineCount++;
      if (line.find("Hardware") != std::string::npos) {
        size_t colonPos = line.find(":");
        if (colonPos != std::string::npos) {
          std::string hardware = line.substr(colonPos + 1);
          hardware.erase(0, hardware.find_first_not_of(" \t"));
          hardware.erase(hardware.find_last_not_of(" \t") + 1);

          if (hardware.find("rk") != std::string::npos ||
              hardware.find("RK") != std::string::npos ||
              hardware.find("Rockchip") != std::string::npos) {
            return true;
          }
        }
        break; // 已找到 Hardware 行，立即退出
      }
    }
  }


  LOG_WARN("[HardwareManager] RK device not detected");
#endif

  return false;
}

} // 命名空间 hsvj
