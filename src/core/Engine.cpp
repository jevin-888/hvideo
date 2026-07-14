/**
 * @file Engine.cpp（文件名）
 * @brief 引擎核心类实现：主流程、图层创建与授权
 *
 * 职责划分：
 * - 目录创建、资源清理、区域渲染器重初始化 → Engine_Init.cpp
 * - 渲染（renderFrame、renderLayersToCanvas、renderSliceItem）→ Engine_Render.cpp
 * - 播放列表与默认播放、提示层、下一首、音频效果回调 → Engine_播放列表.cpp
 * - 本文件：initialize() 主流程、shutdown/run/update、preCreateAuthorizedLayers、
 *   createLayersFromConfig、showLicenseWarning/hideLicenseWarning 等
 */

#include "core/Engine.h"
#include "core/InitProgress.h"
#include "core/LayerDefinitions.h"
#include "lyric/SharedLibassHolder.h"
#include "audio/AudioPlayerManager.h"
#include "audio/AudioProcessor.h"
#include "core/LicenseManager.h"
#include "core/PathConfig.h"
#include "core/PeripheralManager.h"
#include "core/SystemConfig.h"
#include "utils/SliceConfigJson.h"
#include "database/PlaylistDatabase.h"
#include "database/VodDatabase.h"
#include "decoder/VideoDecoder.h"
#include "decoder/core/DecoderCore.h"
#include "effect/EffectManager.h"
#include "layer/LayerImage.h"
#include "layer/LayerMirror.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "network/Dmx512Receiver.h"
#include "network/Dmx512ChannelHandler.h"
#include "network/HttpServer.h"
#include "network/NetworkManager.h"
#include "network/OnlineVodWsClient.h"
#include "network/OnlineVodHttpSyncClient.h"
#include "network/CloudSyncService.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "vod/LocalVodDatabase.h"
#include "vod/LocalVodManager.h"
#include "vod/LocalVodPlayer.h"
#include "vod/LocalSongDatabase.h"
#include "vod/LocalSongFileScanner.h"
class CloudSyncService;
#include "renderer/CaptureRenderer.h"
#include "renderer/RegionRotationRenderer.h"
#include "utils/FileUtils.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"

// 新的图层初始化器框架
#ifdef USE_NEW_LAYER_INIT
#include "layer/initializer/LayerInitializerFactory.h"
#include "layer/initializer/LayerInitializer.h"
#endif
#include "utils/MediaUtils.h"
#include "utils/MemoryMonitor.h"
#include "utils/Version.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <json/json.h>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <future>
#include <thread>
#include <utility>


#include "utils/SystemUtils.h"
#ifdef __ANDROID__
#include "renderer/VulkanRenderer.h"
#include <android/native_window.h>
#include <jni.h>
#include <sys/stat.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <unistd.h>
}
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/termios.h> // 用于 termios2
#include <sys/ioctl.h>
#include <termios.h>

#endif
#include <set>
#include <unordered_set>

// JNI Java VM 指针 (由 jni_interface.cpp 中的 g_jvm 提供)
extern JavaVM* g_jvm;

namespace hsvj {

double Engine::getLastAsyncPresentMs() const {
  return renderer_ ? renderer_->getLastAsyncPresentMs() : 0.0;
}

double Engine::getLastAsyncAcquireMs() const {
  return renderer_ ? renderer_->getLastAsyncAcquireMs() : 0.0;
}

double Engine::getLastAsyncAcquireFenceMs() const {
  return renderer_ ? renderer_->getLastAsyncAcquireFenceMs() : 0.0;
}

long long Engine::getSwapchainNoImageSkipCount() const {
  return renderer_ ? renderer_->getSwapchainNoImageSkipCount() : 0;
}

namespace {
// Engine 实例的 Java 对象引用 (用于回调)
static jobject g_engine_java_object = nullptr;

int localVodPlayStateForSync(LayerVideo* videoLayer) {
  if (!videoLayer) return 0;
  switch (videoLayer->getState()) {
    case LayerVideo::PlayState::PLAYING:
      return 1;
    case LayerVideo::PlayState::PAUSED:
      return 2;
    case LayerVideo::PlayState::STOPPED:
    default:
      return 0;
  }
}

/**
 * 向 Java 层发送初始化进度
 * 节流：除 0%、末尾阶段外，至少间隔 PROGRESS_THROTTLE_MS 才发送，减轻主线程负担、避免 Skipped frames / OOM 前 thrashing
 * @param stage 阶段 (0-4)
 * @param step 步骤
 * @param message 描述信息
 * @param percent 进度百分比 (0-100)
 */
void sendProgress(int stage, int step, const char* message, int percent) {
    if (!g_jvm || !g_engine_java_object) {
        return;  // JNI 未初始化或不支持进度回调
    }
    static constexpr int PROGRESS_THROTTLE_MS = 100;
    static std::chrono::steady_clock::time_point s_lastSendTime;
    static int s_lastSentPercent = -1;
    static std::mutex s_progressThrottleMutex;

    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_progressThrottleMutex);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastSendTime).count();
        bool forceSend = (percent == 0 || percent >= 90 || percent == 100);
        bool throttleOk = (elapsed >= PROGRESS_THROTTLE_MS) || (s_lastSentPercent < 0);
        if (!forceSend && !throttleOk) {
            return;
        }
        s_lastSendTime = now;
        s_lastSentPercent = percent;
    }

    JNIEnv* env = nullptr;
    bool attached = false;

    // 尝试获取当前线程的 JNIEnv
    jint get_env_result = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);

    if (get_env_result == JNI_EDETACHED) {
        // 线程未附加，需要附加
        if (g_jvm->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        } else {
            LOG_WARN("sendProgress: AttachCurrentThread 失败");
            return;
        }
    } else if (get_env_result != JNI_OK) {
        LOG_WARN("sendProgress: GetEnv 失败：%d", get_env_result);
        return;
    }

    if (env) {
        // 获取方法 ID
        jclass clazz = env->GetObjectClass(g_engine_java_object);
        jmethodID methodId = env->GetMethodID(clazz, "onInitializationProgress",
                                               "(IIILjava/lang/String;)V");

        if (methodId) {
            // 创建 Java String
            jstring msg = env->NewStringUTF(message);

            // 调用 Java 方法 (注意参数顺序: stage, step, percent, message)
            env->CallVoidMethod(g_engine_java_object, methodId, stage, step, percent, msg);

            // 清理局部引用
            env->DeleteLocalRef(msg);
            env->DeleteLocalRef(clazz);
        } else {
            LOG_WARN("sendProgress: 找不到 onInitializationProgress 方法");
        }
    }

    // 如果是刚刚附加的线程，需要分离
    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

/**
 * 通知 Java 层初始化完成
 */
void sendComplete() {
    if (!g_jvm || !g_engine_java_object) {
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;

    jint get_env_result = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);

    if (get_env_result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        } else {
            return;
        }
    } else if (get_env_result != JNI_OK) {
        return;
    }

    if (env) {
        jclass clazz = env->GetObjectClass(g_engine_java_object);
        jmethodID methodId = env->GetMethodID(clazz, "onInitializationComplete", "()V");

        if (methodId) {
            env->CallVoidMethod(g_engine_java_object, methodId);
            env->DeleteLocalRef(clazz);
        }
    }

    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

/**
 * 通知 Java 层初始化失败
 */
void sendError(const char* errorMessage) {
    if (!g_jvm || !g_engine_java_object) {
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;

    jint get_env_result = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);

    if (get_env_result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        } else {
            return;
        }
    } else if (get_env_result != JNI_OK) {
        return;
    }

    if (env) {
        jclass clazz = env->GetObjectClass(g_engine_java_object);
        jmethodID methodId = env->GetMethodID(clazz, "onInitializationError",
                                               "(Ljava/lang/String;)V");

        if (methodId) {
            jstring msg = env->NewStringUTF(errorMessage ? errorMessage : "Unknown initialization error");
            env->CallVoidMethod(g_engine_java_object, methodId, msg);
            env->DeleteLocalRef(msg);
        }
        env->DeleteLocalRef(clazz);
    }

    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

/**
 * 设置 Engine 的 Java 对象引用 (由 JNI 层调用)
 */
extern "C" void setEngineJavaObject(jobject obj) {
    if (g_engine_java_object && g_jvm) {
        // 释放旧的引用（线程未附加时先附加）
        JNIEnv* env = nullptr;
        bool attached = false;

        jint getEnvResult = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
        if (getEnvResult == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            } else {
                LOG_WARN("setEngineJavaObject: AttachCurrentThread 失败，旧 GlobalRef 延后释放");
                env = nullptr;
            }
        } else if (getEnvResult != JNI_OK) {
            LOG_WARN("setEngineJavaObject: GetEnv 失败：%d", getEnvResult);
            env = nullptr;
        }

        if (env) {
            env->DeleteGlobalRef(g_engine_java_object);
        }

        if (attached) {
            g_jvm->DetachCurrentThread();
        }
    }
    if (obj && g_jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        jint getEnvResult = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
        if (getEnvResult == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            } else {
                LOG_WARN("setEngineJavaObject: AttachCurrentThread 失败，新 GlobalRef 创建失败");
                env = nullptr;
            }
        } else if (getEnvResult != JNI_OK) {
            LOG_WARN("setEngineJavaObject: GetEnv 失败：%d", getEnvResult);
            env = nullptr;
        }
        g_engine_java_object = env ? env->NewGlobalRef(obj) : nullptr;
        if (attached) {
            g_jvm->DetachCurrentThread();
        }
    } else {
        g_engine_java_object = nullptr;
    }
}

// 设备初始化上报：向授权服务 POST /api/device/register。
// 调用方决定是否放到后台线程执行。
void reportDeviceToLicenseServer(const std::string& baseUrl, int64_t expiry, int64_t start) {
  std::string trimmedBaseUrl = baseUrl;
  while (!trimmedBaseUrl.empty() && (trimmedBaseUrl.back() == '/' || trimmedBaseUrl.back() == ' ')) trimmedBaseUrl.pop_back();
  if (trimmedBaseUrl.empty()) {
    LOG_WARN("[Step 4.9] 跳过设备上报：license server url 为空");
    return;
  }

  // 必须确保以 http:// 开头，HttpClient 才能识别
  std::string finalUrl = trimmedBaseUrl;
  if (finalUrl.compare(0, 7, "http://") != 0 && finalUrl.compare(0, 8, "https://") != 0) {
    finalUrl = "http://" + finalUrl;
  }

  std::string url = finalUrl + "/api/device/register";
  int64_t now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  std::string fingerprint = SystemUtils::generateDeviceFingerprint();
  Json::Value root;
  root["version"] = 1;
  root["export_time"] = static_cast<Json::Int64>(now);
  Json::Value device;
  device["model"] = SystemUtils::getHardwareModel();
  device["serial"] = SystemUtils::getHardwareSerial();
  device["cpu_serial"] = SystemUtils::getCpuSerial();
  device["storage_serial"] = SystemUtils::getStorageSerial();
  device["mac"] = SystemUtils::getMacAddress();
  device["fingerprint"] = fingerprint;
  if (expiry > 0) device["license_expiry"] = static_cast<Json::Int64>(expiry);
  if (start > 0) device["license_start"] = static_cast<Json::Int64>(start);
  root["device"] = device;
  std::string body = JsonUtils::toString(root);
  LOG_INFO("[Step 4.9] 开始设备上报: expiry=%lld start=%lld bodyBytes=%zu",
           (long long)expiry, (long long)start, body.size());
  std::string out = hsvj::httpPostJson(url, body, 15);
  if (!out.empty() && out.find("\"code\":0") != std::string::npos) {
    LOG_INFO("[Step 4.9] 设备上报成功");
  } else {
    LOG_WARN("[Step 4.9] 设备上报失败或未响应: %s", out.empty() ? "(无输出)" : out.substr(0, 120).c_str());
  }
}

std::string trimCopy(std::string value) {
  auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::string stripSurroundingQuotes(std::string value) {
  value = trimCopy(std::move(value));
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return trimCopy(std::move(value));
}

std::string readDebugHotspotValueFromStatusFile(const std::string& key) {
  std::ifstream file("/data/local/tmp/hsvj_hotspot_status");
  if (!file.is_open()) {
    return "";
  }

  const std::string prefix = key + "=";
  std::string line;
  while (std::getline(file, line)) {
    line = trimCopy(line);
    if (startsWith(line, prefix)) {
      return stripSurroundingQuotes(line.substr(prefix.size()));
    }
  }
  return "";
}

constexpr const char* kDebugHotspotSsid = "HVIDEO";

bool isLikelyDebugHotspotInterface(const std::string& name) {
  return name == "wlan0" || startsWith(name, "softap") ||
         startsWith(name, "ap") || name.find("wlan") != std::string::npos;
}

bool isIpAssignedToDebugHotspotInterface(const std::string& ip) {
  if (ip.empty()) {
    return false;
  }

  Json::Value interfaces = SystemUtils::getNetworkInterfaces();
  for (const auto& iface : interfaces) {
    if (!iface.isMember("name") || !iface.isMember("ip")) {
      continue;
    }
    if (iface["ip"].asString() != ip) {
      continue;
    }
    std::string name = iface["name"].asString();
    if (isLikelyDebugHotspotInterface(name)) {
      return true;
    }
  }
  return false;
}

std::string getDebugHotspotSsid() {
  return kDebugHotspotSsid;
}

std::string getDebugHotspotIp() {
  std::string state = readDebugHotspotValueFromStatusFile("state");
  std::string ip = readDebugHotspotValueFromStatusFile("ip");
  if (state == "ready" && isIpAssignedToDebugHotspotInterface(ip)) {
    return ip;
  }

  return "";
}

std::string getInterfaceIp(const std::string& interfaceName) {
  Json::Value interfaces = SystemUtils::getNetworkInterfaces();
  for (const auto& iface : interfaces) {
    if (!iface.isMember("name") || !iface.isMember("ip")) {
      continue;
    }
    if (iface["name"].asString() == interfaceName) {
      return iface["ip"].asString();
    }
  }
  return "";
}

std::string getStartupMobileAccessIp() {
  std::string ip = getDebugHotspotIp();
  if (!ip.empty()) {
    return ip;
  }

  ip = getInterfaceIp("wlan0");
  if (!ip.empty()) {
    return ip;
  }

  return SystemUtils::getLocalIp();
}

constexpr int kStartupQrVersion = 3;
constexpr int kStartupQrSize = 21 + 4 * (kStartupQrVersion - 1);
constexpr int kStartupQrDataCodewords = 44;
constexpr int kStartupQrEccCodewords = 26;
constexpr int kStartupQrImageSize = 360;
using StartupQrGrid = std::array<std::array<bool, kStartupQrSize>, kStartupQrSize>;

struct StartupQrMatrix {
  StartupQrGrid modules{};
  StartupQrGrid function{};
};

struct StartupRegionHintLayout {
  int x = 0;
  int y = 0;
  int width = 1920;
  int height = 1080;
  int qrX = 0;
  int qrY = 0;
  int qrSize = 360;
  int textX = 0;
  int textY = 0;
  int textWidth = 1920;
  int textHeight = 1080;
};

void setStartupQrModule(StartupQrMatrix& qr, int x, int y, bool dark, bool isFunction) {
  if (x < 0 || y < 0 || x >= kStartupQrSize || y >= kStartupQrSize) return;
  qr.modules[static_cast<size_t>(y)][static_cast<size_t>(x)] = dark;
  if (isFunction) {
    qr.function[static_cast<size_t>(y)][static_cast<size_t>(x)] = true;
  }
}

void drawStartupQrFinder(StartupQrMatrix& qr, int left, int top) {
  for (int dy = -1; dy <= 7; ++dy) {
    for (int dx = -1; dx <= 7; ++dx) {
      const int x = left + dx;
      const int y = top + dy;
      if (x < 0 || y < 0 || x >= kStartupQrSize || y >= kStartupQrSize) continue;
      const bool in = dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6;
      const bool dark = in && (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                               (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
      setStartupQrModule(qr, x, y, dark, true);
    }
  }
}

void drawStartupQrAlignment(StartupQrMatrix& qr, int cx, int cy) {
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const int dist = std::max(std::abs(dx), std::abs(dy));
      setStartupQrModule(qr, cx + dx, cy + dy, dist != 1, true);
    }
  }
}

void reserveStartupQrFormat(StartupQrMatrix& qr) {
  for (int i = 0; i <= 8; ++i) {
    if (i != 6) {
      qr.function[8][static_cast<size_t>(i)] = true;
      qr.function[static_cast<size_t>(i)][8] = true;
    }
  }
  for (int i = 0; i < 8; ++i) {
    qr.function[8][static_cast<size_t>(kStartupQrSize - 1 - i)] = true;
    qr.function[static_cast<size_t>(kStartupQrSize - 1 - i)][8] = true;
  }
}

StartupQrMatrix makeStartupQrBaseMatrix() {
  StartupQrMatrix qr;
  drawStartupQrFinder(qr, 0, 0);
  drawStartupQrFinder(qr, kStartupQrSize - 7, 0);
  drawStartupQrFinder(qr, 0, kStartupQrSize - 7);
  drawStartupQrAlignment(qr, 22, 22);
  for (int i = 8; i < kStartupQrSize - 8; ++i) {
    const bool dark = (i % 2) == 0;
    if (!qr.function[6][static_cast<size_t>(i)]) {
      setStartupQrModule(qr, i, 6, dark, true);
    }
    if (!qr.function[static_cast<size_t>(i)][6]) {
      setStartupQrModule(qr, 6, i, dark, true);
    }
  }
  setStartupQrModule(qr, 8, kStartupQrSize - 8, true, true);
  reserveStartupQrFormat(qr);
  return qr;
}

void appendStartupQrBits(std::vector<bool>& bits, int value, int count) {
  for (int i = count - 1; i >= 0; --i) {
    bits.push_back(((value >> i) & 1) != 0);
  }
}

uint8_t startupQrGfMul(uint8_t x, uint8_t y) {
  int z = 0;
  int a = x;
  int b = y;
  while (b != 0) {
    if ((b & 1) != 0) z ^= a;
    a <<= 1;
    if ((a & 0x100) != 0) a ^= 0x11D;
    b >>= 1;
  }
  return static_cast<uint8_t>(z);
}

std::vector<uint8_t> makeStartupQrRsDivisor(int degree) {
  std::vector<uint8_t> result(static_cast<size_t>(degree), 0);
  result[static_cast<size_t>(degree - 1)] = 1;
  uint8_t root = 1;
  for (int i = 0; i < degree; ++i) {
    for (int j = 0; j < degree; ++j) {
      result[static_cast<size_t>(j)] = startupQrGfMul(result[static_cast<size_t>(j)], root);
      if (j + 1 < degree) {
        result[static_cast<size_t>(j)] ^= result[static_cast<size_t>(j + 1)];
      }
    }
    root = startupQrGfMul(root, 0x02);
  }
  return result;
}

std::vector<uint8_t> makeStartupQrRsRemainder(const std::vector<uint8_t>& data,
                                              const std::vector<uint8_t>& divisor) {
  std::vector<uint8_t> result(divisor.size(), 0);
  for (uint8_t value : data) {
    const uint8_t factor = value ^ result[0];
    for (size_t i = 0; i + 1 < result.size(); ++i) {
      result[i] = result[i + 1];
    }
    result.back() = 0;
    for (size_t i = 0; i < result.size(); ++i) {
      result[i] ^= startupQrGfMul(divisor[i], factor);
    }
  }
  return result;
}

std::vector<uint8_t> makeStartupQrCodewords(const std::string& text) {
  if (text.size() > 42) {
    LOG_WARN("启动融合二维码 URL 过长，无法生成 version3 QR: %zu bytes", text.size());
    return {};
  }

  std::vector<bool> bits;
  appendStartupQrBits(bits, 0x4, 4); // byte mode
  appendStartupQrBits(bits, static_cast<int>(text.size()), 8);
  for (unsigned char ch : text) {
    appendStartupQrBits(bits, ch, 8);
  }
  const int dataBitCapacity = kStartupQrDataCodewords * 8;
  const int terminator = std::min(4, dataBitCapacity - static_cast<int>(bits.size()));
  for (int i = 0; i < terminator; ++i) bits.push_back(false);
  while ((bits.size() % 8) != 0) bits.push_back(false);

  std::vector<uint8_t> data;
  data.reserve(kStartupQrDataCodewords);
  for (size_t i = 0; i < bits.size(); i += 8) {
    uint8_t value = 0;
    for (int j = 0; j < 8; ++j) {
      value = static_cast<uint8_t>((value << 1) | (bits[i + static_cast<size_t>(j)] ? 1 : 0));
    }
    data.push_back(value);
  }
  for (uint8_t pad = 0xEC; data.size() < kStartupQrDataCodewords; pad ^= 0xEC ^ 0x11) {
    data.push_back(pad);
  }

  std::vector<uint8_t> result = data;
  std::vector<uint8_t> ecc =
      makeStartupQrRsRemainder(data, makeStartupQrRsDivisor(kStartupQrEccCodewords));
  result.insert(result.end(), ecc.begin(), ecc.end());
  return result;
}

bool startupQrMask(int mask, int x, int y) {
  switch (mask) {
    case 0: return ((x + y) & 1) == 0;
    case 1: return (y & 1) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((x + y) % 3) == 0;
    case 4: return (((y / 2) + (x / 3)) & 1) == 0;
    case 5: return (((x * y) % 2) + ((x * y) % 3)) == 0;
    case 6: return ((((x * y) % 2) + ((x * y) % 3)) & 1) == 0;
    case 7: return ((((x + y) % 2) + ((x * y) % 3)) & 1) == 0;
    default: return false;
  }
}

int startupQrFormatBits(int mask) {
  int data = mask; // Error correction level M uses format bits 00.
  int rem = data;
  for (int i = 0; i < 10; ++i) {
    rem = (rem << 1) ^ (((rem >> 9) & 1) != 0 ? 0x537 : 0);
  }
  return ((data << 10) | rem) ^ 0x5412;
}

void drawStartupQrFormatBits(StartupQrMatrix& qr, int mask) {
  const int bits = startupQrFormatBits(mask);
  auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };

  for (int i = 0; i <= 5; ++i) setStartupQrModule(qr, 8, i, bit(i), true);
  setStartupQrModule(qr, 8, 7, bit(6), true);
  setStartupQrModule(qr, 8, 8, bit(7), true);
  setStartupQrModule(qr, 7, 8, bit(8), true);
  for (int i = 9; i < 15; ++i) setStartupQrModule(qr, 14 - i, 8, bit(i), true);

  for (int i = 0; i < 8; ++i) {
    setStartupQrModule(qr, kStartupQrSize - 1 - i, 8, bit(i), true);
  }
  for (int i = 8; i < 15; ++i) {
    setStartupQrModule(qr, 8, kStartupQrSize - 15 + i, bit(i), true);
  }
  setStartupQrModule(qr, 8, kStartupQrSize - 8, true, true);
}

void drawStartupQrCodewords(StartupQrMatrix& qr, const std::vector<uint8_t>& codewords, int mask) {
  int bitIndex = 0;
  const int totalBits = static_cast<int>(codewords.size() * 8);
  for (int right = kStartupQrSize - 1; right >= 1; right -= 2) {
    if (right == 6) --right;
    for (int vert = 0; vert < kStartupQrSize; ++vert) {
      const int y = (((right + 1) & 2) == 0) ? (kStartupQrSize - 1 - vert) : vert;
      for (int j = 0; j < 2; ++j) {
        const int x = right - j;
        if (qr.function[static_cast<size_t>(y)][static_cast<size_t>(x)]) continue;
        bool dark = false;
        if (bitIndex < totalBits) {
          const uint8_t value = codewords[static_cast<size_t>(bitIndex >> 3)];
          dark = ((value >> (7 - (bitIndex & 7))) & 1) != 0;
        }
        ++bitIndex;
        if (startupQrMask(mask, x, y)) dark = !dark;
        setStartupQrModule(qr, x, y, dark, false);
      }
    }
  }
}

int startupQrPenalty(const StartupQrMatrix& qr) {
  int penalty = 0;
  auto get = [&qr](int x, int y) {
    return qr.modules[static_cast<size_t>(y)][static_cast<size_t>(x)];
  };

  for (int y = 0; y < kStartupQrSize; ++y) {
    int runColor = get(0, y) ? 1 : 0;
    int runLen = 1;
    for (int x = 1; x < kStartupQrSize; ++x) {
      const int color = get(x, y) ? 1 : 0;
      if (color == runColor) {
        ++runLen;
      } else {
        if (runLen >= 5) penalty += 3 + (runLen - 5);
        runColor = color;
        runLen = 1;
      }
    }
    if (runLen >= 5) penalty += 3 + (runLen - 5);
  }
  for (int x = 0; x < kStartupQrSize; ++x) {
    int runColor = get(x, 0) ? 1 : 0;
    int runLen = 1;
    for (int y = 1; y < kStartupQrSize; ++y) {
      const int color = get(x, y) ? 1 : 0;
      if (color == runColor) {
        ++runLen;
      } else {
        if (runLen >= 5) penalty += 3 + (runLen - 5);
        runColor = color;
        runLen = 1;
      }
    }
    if (runLen >= 5) penalty += 3 + (runLen - 5);
  }
  for (int y = 0; y + 1 < kStartupQrSize; ++y) {
    for (int x = 0; x + 1 < kStartupQrSize; ++x) {
      const bool c = get(x, y);
      if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) {
        penalty += 3;
      }
    }
  }
  auto finderPenalty = [&get](int x, int y, bool horizontal) {
    int bits = 0;
    for (int i = 0; i < 11; ++i) {
      bits = (bits << 1) | (get(x + (horizontal ? i : 0), y + (horizontal ? 0 : i)) ? 1 : 0);
    }
    return bits == 0x5D0 || bits == 0x05D;
  };
  for (int y = 0; y < kStartupQrSize; ++y) {
    for (int x = 0; x + 10 < kStartupQrSize; ++x) {
      if (finderPenalty(x, y, true)) penalty += 40;
    }
  }
  for (int x = 0; x < kStartupQrSize; ++x) {
    for (int y = 0; y + 10 < kStartupQrSize; ++y) {
      if (finderPenalty(x, y, false)) penalty += 40;
    }
  }
  int dark = 0;
  for (int y = 0; y < kStartupQrSize; ++y) {
    for (int x = 0; x < kStartupQrSize; ++x) {
      if (get(x, y)) ++dark;
    }
  }
  const int total = kStartupQrSize * kStartupQrSize;
  const int k = std::abs(dark * 20 - total * 10) / total;
  penalty += k * 10;
  return penalty;
}

bool makeStartupQrMatrix(const std::string& text, StartupQrGrid& out) {
  const std::vector<uint8_t> codewords = makeStartupQrCodewords(text);
  if (codewords.empty()) return false;

  int bestPenalty = std::numeric_limits<int>::max();
  StartupQrGrid best{};
  for (int mask = 0; mask < 8; ++mask) {
    StartupQrMatrix qr = makeStartupQrBaseMatrix();
    drawStartupQrCodewords(qr, codewords, mask);
    drawStartupQrFormatBits(qr, mask);
    const int penalty = startupQrPenalty(qr);
    if (penalty < bestPenalty) {
      bestPenalty = penalty;
      best = qr.modules;
    }
  }
  out = best;
  return true;
}

uint32_t startupQrCrc32(const std::vector<uint8_t>& bytes) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : bytes) {
    c ^= b;
    for (int i = 0; i < 8; ++i) {
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

uint32_t startupQrAdler32(const std::vector<uint8_t>& bytes) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (uint8_t value : bytes) {
    a = (a + value) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void appendU32Be(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU16Le(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void appendPngChunk(std::vector<uint8_t>& png, const char type[4],
                    const std::vector<uint8_t>& data) {
  appendU32Be(png, static_cast<uint32_t>(data.size()));
  const size_t typeStart = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  std::vector<uint8_t> crcInput(png.begin() + static_cast<std::ptrdiff_t>(typeStart), png.end());
  appendU32Be(png, startupQrCrc32(crcInput));
}

std::vector<uint8_t> makeUncompressedZlibStream(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  out.reserve(data.size() + data.size() / 65535 * 5 + 16);
  out.push_back(0x78);
  out.push_back(0x01);
  size_t pos = 0;
  while (pos < data.size()) {
    const size_t len = std::min<size_t>(65535, data.size() - pos);
    const bool finalBlock = (pos + len) == data.size();
    out.push_back(finalBlock ? 0x01 : 0x00);
    appendU16Le(out, static_cast<uint16_t>(len));
    appendU16Le(out, static_cast<uint16_t>(~static_cast<uint16_t>(len)));
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
               data.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
  }
  appendU32Be(out, startupQrAdler32(data));
  return out;
}

bool writeStartupQrPng(const std::string& text, const std::string& path) {
  StartupQrGrid grid{};
  if (!makeStartupQrMatrix(text, grid)) return false;

  const int quiet = 2;
  const int totalModules = kStartupQrSize + quiet * 2;
  const int scale = std::max(1, kStartupQrImageSize / totalModules);
  const int imageModulesPx = totalModules * scale;
  const int offset = std::max(0, (kStartupQrImageSize - imageModulesPx) / 2);
  constexpr uint8_t darkR = 0x11, darkG = 0x18, darkB = 0x27;

  std::vector<uint8_t> scanlines(static_cast<size_t>((kStartupQrImageSize * 4 + 1) * kStartupQrImageSize), 0);
  for (int y = 0; y < kStartupQrImageSize; ++y) {
    const size_t row = static_cast<size_t>(y * (kStartupQrImageSize * 4 + 1));
    scanlines[row] = 0;
    for (int x = 0; x < kStartupQrImageSize; ++x) {
      int moduleX = (x - offset) / scale - quiet;
      int moduleY = (y - offset) / scale - quiet;
      bool dark = false;
      if (x >= offset && y >= offset && moduleX >= 0 && moduleY >= 0 &&
          moduleX < kStartupQrSize && moduleY < kStartupQrSize) {
        dark = grid[static_cast<size_t>(moduleY)][static_cast<size_t>(moduleX)];
      }
      const size_t idx = row + 1 + static_cast<size_t>(x * 4);
      scanlines[idx + 0] = dark ? darkR : 0xFF;
      scanlines[idx + 1] = dark ? darkG : 0xFF;
      scanlines[idx + 2] = dark ? darkB : 0xFF;
      scanlines[idx + 3] = 0xFF;
    }
  }

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  appendU32Be(ihdr, kStartupQrImageSize);
  appendU32Be(ihdr, kStartupQrImageSize);
  ihdr.push_back(8);
  ihdr.push_back(6);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  appendPngChunk(png, "IHDR", ihdr);
  appendPngChunk(png, "IDAT", makeUncompressedZlibStream(scanlines));
  appendPngChunk(png, "IEND", {});

  FileUtils::createDirectory(QR_CODE_DIR);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return out.good();
}

std::string getStartupFusionDebugUrl() {
  std::string ip = getStartupMobileAccessIp();
  if (ip.empty()) {
    return "";
  }
  return "http://" + ip + "/fusion-mobile/";
}

StartupRegionHintLayout resolveStartupRegionHintLayout(SystemConfig* systemConfig) {
  StartupRegionHintLayout layout;
  Resolution res;
  res.width = 1920;
  res.height = 1080;
  if (systemConfig) {
    res = systemConfig->getResolution();
  }
  const int canvasW = res.width > 0 ? res.width : 1920;
  const int canvasH = res.height > 0 ? res.height : 1080;
  const int inputCols = systemConfig ? std::max(1, systemConfig->getInputLayoutCols()) : 1;
  const int inputRows = systemConfig ? std::max(1, systemConfig->getInputLayoutRows()) : 1;
  int regionW = systemConfig ? systemConfig->getRegionWidth() : 0;
  int regionH = systemConfig ? systemConfig->getRegionHeight() : 0;
  if (regionW <= 0) regionW = std::max(1, canvasW / inputCols);
  if (regionH <= 0) regionH = std::max(1, canvasH / inputRows);

  layout.x = 0;
  layout.y = 0;
  layout.width = std::clamp(regionW, 1, canvasW);
  layout.height = std::clamp(regionH, 1, canvasH);

  const int shortSide = std::min(layout.width, layout.height);
  const int margin = std::clamp(shortSide / 16, 18, 64);
  const int maxQrByRegion = std::max(96, std::min(layout.width, layout.height) - margin * 2);
  const int qrUpper = std::max(72, std::min(180, maxQrByRegion));
  const int qrLower = std::min(90, qrUpper);
  layout.qrSize = std::clamp(shortSide / 6, qrLower, qrUpper);

  layout.textX = layout.x + margin;
  layout.textY = layout.y + margin;
  layout.textWidth = layout.width - margin * 2;
  layout.textHeight = layout.height - margin * 2;
  layout.qrX = layout.x + layout.width - margin - layout.qrSize;
  layout.qrY = layout.y + layout.height - margin - layout.qrSize;

  layout.textWidth = std::max(1, layout.textWidth);
  layout.textHeight = std::max(1, layout.textHeight);
  layout.qrX = std::clamp(layout.qrX, layout.x,
                          layout.x + std::max(0, layout.width - layout.qrSize));
  layout.qrY = std::clamp(layout.qrY, layout.y,
                          layout.y + std::max(0, layout.height - layout.qrSize));
  return layout;
}
} // 命名空间

void Engine::triggerStartupDeviceReport() {
  bool expected = false;
  if (!startupDeviceReportTriggered_.compare_exchange_strong(expected, true)) {
    return;
  }

  std::string licenseServerUrl = systemConfig_ ? systemConfig_->getLicenseServerUrl() : "";
  if (licenseServerUrl.empty()) {
    licenseServerUrl = "http://" + std::string(LicenseManager::DEFAULT_CLOUD_HOST) + ":" + std::to_string(LicenseManager::DEFAULT_CLOUD_PORT);
    LOG_INFO("[Step 4.9] 使用默认授权服务器 fallback");
  }

  if (licenseManager_) {
    LOG_INFO("[Step 4.9] 触发设备初始化上报（异步）");
    int64_t expiry = licenseManager_->getExpiryTime();
    int64_t start = licenseManager_->getStartTime();
    LOG_INFO("[Step 4.9] 上报参数: expiry=%lld start=%lld",
             (long long)expiry, (long long)start);
    trackEngineAsyncTask(std::async(std::launch::async, [this, licenseServerUrl, expiry, start]() {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      if (shuttingDown_.load()) return;
      reportDeviceToLicenseServer(licenseServerUrl, expiry, start);
    }));
  } else {
    LOG_WARN("[Step 4.9] 跳过设备上报：licenseManager_ 为空");
  }
}

void Engine::initializeNetworkServices() {
  LOG_INFO("[Step 4.5] 初始化网络模块");
  try {
      auto tCall = std::chrono::steady_clock::now();
      PeripheralManager::getInstance().setSystemConfig(systemConfig_.get());
      PeripheralManager::getInstance().setEngine(this);
      // 从磁盘恢复外设配置（含 UDP/TCP 触发映射）；无文件时返回 false，不视为错误
      PeripheralManager::getInstance().loadFromDisk();
      NetworkManager::getInstance().init(commandRouter_.get(), mubu_.get(),
                                         systemConfig_.get(),
                                         playlistManager_.get(),
                                         vodDatabase_.get(), this);
      long long callMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - tCall).count();
      LOG_INFO("  [InitProfile] NetworkManager::init 耗时 %lld ms", callMs);
      std::string webDir = WEB_DIR;
      if (NetworkManager::getInstance().getHttpServer()) {
          NetworkManager::getInstance().getHttpServer()->setStaticDir(webDir);
      }
      if (NetworkManager::getInstance().getMobileHttpServer()) {
          std::string mobileWebDir = webDir + "mobile/";
          NetworkManager::getInstance().getMobileHttpServer()->setStaticDir(mobileWebDir);
      }
      if (NetworkManager::getInstance().getVodHttpServer()) {
          NetworkManager::getInstance().getVodHttpServer()->setStaticDir(hsvj::VOD_WEB_DIR);
          NetworkManager::getInstance().getVodHttpServer()->setKtvStaticDir(hsvj::KTV_WEB_DIR);
      }
      LOG_INFO("  HttpServer(8080) 调试/主 Web");
      LOG_INFO("  HttpServer(8081) 手机/移动端");
      LOG_INFO("  HttpServer(9898) 点歌 /ktv、/vod");
      LOG_INFO("  TcpServer(9000)");
      LOG_INFO("  UdpServer(8000)");
      LOG_INFO("  WebSocketServer(%d)", NetworkManager::getInstance().getWebSocketPort());
  } catch (const std::exception &e) {
      LOG_WARN("  网络模块初始化异常：%s", e.what());
  }
}

void Engine::startNetworkServices() {
  LOG_INFO("[Step 4.6] 统一启用外部端口");
  try {
      auto tCall = std::chrono::steady_clock::now();
      NetworkManager::getInstance().startAll();
      long long callMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - tCall).count();
      LOG_INFO("  [InitProfile] NetworkManager::startAll 耗时 %lld ms", callMs);
      LOG_INFO("  HTTP :8080 调试");
      LOG_INFO("  HTTP :8081 手机");
      LOG_INFO("  HTTP :9898 点歌 /ktv、/vod");
      LOG_INFO("  TCP  :9000");
      LOG_INFO("  UDP  :8000");
      LOG_INFO("  WebSocket :%d", NetworkManager::getInstance().getWebSocketPort());
  } catch (const std::exception &e) {
      LOG_WARN("  网络启动异常：%s", e.what());
  }
}

void Engine::broadcastLocalVodStartupState() {
  if (!systemConfig_ || systemConfig_->getVodMode() != 1 || !systemConfig_->isLocalVodEnabled()) return;
  const Json::Int64 timestamp = static_cast<Json::Int64>(std::time(nullptr) * 1000LL);
  int playState = 0;
  int volume = 100;
  int micStatus = 1;
  if (localVodManager_ && mubu_) {
    int layerId = localVodManager_->getTargetLayerId();
    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      auto* videoLayer = static_cast<LayerVideo*>(layer);
      playState = localVodPlayStateForSync(videoLayer);
      volume = static_cast<int>((localVodPlayer_ ? localVodPlayer_->getMusicVolume() : videoLayer->getVolume()) * 100.0f);
      micStatus = videoLayer->getCurrentAudioTrack() == 0 ? 1 : 0;
    }
  }

  Json::Value playlist(Json::objectValue);
  playlist["type"] = "playListChanged";
  playlist["listType"] = 1;
  playlist["roomId"] = "current";
  playlist["reason"] = "startup";
  playlist["timestamp"] = timestamp;
  NetworkManager::getInstance().broadcastAll(JsonUtils::toString(playlist));

  Json::Value data(Json::objectValue);
  data["roomId"] = "current";
  data["roomName"] = "current";
  data["status"] = 0;
  data["playState"] = playState;
  data["volume"] = volume;
  data["musicVolume"] = volume;
  data["micVolume"] = 100;
  data["mute"] = false;
  data["micStatus"] = micStatus;
  data["currentSongId"] = "";
  data["currentSongTitle"] = "";
  Json::Value playingNow(Json::objectValue);
  playingNow["songId"] = "";
  playingNow["songName"] = "";
  playingNow["songPath"] = "";
  data["playingNow"] = playingNow;
  data["ac"] = Json::Value(Json::objectValue);
  data["light"] = Json::Value(Json::objectValue);
  data["effect"] = Json::Value(Json::objectValue);
  if (localVodPlayer_ && localVodManager_ && localVodPlayer_->getCurrentPlayingId() > 0) {
    LocalVodDatabase::QueueItem item;
    if (localVodManager_->getQueueItemById(localVodPlayer_->getCurrentPlayingId(), item)) {
      playingNow["songId"] = item.songNo;
      playingNow["songName"] = item.songName;
      playingNow["songPath"] = item.songPath;
      data["playingNow"] = playingNow;
      data["currentSongId"] = item.songNo;
      data["currentSongTitle"] = item.songName;
    }
  }

  Json::Value state(Json::objectValue);
  state["type"] = "roomStateChanged";
  state["data"] = data;
  state["roomId"] = "current";
  state["reason"] = "startup";
  state["timestamp"] = timestamp;
  NetworkManager::getInstance().broadcastAll(JsonUtils::toString(state));

  Json::Value command(Json::objectValue);
  command["type"] = "command";
  command["action"] = "SetVolume";
  command["volume"] = volume;
  command["micVolume"] = 100;
  command["timestamp"] = timestamp;
  NetworkManager::getInstance().broadcastAll(JsonUtils::toString(command));

  command = Json::Value(Json::objectValue);
  command["type"] = "command";
  command["action"] = "SwitchTrack";
  command["trackId"] = micStatus;
  command["micStatus"] = micStatus;
  command["timestamp"] = timestamp;
  NetworkManager::getInstance().broadcastAll(JsonUtils::toString(command));

  command = Json::Value(Json::objectValue);
  command["type"] = "command";
  command["action"] = playState == 1 ? "Play" : "Pause";
  command["timestamp"] = timestamp;
  NetworkManager::getInstance().broadcastAll(JsonUtils::toString(command));
  size_t wsClients = 0;
  if (auto* ws = NetworkManager::getInstance().getWebSocketServer()) {
    wsClients = ws->clientCount();
  }
  if (wsClients == 0) {
    LOG_WARN("[VOD] startup sync notify no websocket client playState=%d volume=%d micStatus=%d command=%s",
             playState, volume, micStatus, playState == 1 ? "Play" : "Pause");
  } else {
    LOG_INFO("[VOD] startup sync notify wsClients=%zu playState=%d volume=%d micStatus=%d command=%s",
             wsClients, playState, volume, micStatus, playState == 1 ? "Play" : "Pause");
  }
}

// 定义于 Engine_Init.cpp，供 initialize() 使用
Resolution getSystemResolution();

Engine::Engine()
#ifdef __ANDROID__
    : nativeWindow_(nullptr), initialized_(false), lastAttemptedLyricVideoPath_("")
#else
    : initialized_(false), lastAttemptedLyricVideoPath_("")
#endif
{
  // atomic<bool> 初始化为false
}

Engine::~Engine() { shutdown(); }

void Engine::cleanupCompletedEngineAsyncTasks() {
  std::lock_guard<std::mutex> lock(engineAsyncTasksMutex_);
  engineAsyncTasks_.erase(
      std::remove_if(engineAsyncTasks_.begin(), engineAsyncTasks_.end(),
                     [](std::future<void>& f) {
                       return f.valid() &&
                              f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                     }),
      engineAsyncTasks_.end());
}

void Engine::waitAllEngineAsyncTasks() {
  for (;;) {
    std::vector<std::future<void>> tasks;
    {
      std::lock_guard<std::mutex> lock(engineAsyncTasksMutex_);
      if (engineAsyncTasks_.empty()) break;
      tasks.swap(engineAsyncTasks_);
    }
    for (auto& t : tasks) {
      if (t.valid()) t.wait();
    }
  }
}

void Engine::trackEngineAsyncTask(std::future<void> task) {
  if (!task.valid()) return;
  if (shuttingDown_.load()) {
    task.wait();
    return;
  }
  cleanupCompletedEngineAsyncTasks();
  std::unique_lock<std::mutex> lock(engineAsyncTasksMutex_);
  if (shuttingDown_.load()) {
    lock.unlock();
    task.wait();
    return;
  }
  engineAsyncTasks_.push_back(std::move(task));
}

bool Engine::initializeStep1Environment(const std::chrono::steady_clock::time_point& initStart) {
  // ========== Step 1: 环境预检 ==========
  LOG_INFO("[Step 1] 环境预检");

  // Step 1.1: 初始化路径和日志
  LOG_INFO("[Step 1.1] 初始化路径和日志");
  initializePathConfig();
  if (Logger::initialize(LOGS_DIR)) {
    Logger::setLogType(Logger::LOG_TYPE_INIT);
    Logger::setLevel(Logger::INFO_LEVEL);
  }
  sendProgress(0, 0, "初始化路径和日志...", 5);

  // Step 1.2: 目录治理
  LOG_INFO("[Step 1.2] 目录治理");
  cleanupUnnecessaryFiles();
  createRequiredDirectories();
  populateDefaultCommandListFiles();
  LOG_INFO("  必要目录检查完成");
  sendProgress(0, 1, "创建目录结构...", 10);

  // Step 1.2.1: 更新 apiConfig.js 中的 IP 地址
  updateApiConfigIp();

  // Step 1.3: 授权检查并构建内存授权图层池
  LOG_INFO("[Step 1.3] 授权检查");
  licenseManager_ = std::make_unique<LicenseManager>();
  licenseManager_->initialize(LICENSE_DIR);
  licenseManager_->checkLicense();

  LOG_INFO("  授权图层池 OK");
  {
    std::vector<int> enabledIds = licenseManager_->getEnabledLayerIds();
    const auto& mods = licenseManager_->getModules();
    std::string modStr;
    for (size_t i = 0; i < mods.size(); ++i) {
      if (i) modStr += ",";
      modStr += mods[i];
    }
    LOG_INFO("  授权解析: 支持图层数=%zu 模块=[%s]",
            enabledIds.size(), modStr.empty() ? "(无)" : modStr.c_str());
  }
  sendProgress(0, 2, "检查授权许可...", 15);

  // Step 1.3: 检查配置文件
  LOG_INFO("[Step 1.4] 检查配置文件");
  systemConfig_ = std::make_unique<SystemConfig>();
  std::string configPath = CONFIG_PATH;
  if (!FileUtils::isFile(configPath)) {
    LOG_WARN("  config.json 不存在，尝试创建默认配置文件 path=%s", configPath.c_str());

    std::string configDir = CONFIG_DIR;
    if (!FileUtils::exists(configDir)) {
      FileUtils::createDirectory(configDir);
    }

    Resolution defaultRes = getSystemResolution();
    if (defaultRes.width == 0 || defaultRes.height == 0) {
      defaultRes = Resolution(1920, 1080);
      LOG_WARN("  无法获取系统分辨率，使用默认分辨率 1920x1080");
    } else {
      LOG_INFO("  获取到系统分辨率: %dx%d", defaultRes.width, defaultRes.height);
    }
    systemConfig_->setResolution(defaultRes);
    // 显式初始化矩阵保底布局为 1x1 (1920x1080)
    systemConfig_->setInputWidth(defaultRes.width);
    systemConfig_->setInputHeight(defaultRes.height);
    systemConfig_->setInputLayoutCols(1);
    systemConfig_->setInputLayoutRows(1);
    systemConfig_->setRegionWidth(defaultRes.width);
    systemConfig_->setRegionHeight(defaultRes.height);
    systemConfig_->setRegionCount(1);

    systemConfig_->setOutputWidth(defaultRes.width);
    systemConfig_->setOutputHeight(defaultRes.height);
    systemConfig_->setOutputLayoutCols(1);
    systemConfig_->setOutputLayoutRows(1);

    std::vector<int> authorizedIds = licenseManager_->getEnabledLayerIds();
    if (isAuthorizedLayerId(authorizedIds, 1)) {
      LayerConfigData videoLayer;
      videoLayer.layerKey = "layer1";
      videoLayer.layerId = 1;
      videoLayer.visible = true;
      videoLayer.size = Size(defaultRes.width, defaultRes.height);
      videoLayer.alpha = 1.0f;
      videoLayer.priority = 10;
      systemConfig_->setLayerConfig(1, videoLayer);
    }

    if (isAuthorizedLayerId(authorizedIds, 41)) {
      LayerConfigData hintLayer;
      hintLayer.layerKey = "layer41";
      hintLayer.layerId = 41;
      hintLayer.visible = true;
      hintLayer.size = Size(800, 200);
      hintLayer.position = Position((defaultRes.width - 800) / 2, defaultRes.height - 300);
      hintLayer.alpha = 1.0f;
      hintLayer.priority = 90;
      hintLayer.text = "VJEngine";
      systemConfig_->setLayerConfig(41, hintLayer);
    }

    if (systemConfig_->save(configPath)) {
      LOG_INFO("  已创建默认 config.json (分辨率: %dx%d, 布局: 1x1)", defaultRes.width, defaultRes.height);
    } else {
      int err = errno;
      LOG_ERROR("  创建默认 config.json 失败 path=%s errno=%d %s",
                configPath.c_str(), err, (err != 0 ? strerror(err) : ""));
      return false;
    }
  }
  LOG_INFO("  config.json OK");
  sendProgress(0, 2, "加载配置文件...", 15);

  if (!systemConfig_->load(configPath)) {
    LOG_ERROR("  config.json 加载失败");
    return false;
  }

  {
    std::vector<int> authorizedIds = licenseManager_->getEnabledLayerIds();
    bool hasAuthorizedConfigLayer = false;
    for (const auto &pair : systemConfig_->getAllLayerConfigs()) {
      if (isAuthorizedLayerId(authorizedIds, pair.first)) {
        hasAuthorizedConfigLayer = true;
        break;
      }
    }
    if (!hasAuthorizedConfigLayer) {
      Resolution fallbackRes = systemConfig_->getResolution();
      if (fallbackRes.width == 0 || fallbackRes.height == 0) {
        fallbackRes = Resolution(1920, 1080);
        systemConfig_->setResolution(fallbackRes);
      }
      LOG_WARN("  config.json 中没有已授权图层，生成保底 layer1/layer41 配置");
      if (isAuthorizedLayerId(authorizedIds, 1)) {
        LayerConfigData videoLayer;
        videoLayer.layerKey = "layer1";
        videoLayer.layerId = 1;
        videoLayer.visible = true;
        videoLayer.size = Size(fallbackRes.width, fallbackRes.height);
        videoLayer.alpha = 1.0f;
        videoLayer.priority = 10;
        systemConfig_->setLayerConfig(1, videoLayer);
      }
      if (isAuthorizedLayerId(authorizedIds, 41)) {
        LayerConfigData hintLayer;
        hintLayer.layerKey = "layer41";
        hintLayer.layerId = 41;
        hintLayer.visible = true;
        hintLayer.size = Size(800, 200);
        hintLayer.position = Position((fallbackRes.width - 800) / 2, fallbackRes.height - 300);
        hintLayer.alpha = 1.0f;
        hintLayer.priority = 90;
        hintLayer.text = "VJEngine";
        systemConfig_->setLayerConfig(41, hintLayer);
      }
      systemConfig_->save(configPath);
    }
  }

  // 同步 MPEG-PS 硬解开关到解码核心全局 flag
  hsvj::DecoderCore::sMpegPsHardwareDecode.store(
      systemConfig_->isMpegPsHardwareDecode(), std::memory_order_relaxed);
  hsvj::DecoderCore::sAudioLipSyncOffsetMs.store(
      systemConfig_->getAudioLipSyncOffsetMs(), std::memory_order_relaxed);
  LOG_INFO("  MPEG-PS RKMPP decode config: %s, audioLipSyncOffsetMs=%d",
           systemConfig_->isMpegPsHardwareDecode() ? "enabled" : "legacy-disabled",
           systemConfig_->getAudioLipSyncOffsetMs());

  // 旧 dense_config.json（SuperFlux 密集鼓点检测配置）随 RKNN 子系统下线一起移除。
  // 当前节奏检测由 AudioReactiveEngine 负责，参数走 /api/audio-reactive/config 运行期改写。

  Resolution currentRes = systemConfig_->getResolution();
  if (currentRes.width == 0 || currentRes.height == 0) {
    LOG_WARN("  检测到配置文件分辨率为 0x0，尝试修复");
    Resolution fixedRes = getSystemResolution();
    if (fixedRes.width == 0 || fixedRes.height == 0) {
      fixedRes = Resolution(1920, 1080);
      LOG_WARN("  无法获取系统分辨率，使用默认分辨率 1920x1080");
    } else {
      LOG_INFO("  获取到系统分辨率: %dx%d", fixedRes.width, fixedRes.height);
    }
    systemConfig_->setResolution(fixedRes);
    if (systemConfig_->save(configPath)) {
      LOG_INFO("  已修复配置文件分辨率: %dx%d", fixedRes.width, fixedRes.height);
    } else {
      LOG_WARN("  修复配置文件分辨率失败，但继续使用内存中的值");
    }
  }

  LOG_INFO("[Init 耗时] Step1 完成 %lld ms",
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - initStart).count());

  triggerStartupDeviceReport();

  return true;
}

void Engine::initializeStep2Framework(const std::chrono::steady_clock::time_point& initStart) {
  LOG_INFO("[Step 2] 启动引擎框架");

  LOG_INFO("[Step 2.1] 加载 config.json 配置");
  Resolution res = systemConfig_->getResolution();
  LOG_INFO("  幕布分辨率：%dx%d", res.width, res.height);
  sendProgress(1, 0, "启动引擎框架...", 20);

  LOG_INFO("[Step 2.2] 构造管理器骨架");
  {
    std::vector<int> enabledIds = licenseManager_->getEnabledLayerIds();
    bool licenseHas21 = std::find(enabledIds.begin(), enabledIds.end(), 21) != enabledIds.end();
    bool configHas21 = systemConfig_->hasLayerConfig(21);
    if (licenseHas21 && configHas21) {
      sharedLibassHolder_ = std::make_shared<SharedLibassHolder>();
      LOG_INFO("  SharedLibassHolder（Layer 21 歌词）按需创建（授权+config 均有）");
    } else {
      if (!licenseHas21) {
        LOG_INFO("  SharedLibassHolder 未创建（授权无 Layer 21，节省 ~10MB）");
      } else {
        LOG_INFO("  SharedLibassHolder 未创建（config 未配置 Layer 21，节省 ~10MB）");
      }
    }

    sharedTextOverlayHolder_ = std::make_shared<SharedTextOverlayHolder>();
    LOG_INFO("  SharedTextOverlayHolder（Layer 40/41 共享字体）已创建");
  }
  mubu_ = std::make_unique<Mubu>();
  mubu_->setSharedLibassHolder(sharedLibassHolder_);
  mubu_->setSharedTextOverlayHolder(sharedTextOverlayHolder_);
  LOG_INFO("  Mubu（图层管理器）已注入共享文本资源");
  sendProgress(1, 1, "构造管理器骨架...", 25);

  LOG_INFO("[Init 耗时] Step2 完成 %lld ms",
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - initStart).count());
}

bool Engine::initializeStep3Modules(bool& hasAudioLayer,
                                    bool& hasEffectLayer,
                                    bool& hasPlaylistLayer,
                                    bool& hasKtvLayer,
                                    const std::chrono::steady_clock::time_point& initStart) {
  // ========== Step 3: 初始化模块 ==========
  LOG_INFO("[Step 3] 初始化模块（以图层为中心）");

  LOG_INFO("[Step 3.1] 预创建授权图层");
  preCreateAuthorizedLayers();
  createLayersFromConfig();
  sendProgress(2, 0, "预创建授权图层...", 30);

  hasAudioLayer = false;
  hasEffectLayer = false;
  hasPlaylistLayer = false;
  hasKtvLayer = false;

  {
    std::vector<int> configuredIds = getConfiguredLayerIds();
    if (!configuredIds.empty()) {
      std::ostringstream oss;
      for (size_t i = 0; i < configuredIds.size(); ++i) {
        int layerId = configuredIds[i];
        if (i) oss << ",";
        oss << layerId;

        if ((layerId >= 1 && layerId <= 4) || layerId == 10 || layerId == 11) {
          hasAudioLayer = true;
          hasPlaylistLayer = true;
        }
        if (layerId == 50) hasEffectLayer = true;
        if (layerId == 21) hasPlaylistLayer = true;
        if (layerId == 41 || layerId == 60) hasPlaylistLayer = true;
        if (layerId >= 1 && layerId <= 4) hasKtvLayer = true;
      }
      LOG_INFO("  config 启用图层: [%s] 共 %zu 个，将据此按需加载功能模块与端口",
               oss.str().c_str(), configuredIds.size());
    }
  }

  if (hasPlaylistLayer) {
    playlistManager_ = std::make_unique<PlaylistManager>();
    LOG_INFO("  PlaylistManager（播放列表）初始化中...");
    if (!playlistManager_->initialize(PLAYLIST_DB_PATH)) {
        LOG_ERROR("  PlaylistManager 初始化失败");
        cleanupOnInitFailure();
        return false;
    }

    // 清理临时播放列表（U盘播放列表）
    if (playlistManager_->getDatabase()) {
      int deletedCount = playlistManager_->getDatabase()->deleteAllTemporaryPlaylists();
      if (deletedCount > 0) {
        LOG_INFO("  已清理 %d 个临时播放列表（U盘播放列表）", deletedCount);
      }
    }

    // [HSVJ_VOD_SAFETY] enable_vod=true：删除所有占用 VOD 目标播放图层的手动播放列表，避免与点歌抢占冲突
    if (systemConfig_ && systemConfig_->isVodEnabled() && playlistManager_->getDatabase()) {
      int vodLayerId = systemConfig_->getVodLayerId();
      int deleted = 0;
      if (vodLayerId > 0) {
        deleted += playlistManager_->getDatabase()->deletePlaylistsUsingLayer(vodLayerId);
      }
      if (deleted > 0) {
        LOG_INFO("  enable_vod=1：已删除 %d 个占用 VOD 目标图层 %d 的手动播放列表", deleted,
                 vodLayerId);
      }
    }
  } else {
    LOG_INFO("  PlaylistManager 未启用（无需相关图层）");
  }

  sceneManager_ = std::make_unique<SceneManager>();
  LOG_INFO("  SceneManager（场景管理）");
  commandRouter_ = std::make_unique<CommandRouter>();
  LOG_INFO("  CommandRouter（命令路由）");
  sendProgress(2, 1, "初始化管理器...", 35);

  if (hasAudioLayer) {
    audioProcessor_ = std::make_unique<AudioProcessor>();
    LOG_INFO("  AudioProcessor（音频处理）");
  } else {
    LOG_INFO("  AudioProcessor 未启用（无需相关图层）");
  }

  if (hasEffectLayer || hasAudioLayer) {
    effectManager_ = std::make_unique<EffectManager>();
    LOG_INFO("  EffectManager（特效管理）%s", hasEffectLayer ? "" : "(由音视频图层触发启用)");
    LayerVideo::setEffectManager(effectManager_.get());
  } else {
    LayerVideo::setEffectManager(nullptr);
    LOG_INFO("  EffectManager 未启用（无需相关图层）");
  }

  LOG_INFO("[Init 耗时] Step3 完成 %lld ms",
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - initStart).count());
  return true;
}

void Engine::initializeStep4CloudSync() {
    // ========== Step 4.5: 初始化云端同步服务 ==========
    LOG_INFO("[Step 4.5] 初始化云端同步服务");
    if (!playlistManager_ || !playlistManager_->getDatabase()) {
        LOG_WARN("[Step 4.5] PlaylistManager/database 未就绪，跳过云端同步服务");
        return;
    }
    std::string licenseUrl = systemConfig_->getLicenseServerUrl();
    if (licenseUrl.empty()) {
        licenseUrl = "http://60.205.127.117:8080";
    }

    CloudSyncService::SyncConfig config;
    // 简单解析 host:port
    size_t colon = licenseUrl.find("://");
    std::string hostPart = (colon != std::string::npos) ? licenseUrl.substr(colon + 3) : licenseUrl;
    size_t portPos = hostPart.find(':');
    if (portPos != std::string::npos) {
        config.cloudHost = hostPart.substr(0, portPos);
        config.cloudPort = std::stoi(hostPart.substr(portPos + 1));
    } else {
        config.cloudHost = hostPart;
        config.cloudPort = 8080;
    }

    config.fingerprint = SystemUtils::generateDeviceFingerprint();
    config.materialRootPath = ROOT_PATH + "video/";
    config.intervalSeconds = 600; // 默认 10 分钟同步一次，避免播放期间频繁后台 IO 抢占音频线程

    cloudSyncService_ = std::make_unique<CloudSyncService>();
    cloudSyncService_->start(config, playlistManager_->getDatabase());
}

#ifdef __ANDROID__
bool Engine::initialize(ANativeWindow *window) {
  nativeWindow_ = window;
  return initialize();
}
#endif

bool Engine::initialize() {
  // 使用互斥锁保护初始化过程，防止多线程并发初始化
  std::lock_guard<std::mutex> lock(initMutex_);

  if (initialized_.load()) {
    LOG_WARN("Engine already initialized");
    return true;
  }

  if (preparing_.exchange(true)) {
    LOG_WARN("Engine initialization already in progress, ignoring duplicate call");
    return true;
  }

  shuttingDown_.store(false);

    pthread_setname_np(pthread_self(), "EngineInitThread");
    auto initStart = std::chrono::steady_clock::now();

    bool hasAudioLayer = false;
    bool hasEffectLayer = false;
    bool hasPlaylistLayer = false;
    bool hasKtvLayer = false;

    if (!initializeStep1Environment(initStart)) {
      cleanupOnInitFailure();
      sendError("环境预检失败");
      return false;
    }

    initializeStep2Framework(initStart);
    if (!initializeStep3Modules(hasAudioLayer, hasEffectLayer, hasPlaylistLayer, hasKtvLayer, initStart)) {
      cleanupOnInitFailure();
      sendError("模块初始化失败");
      return false;
    }

    if (hasPlaylistLayer) {
        initializeStep4CloudSync();
    }

    // ========== Step 4: 加载各模块 ==========
    LOG_INFO("[Step 4] 加载各模块");

    // Step 4.1: 按 config 启动硬件（config 配置了 dmx_baudrate 时启动 DMX）
    LOG_INFO("[Step 4.1] 按 config 启动硬件模块");
#ifdef __ANDROID__
    bool dmxEnabled = systemConfig_ && systemConfig_->getDmxBaudRate() > 0;
    if (dmxEnabled) {
      int dmxBaudrate = 250000;
      int dmxStartAddress = systemConfig_->getDmxStartAddress();
      if (dmxStartAddress < 1)
        dmxStartAddress = 1;
      if (dmxStartAddress > 512)
        dmxStartAddress = 512;
      std::string dmxPort = PeripheralManager::getInstance().getDmxPort();
      if (dmxPort.empty() || dmxPort == "artnet" || dmxPort == "sacn") {
        dmxPort = "/dev/ttySWK0";
      }
      dmxReceiver_ = std::make_unique<Dmx512Receiver>(dmxPort, dmxStartAddress, dmxBaudrate);
      if (PeripheralManager::getInstance().isDmxExternalMode()) {
        LOG_INFO("  DMX512 本机接收未启动（外接 RS232 模式，startAddress=%d）",
                 dmxStartAddress);
      } else if (dmxReceiver_->start()) {
        LOG_INFO("  DMX512 本机接收启动 (%s, %d bps, startAddress=%d)（config）",
                 dmxPort.c_str(), dmxBaudrate, dmxStartAddress);
      }
    } else {
      LOG_INFO("  DMX512 未启用（config 中 dmx_baudrate<=0）");
    }
#endif

    dmxChannelHandler_ = std::make_unique<Dmx512ChannelHandler>(this);
    LOG_INFO("  Dmx512ChannelHandler（十二通道解析）");

    // Step 4.2: 初始化渲染模块
    LOG_INFO("[Step 4.2] 初始化渲染模块");
#ifdef __ANDROID__
    Resolution configRes = systemConfig_->getResolution();

    // [关键修复] 不设置 ANativeWindow buffer 尺寸，让它使用屏幕实际尺寸（满屏显示）
    // config 中的分辨率是虚拟幕布分辨率，用于内部渲染，不应该限制物理显示输出

    int outputWidth = configRes.width;
    int outputHeight = configRes.height;
    if (nativeWindow_) {
      outputWidth = ANativeWindow_getWidth(nativeWindow_);
      outputHeight = ANativeWindow_getHeight(nativeWindow_);
      LOG_INFO("  Surface 物理显示器尺寸: %dx%d", outputWidth, outputHeight);
    } else {
      LOG_INFO("  DRM/KMS 无 Surface 初始化，物理尺寸由 connector mode 决定");
    }
    LOG_INFO("  虚拟幕布分辨率: %dx%d", configRes.width, configRes.height);

    // Surface 使用窗口尺寸；DRM/KMS 会在 presenter 初始化时改为 connector mode。
    Resolution actualResolution = Resolution(outputWidth, outputHeight);

    LOG_INFO("  VulkanRenderer init (Display: %dx%d, Canvas: %dx%d)",
             actualResolution.width, actualResolution.height, configRes.width, configRes.height);
    renderer_ = std::make_unique<VulkanRenderer>();
    {
      auto tCall = std::chrono::steady_clock::now();
      if (!renderer_->initialize(nativeWindow_, actualResolution)) {
        LOG_ERROR("  VulkanRenderer init failed");
        cleanupOnInitFailure();
        sendError("VulkanRenderer 初始化失败，请检查 shader 资源是否已同步");
        return false;
      }
      long long callMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - tCall).count();
      LOG_INFO("  [InitProfile] VulkanRenderer::initialize 耗时 %lld ms", callMs);
    }
    renderer_->setLogicalResolution(configRes.width, configRes.height);

    LOG_INFO("  Mubu 初始化（幕布分辨率: %dx%d）", systemConfig_->getResolution().width, systemConfig_->getResolution().height);
    {
      auto tCall = std::chrono::steady_clock::now();
      if (!mubu_->initialize(systemConfig_->getResolution())) {
        cleanupOnInitFailure();
        sendError("幕布初始化失败");
        return false;
      }
      long long callMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - tCall).count();
      LOG_INFO("  [InitProfile] Mubu::initialize 耗时 %lld ms", callMs);
    }

    // 授权检查环节提示授权信息 (仅在 15 天内或已过期时提示)
    if (licenseManager_ && mubu_) {
        Layer* layer41 = mubu_->getLayer(41);
        if (layer41 && layer41->getType() == LayerType::TEXT) {
            LayerText* textLayer = static_cast<LayerText*>(layer41);
            int days = licenseManager_->getDaysUntilExpiry();
            if (days <= 15) { // <--- 关键修复：大于 15 天时不显示
                std::string expiry = licenseManager_->getExpiryDate();
                std::string msg = "授权检查: ";
                if (days < 0) {
                    msg += "已过期 (" + expiry + ")";
                } else {
                    msg += "剩余 " + std::to_string(days) + " 天 (" + expiry + ")";
                }
                textLayer->setVisible(true);
                textLayer->showOperationHint(HintType::CUSTOM, msg, 5.0f);
            }
        }
    }

    // 关联 渲染器
    if (renderer_ && renderer_->isInitialized()) {
        if (sceneManager_) {
            sceneManager_->setRenderer(renderer_.get());
        }
        std::vector<int> allLayerIds = mubu_->getAllLayerIds();
        for (int layerId : allLayerIds) {
            Layer *layer = mubu_->getLayer(layerId);
            if (layer && !layer->getRenderer()) {
                layer->setRenderer(renderer_.get());
            }
        }

        // 为视频图层设置预解码回调：在播放接近结尾时获取下一首路径
        for (int layerId : allLayerIds) {
            Layer *layer = mubu_->getLayer(layerId);
            if (layer && layer->getType() == LayerType::VIDEO) {
                LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
                videoLayer->setPreloadNextPathCallback([this](int lid) -> std::string {
                    if (!playlistManager_) return "";
                    std::string playlistId = playlistManager_->getActivePlaylistId(lid);
                    if (playlistId.empty()) return "";
                    NextVideoInfo info = playlistManager_->peekNextVideoInfo(playlistId);
                    if (!info.valid || info.item.uri.empty()) return "";
                    std::string normalized = FileUtils::normalizePath(info.item.uri);
                    if (normalized.empty() || !FileUtils::isFile(normalized)) return "";
                    return normalized;
                });
            }
        }

        // 仅补加载二维码图片（渲染器 此时才就绪；尺寸/位置已在 createLayersFromConfig 中应用，此处不重复设置）
        Layer *layer71 = mubu_->getLayer(71);
        if (layer71 && (layer71->getType() == LayerType::IMAGE ||
                        layer71->getType() == LayerType::QRCODE)) {
            LayerImage *imageLayer = static_cast<LayerImage *>(layer71);
            if (imageLayer->getWidth() == 0 || imageLayer->getHeight() == 0) {
                const LayerConfigData *config = systemConfig_->getLayerConfig(71);
                if (config) {
                    bool imageLoaded = false;
                    if (!config->imagePath.empty()) {
                        std::string imagePath = config->imagePath;
                        if (imagePath.length() > 0 && imagePath[0] != '/' && imagePath[0] != '.') {
                            if (imagePath.find("huoshan/") == 0) {
                                imagePath = ROOT_PATH + imagePath.substr(8);
                            } else if (imagePath.find("QRCode/") == 0 || imagePath.find("qrcode/") == 0) {
                                imagePath = ROOT_PATH + imagePath;
                            }
                        }
                        std::string normalizedPath = FileUtils::normalizePath(imagePath);
                        if (FileUtils::exists(normalizedPath) && imageLayer->loadImage(normalizedPath)) {
                            imageLoaded = true;
                        }
                    }
                    if (!imageLoaded) {
                        std::string fixedPath = ROOT_PATH + "QRCode/qrcode_71.png";
                        std::string normalizedQRPath = FileUtils::normalizePath(fixedPath);
                        if (FileUtils::exists(normalizedQRPath)) {
                            imageLayer->loadImage(normalizedQRPath);
                        }
                    }
                }
            }
        }

        // 补加载 Logo 图片（渲染器_ 此时才已赋值给各图层；loadImage 第一行检查 !渲染器_ 会返回 false）
        // 与上方 Layer71 二维码补加载同理：createLayersFromConfig 阶段 渲染器_ 尚未设置
        Layer *layer70 = mubu_->getLayer(70);
        if (layer70 && layer70->getType() == LayerType::IMAGE) {
            LayerImage *logoLayer = static_cast<LayerImage *>(layer70);
            if (logoLayer->getWidth() == 0 || logoLayer->getHeight() == 0) {
                std::string logoPath = ROOT_PATH + "Logo/logo.png";
                if (FileUtils::exists(logoPath)) {
                  // 补加载前应用 animated 配置
                  const LayerConfigData *logoConfigPtr = systemConfig_ ? systemConfig_->getLayerConfig(70) : nullptr;
                  bool configAnimated = logoConfigPtr ? logoConfigPtr->animated : false;
                  logoLayer->setAnimated(configAnimated);

                  // 核心优化：如果已经加载过同路径图片，loadImage 会自动跳过重复解压
                  if (logoLayer->loadImage(logoPath)) {
                      LOG_DEBUG("[Logo诊断] renderer就绪后补加载成功 (animated=%s): %s",
                               configAnimated ? "true" : "false", logoPath.c_str());
                  }
                }
            }
        }

        // 设置音频能量可视化图层（Layer 50 效果图层）
        Layer* layer50 = mubu_->getLayer(50);
        if (layer50 && layer50->getType() == LayerType::IMAGE) {
            LayerImage* energyLayer = static_cast<LayerImage*>(layer50);

            // 为所有视频图层设置音频能量可视化支持
            for (int layerId : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
                Layer* layer = mubu_->getLayer(layerId);
                if (layer && layer->getType() == LayerType::VIDEO) {
                    LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
                    videoLayer->setAudioEnergyLayer(energyLayer);
                }
            }
            LOG_INFO("音频能量可视化图层已设置（Layer 50）");
        }
    }

    commandRouter_->setSystemConfig(systemConfig_.get());
    commandRouter_->setEngine(this);

    LOG_INFO("  RegionRotationRenderer 初始化");
    {
      auto tCall = std::chrono::steady_clock::now();

      if (!reinitializeRenderPaths()) {
        cleanupOnInitFailure();
        sendError("区域融合渲染器初始化失败");
        return false;
      }

      long long callMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - tCall).count();
      LOG_INFO("  [InitProfile] RegionRotationRenderer 初始化耗时 %lld ms", callMs);
    }

    LOG_INFO("[Init 耗时] Step4.2 渲染模块完成 %lld ms",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - initStart).count());
#else
    if (!mubu_->initialize(systemConfig_->getResolution())) {
        cleanupOnInitFailure();
        sendError("幕布初始化失败");
        return false;
    }
#endif

    // Step 4.3: 业务能力初始化（播放列表管理器 已在 Step 3 初始化以支持云同步）
    LOG_INFO("[Step 4.3] 业务能力初始化");

    if (hasKtvLayer) {
      {
        auto tCall = std::chrono::steady_clock::now();
        const int vodMode = systemConfig_ ? systemConfig_->getVodMode() : 0;
        const bool LocalVodEnabled = systemConfig_ && vodMode == 1 && systemConfig_->isLocalVodEnabled();
        const bool networkVodEnabled = systemConfig_ && systemConfig_->isNetworkVodEnabled();
        LOG_INFO("  [VOD] init mode=%d local=%d network=%d host=%s roomId=%s",
                 vodMode, LocalVodEnabled ? 1 : 0, networkVodEnabled ? 1 : 0,
                 systemConfig_ ? systemConfig_->getOnlineVodHost().c_str() : "",
                 systemConfig_ ? systemConfig_->getOnlineVodRoomId().c_str() : "");
        if (LocalVodEnabled) {
          const std::string runtimeSongDbPath = DB_DIR + "song.db";
          localSongDatabase_ = std::make_unique<LocalSongDatabase>();
          if (!localSongDatabase_->initialize(runtimeSongDbPath)) {
              LOG_WARN("  LocalSongDatabase 未打开（单机本地离线 song.db 不可用）: %s", runtimeSongDbPath.c_str());
          } else {
              LOG_INFO("  LocalSongDatabase（新 song.db）已打开: %s", runtimeSongDbPath.c_str());
          }
          localVodDatabase_ = std::make_unique<LocalVodDatabase>();
          if (!localVodDatabase_->initialize(PLAYLIST_DB_PATH)) {
              LOG_WARN("  LocalVodDatabase 队列库未打开: %s", PLAYLIST_DB_PATH.c_str());
          }
          localVodManager_ = std::make_unique<LocalVodManager>();
          if (!localVodManager_->initialize(localVodDatabase_.get(), localSongDatabase_.get(), mubu_.get())) {
              LOG_WARN("  LocalVodManager 初始化失败");
          } else if (systemConfig_) {
              int layerId = systemConfig_->getVodLayerId();
              if (layerId < 1) layerId = 1;
              if (!localVodManager_->setTargetLayerId(layerId)) {
                  LOG_WARN("  LocalVodManager VOD目标图层应用失败: %d", layerId);
              } else {
                  LOG_INFO("  LocalVodManager VOD目标图层: %d", layerId);
              }
          }
          localVodPlayer_ = std::make_unique<LocalVodPlayer>();
          if (!localVodPlayer_->initialize(localVodManager_.get(), mubu_.get(), systemConfig_.get())) {
              LOG_WARN("  LocalVodPlayer 初始化失败");
          } else {
              localVodPlayer_->enable();
          }
        } else if (networkVodEnabled) {
          vodDatabase_ = std::make_unique<VodDatabase>();
          if (!vodDatabase_->initOnlineVodSyncDb()) {
              LOG_WARN("  VodDatabase OnlineVod 同步库未打开（vod_queue.db 初始化失败）");
          } else {
              LOG_INFO("  VodDatabase OnlineVod 同步库已打开（网络点歌）");
          }
        } else {
          LOG_INFO("  VodDatabase 跳过歌曲库初始化（VOD 未启用）");
        }
        long long callMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tCall).count();
        LOG_INFO("  [InitProfile] VodDatabase init 耗时 %lld ms", callMs);
      }
      if (systemConfig_ && systemConfig_->getVodMode() == 1 && systemConfig_->isLocalVodEnabled()) {
        const std::string runtimeSongDbPath = DB_DIR + "song.db";
        if (systemConfig_->isLocalSongFileScanEnabled()) {
          std::vector<std::string> mediaRoots = {
            ROOT_PATH,
            ROOT_PATH + "VOD",
            ROOT_PATH + "media",
            ROOT_PATH + "song",
            ROOT_PATH + "songs",
            "/storage",
            "/mnt/media_rw",
            "/mnt/usb_storage",
            "/storage/usb_storage"
          };
          localSongFileScanner_ = std::make_unique<LocalSongFileScanner>();
          LOG_INFO("  [LocalVod] 启动本地歌曲文件扫描器: %s", runtimeSongDbPath.c_str());
          localSongFileScanner_->startAsync(runtimeSongDbPath, mediaRoots);
        } else {
          LOG_INFO("  [LocalVod] 本地歌曲文件扫描器已关闭: %s", runtimeSongDbPath.c_str());
        }
      }
    } else {
      LOG_INFO("  VodDatabase（VOD 点播库）未启动（无需视频层）");
    }

    commandRouter_->setMubu(mubu_.get());
    commandRouter_->setSystemConfig(systemConfig_.get());
    if (playlistManager_) {
      commandRouter_->setPlaylistManager(playlistManager_.get());
    }
    sceneManager_->setMubu(mubu_.get());
    sceneManager_->setSystemConfig(systemConfig_.get());
    sceneManager_->setAuthorizedLayerIds(
        licenseManager_ ? licenseManager_->getEnabledLayerIds() : std::vector<int>());
    sceneManager_->setCurrentConfigPath(CONFIG_DIR + "config.json");
    commandRouter_->setSceneManager(sceneManager_.get());
    commandRouter_->setEngine(this);

    initializeNetworkServices();
    sendProgress(3, 4, "初始化网络模块...", 69);

#ifdef __ANDROID__
    if (systemConfig_) {
        int regionCount = systemConfig_->getRegionCount();
        if (regionCount <= 0)
            regionCount = systemConfig_->getRegionLayoutCols() * systemConfig_->getRegionLayoutRows();
        int outputLayoutCols = systemConfig_->getOutputLayoutCols();
        int outputLayoutRows = systemConfig_->getOutputLayoutRows();

        {
          auto tCall = std::chrono::steady_clock::now();
          // 验证配置有效性
          const bool regionsValid = (regionCount > 0 &&
                                     systemConfig_->getRegionWidth() > 0 &&
                                     systemConfig_->getRegionHeight() > 0);
          LOG_INFO("  Region 配置已加载到 SystemConfig：valid=%d count=%d, inputLayout=%dx%d, outputLayout=%dx%d",
                   regionsValid ? 1 : 0, regionCount,
                   systemConfig_->getInputLayoutCols(),
                   systemConfig_->getInputLayoutRows(), outputLayoutCols,
                   outputLayoutRows);
          if (!regionsValid) {
            LOG_ERROR("  Region 配置无效");
            cleanupOnInitFailure();
            sendError("区域配置无效");
            return false;
          }
          long long callMs = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - tCall).count();
          LOG_INFO("  [InitProfile] Region 配置验证耗时 %lld ms", callMs);
        }
    }
    LOG_INFO("  Region 配置已在渲染路径初始化阶段发布");
    if (renderer_) {
      renderer_->setOnLogicalDeviceRecreated([this]() {
        // GPU 重建期间禁止 checkAndPlayNextVideo 误判 STOPPED 触发自动切歌
        gpuRebuildInProgress_.store(true);
        rebindRegionAndInvalidateAfterGpuRecovery();
        gpuRebuildInProgress_.store(false);
      });
    }
#endif
    LOG_INFO("[Init 耗时] Step4.3 业务 + 区域配置完成 %lld ms",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - initStart).count());
    sendProgress(3, 2, "配置业务能力...", 57);

#ifdef __ANDROID__
    // Step 4.4: DSP 音频配置
    LOG_INFO("[Step 4.4] DSP 音频配置");
    {
        float sysVolume = systemConfig_->getSystemVolume();
        // 自动音频管理的实际 DSP 路由由 Engine::syncAudioOutputLayer 首帧统一落地。
        LOG_INFO("  自动音频管理已启用，系统音量目标：%.1f", sysVolume);
    }
#endif
    sendProgress(3, 3, "DSP 音频配置...", 63);

    // Step 4.7/4.8 的实际启动放到初始化完成后的异步任务中：
    // 渲染循环启动后先让采集图层完成首批 DMA-BUF 纹理导入，再启动默认视频播放。
    sendProgress(3, 7, "启动采集模块...", 90);

    // Step 4.8: 根据 config 中存在的采集图层启动采集（Layer 10/11）
    if (mubu_ && systemConfig_) {
      trackEngineAsyncTask(std::async(std::launch::async, [this]() {
        while (!initialized_.load(std::memory_order_acquire) &&
               !shuttingDown_.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (shuttingDown_.load()) return;
        if (!mubu_ || !systemConfig_) return;
        bool startedFromDefaultConfig = false;
        bool hasConfiguredCaptureLayer = false;
        for (int layerId : {10, 11}) {
          bool hasCfg = systemConfig_->hasLayerConfig(layerId);
          Layer *layer = mubu_ ? mubu_->getLayer(layerId) : nullptr;
          const LayerConfigData *cfg = hasCfg ? systemConfig_->getLayerConfig(layerId) : nullptr;
          if (shuttingDown_.load()) return;
          if (!hasCfg) {
            LOG_INFO("[Step 4.8] 图层 %d: 没有配置图层", layerId);
            continue;
          }
          if (!layer || layer->getType() != LayerType::VIDEO) {
            LOG_INFO("[Step 4.8] 图层 %d: 加载配图层失败（未创建或非视频图层）", layerId);
            continue;
          }
          if (!cfg) {
            LOG_WARN("[Step 4.8] 图层 %d: 配置读取失败，跳过采集启动", layerId);
            continue;
          }
          hasConfiguredCaptureLayer = true;
          LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
          // 配置了采集启动时同时显示该图层，避免“只启了采集但不显示”的困惑
          // Visibility 设置 in createLayersFromConfig based on config.visible
          std::string preferredType = cfg->captureType.empty() ? "AUTO" : cfg->captureType;
          int width = cfg->captureWidth;
          int height = cfg->captureHeight;
          videoLayer->setCaptureRotation(cfg->captureRotation);
          LOG_INFO("[Step 4.8] 图层 %d: 启动后台采集预热 type=%s",
                   layerId, preferredType.c_str());
          videoLayer->checkAndAutoCapture(preferredType, width, height, cfg->captureIndex);
          LOG_INFO("[Step 4.8] 图层 %d: 加载配图层成功", layerId);
          startedFromDefaultConfig = true;
        }
        if (!startedFromDefaultConfig) {
          LOG_INFO("[Step 4.8] 默认配置未包含采集图层，开始从场景文件后台预热采集");
          prewarmCaptureLayersFromScenesAsync();
        }

        if (hasConfiguredCaptureLayer) {
          const auto waitStart = std::chrono::steady_clock::now();
          const auto deadline = waitStart + std::chrono::milliseconds(4500);
          bool warmed = false;
          while (!shuttingDown_.load(std::memory_order_acquire) &&
                 std::chrono::steady_clock::now() < deadline) {
            for (int layerId : {10, 11}) {
              Layer *layer = mubu_ ? mubu_->getLayer(layerId) : nullptr;
              if (!layer || layer->getType() != LayerType::VIDEO) {
                continue;
              }
              LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
              if (videoLayer->isCaptureMode() &&
                  videoLayer->hasCaptureTextureReady()) {
                warmed = true;
                break;
              }
            }
            if (warmed) {
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - waitStart).count();
          LOG_INFO("[采集预热] 启动默认播放前等待采集纹理导入: warmed=%d wait=%lldms",
                   warmed ? 1 : 0, static_cast<long long>(waitMs));
          if (warmed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
          }
        }

        if (!shuttingDown_.load(std::memory_order_acquire)) {
          LOG_INFO("[Step 4.7] 默认播放");
          checkAndPlayDefaultVideo();
        }
      }));
    }

    // Step 4.9: 设备初始化上报（异步，不阻塞；config 中 license_server_url 非空时上报）
    // 如果 config 为空，则尝试使用 License管理器 的默认服务器地址作为 fallback
    triggerStartupDeviceReport();
    sendProgress(3, 8, "设备上报...", 93);

    // 初始化完成后在屏幕上提示本地 IP 与授权信息；授权到期后保持永久提示。
    refreshLicenseScreenHint();
    if (getDebugHotspotIp().empty()) {
      trackEngineAsyncTask(std::async(std::launch::async, [this]() {
        for (int attempt = 1; attempt <= 60; ++attempt) {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          if (shuttingDown_.load()) return;
          if (!getDebugHotspotIp().empty()) {
            LOG_INFO("调试热点地址已就绪，刷新启动提示");
            refreshLicenseScreenHint();
            return;
          }
        }
        LOG_INFO("调试热点地址暂未就绪，保留首次启动提示");
      }));
    }


    applyVodConfigNow();
    startNetworkServices();
    sendProgress(3, 5, "启动网络服务...", 95);

    // 4.9 及后续关键收尾完成后再允许渲染，避免 shutdown 与后续阶段竞争导致资源越界访问
    initialized_.store(true);
    preparing_.store(false);  // 初始化完成，重置 preparing 状态
    LOG_INFO("========== 渲染已就绪 ==========");

    LOG_INFO("========== 初始化完成 ==========");
    sendComplete();
    broadcastLocalVodStartupState();
    trackEngineAsyncTask(std::async(std::launch::async, [this]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      if (shuttingDown_.load()) return;
      broadcastLocalVodStartupState();
    }));

  return true;
}

void Engine::showNetworkIpHint(const std::string& mode, const std::string& ip) {
  if (!mubu_) return;
  Layer* layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT) return;
  LayerText* textLayer = static_cast<LayerText*>(layer41);
  std::string label = (mode == "static") ? "固定 IP" : "动态 IP";
  std::string displayIp = ip.empty() ? SystemUtils::getLocalIp() : ip;
  std::string msg = "网络配置已保存";
  if (!displayIp.empty()) {
    msg += " | " + label + ": " + displayIp;
  } else {
    msg += " | " + label + " 获取中";
  }
  LOG_INFO("网络配置提示: %s", msg.c_str());
  textLayer->setVisible(true);
  textLayer->showOperationHint(HintType::CUSTOM, msg, 8.0f);
}

void Engine::showNetworkStatusHint(const std::string& message) {
  if (!mubu_ || message.empty()) return;
  Layer* layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT) return;
  LayerText* textLayer = static_cast<LayerText*>(layer41);
  LOG_INFO("网络状态提示: %s", message.c_str());
  textLayer->setVisible(true);
  textLayer->showOperationHint(HintType::CUSTOM, message, 8.0f);
}

void Engine::showStartupFusionQrOverlay() {
  if (!mubu_ || !systemConfig_) return;
  Layer* layer71 = mubu_->getLayer(71);
  if (!layer71 || (layer71->getType() != LayerType::IMAGE &&
                   layer71->getType() != LayerType::QRCODE)) {
    return;
  }
  Layer* layer41 = mubu_->getLayer(41);

  LayerImage* qrLayer = static_cast<LayerImage*>(layer71);
  const std::string qrUrl = getStartupFusionDebugUrl();
  if (qrUrl.empty()) {
    LOG_WARN("启动融合二维码没有可用网络地址，跳过显示");
    return;
  }
  const std::string qrPath =
      FileUtils::normalizePath(QR_CODE_DIR + "fusion_mobile_debug_qr_runtime.png");

  std::lock_guard<std::mutex> lock(startupQrMutex_);
  if (!startupQrStateSaved_) {
    startupQrOriginalVisible_ = layer71->isVisible();
    startupQrOriginalAlpha_ = layer71->getAlpha();
    startupQrOriginalPosition_ = layer71->getPosition();
    startupQrOriginalSize_ = layer71->getSize();
    startupQrOriginalImagePath_ = qrLayer->getImagePath();
    if (layer41) {
      startupHintTextStateSaved_ = true;
      startupHintTextOriginalVisible_ = layer41->isVisible();
      startupHintTextOriginalPosition_ = layer41->getPosition();
      startupHintTextOriginalSize_ = layer41->getSize();
    }
    startupQrStateSaved_ = true;
  }

  const bool qrNeedsUpdate = startupQrActiveUrl_ != qrUrl || !FileUtils::exists(qrPath);
  if (qrNeedsUpdate) {
    if (!writeStartupQrPng(qrUrl, qrPath)) {
      LOG_WARN("启动融合二维码生成失败: %s -> %s", qrUrl.c_str(), qrPath.c_str());
      return;
    }
    startupQrActiveUrl_ = qrUrl;
    qrLayer->invalidateCache();
    LOG_INFO("启动融合二维码已按当前网络生成: %s", qrUrl.c_str());
  }

  if (qrLayer->getImagePath() != qrPath || qrNeedsUpdate) {
    if (!qrLayer->loadImage(qrPath)) {
      LOG_WARN("启动融合二维码加载失败: %s", qrPath.c_str());
      return;
    }
  }

  const StartupRegionHintLayout layout = resolveStartupRegionHintLayout(systemConfig_.get());
  if (layer41) {
    layer41->setPosition(Position(layout.textX, layout.textY));
    layer41->setSize(Size(layout.textWidth, layout.textHeight));
    layer41->setVisible(true);
  }
  layer71->setSize(Size(layout.qrSize, layout.qrSize));
  layer71->setPosition(Position(layout.qrX, layout.qrY));
  layer71->setAlpha(1.0f);
  layer71->setVisible(true);
#ifdef __ANDROID__
  if (regionRotationRenderer_) {
    regionRotationRenderer_->invalidateQrOverlayCache();
  }
#endif
}

void Engine::restoreStartupFusionQrOverlay() {
  if (!mubu_) return;
  Layer* layer71 = mubu_->getLayer(71);
  if (!layer71 || (layer71->getType() != LayerType::IMAGE &&
                   layer71->getType() != LayerType::QRCODE)) {
    return;
  }

  LayerImage* qrLayer = static_cast<LayerImage*>(layer71);
  std::lock_guard<std::mutex> lock(startupQrMutex_);
  if (!startupQrStateSaved_) return;

  if (!startupQrOriginalImagePath_.empty() &&
      qrLayer->getImagePath() != startupQrOriginalImagePath_) {
    qrLayer->loadImage(startupQrOriginalImagePath_);
  }
  Layer* layer41 = mubu_->getLayer(41);
  if (layer41 && startupHintTextStateSaved_) {
    layer41->setPosition(startupHintTextOriginalPosition_);
    layer41->setSize(startupHintTextOriginalSize_);
    // Layer 41 is an overlay. After the startup QR/hint window ends, keep it
    // hidden unless a fresh operation or playlist hint explicitly re-enables it.
    layer41->setVisible(false);
  }
  layer71->setPosition(startupQrOriginalPosition_);
  layer71->setSize(startupQrOriginalSize_);
  layer71->setAlpha(startupQrOriginalAlpha_);
  layer71->setVisible(startupQrOriginalVisible_);
  startupQrStateSaved_ = false;
  startupHintTextStateSaved_ = false;
  startupQrOriginalImagePath_.clear();
#ifdef __ANDROID__
  if (regionRotationRenderer_) {
    regionRotationRenderer_->invalidateQrOverlayCache();
  }
#endif
  LOG_INFO("启动融合二维码提示已恢复原 Layer71 状态");
}

void Engine::refreshLicenseScreenHint() {
  if (!mubu_) return;
  Layer* layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT) return;
  LayerText* textLayer = static_cast<LayerText*>(layer41);

  std::string localIp = getStartupMobileAccessIp();
  if (systemConfig_ && systemConfig_->getNetworkIpMode() == "static" &&
      !systemConfig_->getNetworkStaticIp().empty()) {
    localIp = systemConfig_->getNetworkStaticIp();
  }

  std::string msg = "引擎就绪";
  if (!localIp.empty()) msg += " | IP: " + localIp;
  std::string hotspotSsid = getDebugHotspotSsid();
  std::string hotspotIp = getDebugHotspotIp();
  msg += " | 调试热点: " + hotspotSsid;
  if (!hotspotIp.empty()) {
    msg += " | 地址: " + hotspotIp;
  } else {
    msg += " | 地址获取中";
  }

  bool permanentHint = false;
  if (licenseManager_) {
    const int days = licenseManager_->getDaysUntilExpiry();
    const std::string daysSource = licenseManager_->getDaysSource();
    if (days <= 15) {
      if (days < 0) {
        msg += " | 授权已过期 " + std::to_string(std::abs(days)) + " 天";
      } else {
        msg += " | 授权剩 " + std::to_string(days) + " 天";
      }
      permanentHint = licenseManager_->isLicensed() && days <= 0 &&
          daysSource != "time_invalid" && daysSource != "license_duration";
    }
  }

  textLayer->setVisible(true);
  const unsigned long long hintGeneration =
      startupHintGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (permanentHint) {
    textLayer->showProtectedOperationHint(HintType::CUSTOM, msg);
  } else {
    showStartupFusionQrOverlay();
    textLayer->showProtectedOperationHint(HintType::CUSTOM, msg);
    trackEngineAsyncTask(std::async(std::launch::async, [this, hintGeneration, msg]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (shuttingDown_.load()) return;
        if (startupHintGeneration_.load(std::memory_order_relaxed) != hintGeneration) return;
        if (!mubu_) return;
        Layer* layer41 = mubu_->getLayer(41);
        if (!layer41 || layer41->getType() != LayerType::TEXT) return;
        LayerText* textLayer = static_cast<LayerText*>(layer41);
        textLayer->setVisible(true);
        textLayer->showProtectedOperationHint(HintType::CUSTOM, msg);
        showStartupFusionQrOverlay();
      }
      if (startupHintGeneration_.load(std::memory_order_relaxed) == hintGeneration &&
          !shuttingDown_.load() && mubu_) {
        Layer* layer41 = mubu_->getLayer(41);
        if (layer41 && layer41->getType() == LayerType::TEXT) {
          LayerText* textLayer = static_cast<LayerText*>(layer41);
          if (auto* hint = textLayer->getMessageHintRenderer()) {
            hint->clearOperationHint();
            LOG_INFO("启动调试热点提示已显示 30 秒，解除保护");
          }
          textLayer->setVisible(false);
        }
        restoreStartupFusionQrOverlay();
      }
    }));
  }
  LOG_INFO("刷新大屏授权提示: %s", msg.c_str());
}

void Engine::prewarmCaptureLayersFromScenesAsync() {
  if (!mubu_ || !systemConfig_) return;

  trackEngineAsyncTask(std::async(std::launch::async, [this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (shuttingDown_.load() || !mubu_ || !renderer_) return;

    std::vector<std::string> sceneFiles = FileUtils::listFiles(SCENE_DIR, ".json");
    if (sceneFiles.empty()) {
      LOG_INFO("[采集预热] 场景目录没有 json，跳过后台采集预热: %s", SCENE_DIR.c_str());
      return;
    }

    std::set<int> prewarmedLayers;
    for (const std::string &scenePath : sceneFiles) {
      if (shuttingDown_.load()) return;
      if (prewarmedLayers.size() >= 2) break;

      SystemConfig sceneConfig;
      if (!sceneConfig.load(scenePath)) {
        continue;
      }

      for (int layerId : {10, 11}) {
        if (shuttingDown_.load()) return;
        if (prewarmedLayers.count(layerId) > 0) continue;
        const LayerConfigData *cfg = sceneConfig.getLayerConfig(layerId);
        if (!cfg) continue;

        Layer *layer = mubu_->getLayer(layerId);
        if (!layer) {
          LOG_WARN("[采集预热] 图层 %d 未预创建，跳过后台预热 scene=%s",
                   layerId, scenePath.c_str());
          continue;
        }
        if (!layer || layer->getType() != LayerType::VIDEO) {
          continue;
        }

        layer->setRenderer(renderer_.get());
        layer->setVisible(false);

        LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
        videoLayer->setConfiguredCaptureLayer(true);

        LayerConfigData runtimeConfig = *cfg;
        runtimeConfig.layerId = layerId;
        runtimeConfig.visible = false;
        videoLayer->setCaptureRotation(runtimeConfig.captureRotation);
        keepCaptureLayerRunning(layerId, runtimeConfig);

        std::string captureType = runtimeConfig.captureType.empty() ? "AUTO" : runtimeConfig.captureType;
        int width = runtimeConfig.captureWidth;
        int height = runtimeConfig.captureHeight;
        LOG_INFO("[采集预热] 场景采集图层 %d type=%s scene=%s，后台启动并保活",
                 layerId, captureType.c_str(), scenePath.c_str());
        videoLayer->checkAndAutoCapture(captureType, width, height, runtimeConfig.captureIndex);
        prewarmedLayers.insert(layerId);
      }
    }

    if (prewarmedLayers.empty()) {
      LOG_INFO("[采集预热] 未在场景文件中发现采集图层配置");
    }
  }));
}

void Engine::shutdown() {
  // 使用互斥锁保护关闭过程
  std::lock_guard<std::mutex> lock(initMutex_);

  bool wasInitialized = initialized_.load();
  bool wasPreparing = preparing_.load();
  if (!wasInitialized && !wasPreparing) {
    return;
  }

  // 立即设置 shuttingDown 标志，停止所有自动播放逻辑
  shuttingDown_.store(true);

  LOG_DEBUG("Shutting down HSVJEngine...");
  waitAllEngineAsyncTasks();
  cleanupResources();
  initialized_.store(false);
  preparing_.store(false);
  LOG_DEBUG("HSVJEngine shutdown complete");

  // 关闭日志系统
  Logger::shutdown();
}

void Engine::refreshOnlineVodQueueAndApplyAsync(const char* reason) {
  static std::atomic<bool> s_playlistRefreshInFlight{false};
  bool expected = false;
  if (!s_playlistRefreshInFlight.compare_exchange_strong(expected, true)) {
    LOG_INFO("[OnlineVod] queue refresh already in flight, skip request: %s", reason ? reason : "");
    return;
  }

  std::string reasonCopy = reason ? reason : "";
  trackEngineAsyncTask(std::async(std::launch::async, [this, reasonCopy]() {
    struct ResetInFlight {
      std::atomic<bool>& flag;
      ~ResetInFlight() { flag.store(false); }
    } reset{s_playlistRefreshInFlight};

    if (shuttingDown_.load()) return;
    if (!systemConfig_) return;
    std::string host = systemConfig_->getOnlineVodHost();
    int port = 9898;
    std::string roomId = lastOnlineVodWsRoomId_.empty() ? "current" : lastOnlineVodWsRoomId_;
    if (host.empty()) host = lastOnlineVodWsHost_;
    if (host.empty()) return;

    std::string url = "http://" + host;
    if (port != 80 && port > 0) url += ":" + std::to_string(port);
    url += "/api/v1/rooms/" + roomId + "/queue";

    LOG_DEBUG("[OnlineVod] refreshing queue (%s): %s", reasonCopy.c_str(), url.c_str());
    std::string body = httpGet(url, 2);
    if (body.empty()) {
      LOG_WARN("[OnlineVod] async queue refresh failed (empty): %s", url.c_str());
      return;
    }

    Json::Value root;
    std::string err;
    if (!JsonUtils::parseJson(body, root, err)) {
      LOG_WARN("[OnlineVod] async queue refresh parse failed: %s", err.c_str());
      return;
    }

    Json::Value arr;
    if (root.isObject() && root.isMember("data") && root["data"].isArray()) {
      arr = root["data"];
    } else if (root.isArray()) {
      arr = root;
    } else {
      LOG_WARN("[OnlineVod] async queue refresh invalid payload");
      return;
    }

    std::vector<VodDatabase::OnlineVodQueueItem> refreshed;
    refreshed.reserve(static_cast<size_t>(arr.size()));
    for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
      const auto& it = arr[i];
      if (!it.isObject()) continue;
      VodDatabase::OnlineVodQueueItem q;
      q.id = it.get("id", "").asString();
      q.roomId = it.get("roomId", "").asString();
      q.songId = it.get("songId", "").asString();
      q.songTitle = it.get("songName", it.get("songTitle", "")).asString();
      q.artistName = it.get("singerName", it.get("artistName", "")).asString();
      q.position = it.get("position", 0).asInt();
      q.status = it.get("status", 0).asInt();
      q.songPath = it.get("songPath", "").asString();
      q.track = it.get("track", 0).asInt();
      refreshed.push_back(std::move(q));
    }

    {
      std::lock_guard<std::mutex> lock(onlineVodQueueMutex_);
      onlineVodQueue_ = std::move(refreshed);
      LOG_DEBUG("[OnlineVod] async queue refresh ok: %zu items", onlineVodQueue_.size());
    }

    if (reasonCopy == "websocket_reconnected") {
      std::lock_guard<std::mutex> lock(onlineVodQueueMutex_);
      if (onlineVodQueue_.empty()) {
        LOG_INFO("[OnlineVod] queue empty after reconnect, requesting server skip instead of local playback fallback");
        requestOnlineVodSkipAsync("reconnect_queue_empty");
      }
    }
  }));
}

void Engine::requestOnlineVodSkipAsync(const std::string& reason) {
  std::string host = systemConfig_ ? systemConfig_->getOnlineVodHost() : "";
  int port = 9898;
  if (host.empty()) host = lastOnlineVodWsHost_;
  if (host.empty()) {
    LOG_WARN("[OnlineVod] skip request ignored, missing server host: %s", reason.c_str());
    return;
  }

  std::string roomId = lastOnlineVodWsRoomId_.empty() ? "current" : lastOnlineVodWsRoomId_;
  std::string url = "http://" + host;
  if (port != 80 && port > 0) url += ":" + std::to_string(port);
  url += "/api/v1/rooms/" + roomId + "/skip";
  trackEngineAsyncTask(std::async(std::launch::async, [this, url, reason]() {
    if (shuttingDown_.load()) return;
    std::string resp = httpPostJson(url, "{}", 3);
    LOG_INFO("[OnlineVod] skip request (%s): %s -> %s", reason.c_str(), url.c_str(), resp.c_str());
  }));
}

bool Engine::startOnlineVodSync(const std::string& host, int port, const std::string& roomId) {
  if (!vodDatabase_) {
    LOG_WARN("startOnlineVodSync: VodDatabase not ready");
    return false;
  }
  if (systemConfig_ && !systemConfig_->isNetworkVodEnabled()) {
    LOG_WARN("startOnlineVodSync: vodMode is not network, sync is disabled");
    return false;
  }
#if defined(_WIN32) || defined(_WIN64)
  if (!onlineVodHttpSyncClient_) {
    onlineVodHttpSyncClient_ = std::make_unique<OnlineVodHttpSyncClient>();
  }
  if (onlineVodHttpSyncClient_->isRunning()) return true;
  LOG_INFO("OnlineVodSync: start http-poll=%s:%d roomId=%s", host.c_str(), port, roomId.c_str());
  return onlineVodHttpSyncClient_->start(host, port, roomId, vodDatabase_.get(), 1000);
#else
  // 与「当前 WS 实际连接」对比，避免仅更新 lastOnlineVod* 配置快照后误判为未变化
  bool paramsChanged = false;
  if (onlineVodWsClient_ && onlineVodWsClient_->isRunning()) {
    paramsChanged = (host != lastOnlineVodWsHost_) || (port != lastOnlineVodWsPort_) ||
                    (roomId != lastOnlineVodWsRoomId_);

    if (paramsChanged) {
      LOG_INFO("[OnlineVod] config changed, restarting WebSocket (old: %s:%d room=%s, new: %s:%d room=%s)",
               lastOnlineVodWsHost_.c_str(), lastOnlineVodWsPort_,
               lastOnlineVodWsRoomId_.c_str(), host.c_str(), port, roomId.c_str());
      onlineVodWsEverConnected_.store(false);
      onlineVodWsClient_->stop();
      for (int i = 0; i < 20 && onlineVodWsClient_->isRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    } else {
      return true;
    }
  }

  // 创建客户端（如果还没创建）
  if (!onlineVodWsClient_) {
    onlineVodWsClient_ = std::make_unique<OnlineVodWsClient>();

    // 只在创建时注册一次回调，避免重复注册
    LOG_INFO("[OnlineVod] registering WebSocket callbacks (one-time setup)");

    // 注入连接状态回调，用于显示连接状态提示
    onlineVodWsClient_->setOnFreesongNotify([this]() {
      // 服务器通知 freesongs 列表有变化：重新拉取列表，然后立即播放
      if (shuttingDown_.load()) return;
      int vodLayerId = systemConfig_ ? systemConfig_->getVodLayerId() : 1;
      if (vodLayerId < 1) vodLayerId = 1;
      const int capturedLayerId = vodLayerId;
      LOG_INFO("[OnlineVod] freesong notify received, refreshing list and playing on layer %d", capturedLayerId);
      onlineVodNextEmptyCount_ = 0;  // 服务器有新内容，重置空响应计数器恢复正常轮询
      trackEngineAsyncTask(std::async(std::launch::async, [this, capturedLayerId]() {
        if (shuttingDown_.load()) return;
        // 重新拉取空闲歌曲列表
        fetchAndCacheFreesongs();
        // 只有当前没有点歌在播时才切换到 freesong
        if (!mubu_) return;
        Layer* layer = mubu_->getLayer(capturedLayerId);
        if (!layer || layer->getType() != LayerType::VIDEO) return;
        LayerVideo* vl = static_cast<LayerVideo*>(layer);
        if (vl->getState() == LayerVideo::PlayState::PLAYING) {
          // 当前有歌在播（点歌），不打断
          LOG_INFO("[OnlineVod] freesong notify: layer %d is playing, skip freesong", capturedLayerId);
          return;
        }
        tryPlayFreesongOnLayer(capturedLayerId);
      }));
    });

    onlineVodWsClient_->setOnConnectionStateChanged([this](bool connected) {
      if (!connected) {
        if (mubu_) {
          Layer* layer41 = mubu_->getLayer(41);
          if (layer41 && layer41->getType() == LayerType::TEXT) {
            LayerText* textLayer = static_cast<LayerText*>(layer41);
            if (!onlineVodWsEverConnected_.load()) {
              textLayer->setTextColor(Color(1.0f, 0.31f, 0.31f, 1.0f));
              textLayer->showOperationHint(HintType::CUSTOM, "连接服务器失败\n请检查网络", 12.0f);
            } else {
              textLayer->setTextColor(Color(1.0f, 0.75f, 0.0f, 1.0f));
              textLayer->showOperationHint(HintType::CUSTOM, "点播服务器已断开\n正在重连...", 8.0f);
            }
          }
        }
        LOG_WARN("[OnlineVod] WebSocket disconnected, showing hint (everConnected=%d)",
                 onlineVodWsEverConnected_.load() ? 1 : 0);
      } else {
        bool wasEverConnected = onlineVodWsEverConnected_.exchange(true);
        if (wasEverConnected) {
          if (mubu_) {
            Layer* layer41 = mubu_->getLayer(41);
            if (layer41 && layer41->getType() == LayerType::TEXT) {
              LayerText* textLayer = static_cast<LayerText*>(layer41);
              textLayer->showOperationHint(HintType::CUSTOM, "已重新连接", 2.0f);
            }
          }
          // 重连后主动发 joinRoom，触发服务器重新推送当前房态
          if (!lastOnlineVodWsRoomId_.empty()) {
            onlineVodWsClient_->postJoinRoom(lastOnlineVodWsRoomId_);
            LOG_INFO("[OnlineVod] WebSocket reconnected, re-joining room: %s", lastOnlineVodWsRoomId_.c_str());
          }
          refreshOnlineVodQueueAndApplyAsync("websocket_reconnected");
          LOG_INFO("[OnlineVod] WebSocket reconnected successfully");
        } else {
          // 首次连接：发 joinRoom
          if (!lastOnlineVodWsRoomId_.empty()) {
            onlineVodWsClient_->postJoinRoom(lastOnlineVodWsRoomId_);
            LOG_INFO("[OnlineVod] WebSocket connected (first time), joining room: %s", lastOnlineVodWsRoomId_.c_str());
          } else {
            LOG_INFO("[OnlineVod] WebSocket connected successfully (first time)");
          }
          refreshOnlineVodQueueAndApplyAsync("websocket_first_connected");
        }
      }
    });

    // 注入状态变化回调，用于触发图层41操作提示
    onlineVodWsClient_->setOnRoomStateChanged([this](const VodDatabase::OnlineVodState& st) {
      onOnlineVodStateChanged(st);
    });

    onlineVodWsClient_->setOnPlaybackCommand([this](const std::string& action,
                                                 const Json::Value& param,
                                                 const std::string& rawJson) {
      return handleOnlineVodPlaybackCommand(action, param, rawJson);
    });

    onlineVodWsClient_->setOnPlayListUpdated([this](std::vector<VodDatabase::OnlineVodQueueItem> items, int listType) {
    // 清除等待服务器响应的标志
    if (vodDatabase_ && listType == 1) {
      std::string waiting = vodDatabase_->getOnlineVodSyncMeta("online_vod_waiting_for_next");
      if (!waiting.empty()) {
        LOG_INFO("[OnlineVod] received new queue while waiting for next response, clearing wait flag");
        vodDatabase_->setOnlineVodSyncMeta("online_vod_waiting_for_next", "");
      }
    }

    // 存入内存队列（轻量协议下，items 可能为空，仅表示“队列有变化”）
    if (listType == 1) {
      if (!items.empty()) {
        onlineVodSuppressInitialEmptyPlaylistRefresh_.store(false);
        std::lock_guard<std::mutex> lock(onlineVodQueueMutex_);
        onlineVodQueue_ = std::move(items);
      } else {
        if (onlineVodSuppressInitialEmptyPlaylistRefresh_.exchange(false)) {
          LOG_INFO("[OnlineVod] initial empty playListChanged accepted, async refresh queue");
        }
        refreshOnlineVodQueueAndApplyAsync("playlist_changed");
      }
    }

    if (listType != 1) return;
    // 轻量协议：playListChanged 只作为“列表有变化”的信号；由本地异步刷新队列
    LOG_DEBUG("[OnlineVod] playListChanged signal received, async refresh queue");
    });
  }

  // 每次启动前更新服务器地址到数据库（供播放 URL 构造和后续同步使用）
  vodDatabase_->setOnlineVodSyncMeta("online_vod_server_host", host);
  vodDatabase_->setOnlineVodSyncMeta("online_vod_server_port", std::to_string(port));
  vodDatabase_->setOnlineVodSyncMeta("online_vod_roomId", roomId);

  // 启动前清空内存队列
  {
    std::lock_guard<std::mutex> lock(onlineVodQueueMutex_);
    onlineVodQueue_.clear();
  }
  LOG_INFO("[OnlineVod] cleared in-memory queue");
  onlineVodSuppressInitialEmptyPlaylistRefresh_.store(true);

  LOG_INFO("[OnlineVod] starting WebSocket connection: host=%s port=%d roomId=%s",
           host.c_str(), port, roomId.c_str());
  bool started = onlineVodWsClient_->start(host, port, roomId, vodDatabase_.get());
  if (started) {
    lastOnlineVodWsHost_ = host;
    lastOnlineVodWsPort_ = port;
    lastOnlineVodWsRoomId_ = roomId;
    onlineVodFreesongBaseUrl_ = "http://" + host;
    if (port != 80 && port > 0) onlineVodFreesongBaseUrl_ += ":" + std::to_string(port);
  }
  return started;
#endif
}

static bool isVideoExtension(const std::string& name) {
  static const char* exts[] = {
    ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv",
    ".ts", ".m4v", ".webm", ".mpg", ".mpeg", ".3gp"
  };
  auto dotPos = name.rfind('.');
  if (dotPos == std::string::npos) return false;
  std::string ext = name.substr(dotPos);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* e : exts) {
    if (ext == e) return true;
  }
  return false;
}

void Engine::fetchAndCacheFreesongs() {
  if (onlineVodFreesongBaseUrl_.empty()) return;

  auto trimCopy = [](std::string s) -> std::string {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
  };

  std::string idleSongPathForUrl;
  auto normalizeSubPath = [](std::string path) -> std::string {
    for (auto& c : path) {
      if (c == '\\') c = '/';
    }
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
  };

  auto makeAbsoluteUrl = [this, trimCopy, normalizeSubPath, &idleSongPathForUrl](std::string raw) -> std::string {
    raw = trimCopy(raw);
    if (raw.empty()) return "";

    if ((raw.front() == '"' && raw.back() == '"') ||
        (raw.front() == '\'' && raw.back() == '\'')) {
      raw = raw.substr(1, raw.size() - 2);
    }

    if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0) {
      size_t schemeEnd = raw.find("://");
      size_t hostStart = (schemeEnd == std::string::npos) ? 0 : (schemeEnd + 3);
      size_t pathStart = raw.find('/', hostStart);
      if (pathStart != std::string::npos) {
        return onlineVodFreesongBaseUrl_ + raw.substr(pathStart);
      }
      return raw;
    }

    if (raw.rfind("/", 0) == 0) {
      return onlineVodFreesongBaseUrl_ + raw;
    }

    const std::string subPath = normalizeSubPath(idleSongPathForUrl);
    if (subPath.empty()) {
      return onlineVodFreesongBaseUrl_ + "/" + raw;
    }
    return onlineVodFreesongBaseUrl_ + "/" + subPath + "/" + raw;
  };

  auto appendCandidate = [&](const std::string& candidate, std::vector<std::string>& out,
                             std::set<std::string>& dedup) {
    std::string normalized = trimCopy(candidate);
    if (normalized.empty()) return;

    std::string checkName = normalized;
    size_t queryPos = checkName.find_first_of("?#");
    if (queryPos != std::string::npos) {
      checkName = checkName.substr(0, queryPos);
    }

    size_t slashPos = checkName.find_last_of('/');
    if (slashPos != std::string::npos) {
      checkName = checkName.substr(slashPos + 1);
    }

    if (!isVideoExtension(checkName)) return;

    std::string absoluteUrl = makeAbsoluteUrl(normalized);
    if (absoluteUrl.empty()) return;
    if (dedup.insert(absoluteUrl).second) {
      out.push_back(absoluteUrl);
    }
  };

  auto extractFromRawResponse = [&](const std::string& resp, std::vector<std::string>& out) -> bool {
    std::set<std::string> dedup;
    out.clear();

    // 1. 逐行文本：兼容纯文本列表、目录索引、直接 URL
    size_t start = 0;
    while (start <= resp.size()) {
      size_t end = resp.find('\n', start);
      std::string line = trimCopy(resp.substr(start, end == std::string::npos ? std::string::npos : end - start));
      if (!line.empty()) {
        appendCandidate(line, out, dedup);
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }

    // 2. HTML href：兼容目录页/静态文件索引
    const std::string hrefKey = "href=";
    size_t pos = 0;
    while ((pos = resp.find(hrefKey, pos)) != std::string::npos) {
      pos += hrefKey.size();
      if (pos >= resp.size()) break;

      char quote = resp[pos];
      if (quote == '"' || quote == '\'') {
        ++pos;
        size_t end = resp.find(quote, pos);
        if (end == std::string::npos) break;
        appendCandidate(resp.substr(pos, end - pos), out, dedup);
        pos = end + 1;
      }
    }

    return !out.empty();
  };

  auto fetchFromApi = [&extractFromRawResponse, makeAbsoluteUrl, &idleSongPathForUrl](const std::string& apiUrl, int timeoutSec,
                             std::vector<std::string>& out) -> bool {
    LOG_INFO("[OnlineVod] fetching freesongs: %s", apiUrl.c_str());
    std::string resp = hsvj::httpGet(apiUrl, timeoutSec);
    if (resp.empty()) {
      LOG_WARN("[OnlineVod] freesongs fetch: empty response (timeout or network error), url=%s", apiUrl.c_str());
      return false;
    }

    Json::Value root;
    std::string err;
    if (!JsonUtils::parseJson(resp, root, err) || !root.isMember("data")) {
      if (extractFromRawResponse(resp, out)) {
        LOG_INFO("[OnlineVod] freesongs response is non-JSON, extracted %zu video files from raw body", out.size());
        return true;
      }
      LOG_WARN("[OnlineVod] freesongs parse failed: %s, url=%s", err.c_str(), apiUrl.c_str());
      return false;
    }

    if (root["data"].isObject() && root["data"].isMember("files") && root["data"]["files"].isArray()) {
      const auto& data = root["data"];
      if (data.isMember("idle_song_path") && data["idle_song_path"].isString()) {
        idleSongPathForUrl = data["idle_song_path"].asString();
      }

      int skipped = 0;
      std::set<std::string> dedup;
      out.clear();
      for (Json::ArrayIndex i = 0; i < data["files"].size(); i++) {
        if (!data["files"][i].isString()) { ++skipped; continue; }
        std::string name = data["files"][i].asString();
        if (!isVideoExtension(name)) { ++skipped; continue; }
        std::string absoluteUrl = makeAbsoluteUrl(name);
        if (!absoluteUrl.empty() && dedup.insert(absoluteUrl).second) {
          out.push_back(absoluteUrl);
        }
      }
      if (skipped > 0) LOG_INFO("[OnlineVod] freesongs scan: skipped %d non-video entries", skipped);
      return !out.empty();
    }

    if (!root["data"].isArray()) {
      LOG_WARN("[OnlineVod] freesongs parse failed: data is neither array nor scan object, url=%s", apiUrl.c_str());
      return false;
    }

    int skipped = 0;
    std::set<std::string> dedup;
    out.clear();
    for (Json::ArrayIndex i = 0; i < root["data"].size(); i++) {
      const auto& f = root["data"][i];
      std::string fileUrl = f.get("url", "").asString();
      std::string name = f.get("name", "").asString();
      std::string checkName = name.empty() ? fileUrl : name;
      if (!isVideoExtension(checkName)) { ++skipped; continue; }

      if (!fileUrl.empty()) {
        fileUrl = makeAbsoluteUrl(fileUrl);
        if (!fileUrl.empty() && dedup.insert(fileUrl).second) {
          out.push_back(fileUrl);
        }
      } else if (!name.empty()) {
        std::string absoluteUrl = makeAbsoluteUrl(name);
        if (!absoluteUrl.empty() && dedup.insert(absoluteUrl).second) {
          out.push_back(absoluteUrl);
        }
      }
    }

    if (skipped > 0) LOG_INFO("[OnlineVod] freesongs: skipped %d non-video files", skipped);
    return !out.empty();
  };

  std::vector<std::string> found;
  const std::string api1 = onlineVodFreesongBaseUrl_ + "/api/v1/idle-media/scan";

  bool ok = fetchFromApi(api1, 5, found);

  if (!ok || found.empty()) {
    std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
    if (!onlineVodFreesongsCache_.empty()) {
      if (onlineVodFreesongsList_.empty()) {
        onlineVodFreesongsList_ = onlineVodFreesongsCache_;
        onlineVodFreesongsIndex_ = 0;
      }
      LOG_INFO("[OnlineVod] freesongs fetch failed, keep cached list: %zu files", onlineVodFreesongsCache_.size());
    } else {
      onlineVodFreesongsList_.clear();
      onlineVodFreesongsCache_.clear();
      onlineVodFreesongsIndex_ = 0;
      LOG_WARN("[OnlineVod] freesongs fetch failed and no cache available; idle list remains empty");
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
    const int preservedIndex = onlineVodFreesongsList_.empty()
        ? 0
        : (onlineVodFreesongsIndex_ % static_cast<int>(found.size()));
    onlineVodFreesongsList_ = found;
    onlineVodFreesongsCache_ = found;
    onlineVodFreesongsIndex_ = preservedIndex;
  }
  LOG_INFO("[OnlineVod] freesongs indexed: %zu video files", found.size());
  const size_t sampleCount = std::min<size_t>(found.size(), 3);
  for (size_t i = 0; i < sampleCount; ++i) {
    LOG_INFO("[OnlineVod] freesongs sample[%zu]=%s", i, found[i].c_str());
  }
}

void Engine::onlineVodWsPostJoinRoom(const std::string& roomId) {
  if (roomId.empty()) return;
  if (onlineVodWsClient_ && onlineVodWsClient_->isRunning())
    onlineVodWsClient_->postJoinRoom(roomId);
}

void Engine::onlineVodWsPostAppPing() {
  if (onlineVodWsClient_ && onlineVodWsClient_->isRunning())
    onlineVodWsClient_->postAppLayerPing();
}

void Engine::processOnlineVodServerPendingActions() {
  if (!vodDatabase_ || shuttingDown_.load()) return;

  std::string routerJson = vodDatabase_->getOnlineVodSyncMeta("online_vod_pending_router_json", "");
  if (!routerJson.empty() && commandRouter_) {
    vodDatabase_->setOnlineVodSyncMeta("online_vod_pending_router_json", "");
    commandRouter_->processCommand(routerJson);
  }
}

bool Engine::handleOnlineVodPlaybackCommand(const std::string& action,
                                     const Json::Value& param,
                                     const std::string& rawJson) {
  (void)rawJson;
  if (!commandRouter_ || !systemConfig_) return false;

  auto dispatchPlaybackCommand = [this, &action](const Json::Value& cmdParam) -> bool {
    Json::Value cmdJson;
    cmdJson["type"] = 0;
    cmdJson["code"] = 0x02;
    cmdJson["param"] = cmdParam;

    const CommandResponse resp = commandRouter_->processCommand(JsonUtils::toString(cmdJson));
    LOG_INFO("[OnlineVod] command direct execute: action=%s ok=%d error=0x%04X message=%s",
             action.c_str(), resp.ok ? 1 : 0, resp.error, resp.message.c_str());
    return resp.ok;
  };

  Json::Value cmdParam(Json::objectValue);
  const int audioLayerId = systemConfig_->getAudioOutputLayerId() > 0
                               ? systemConfig_->getAudioOutputLayerId()
                               : 1;
  cmdParam["layerId"] = audioLayerId;

  if (action == "Play" || action == "Resume") {
    cmdParam["action"] = "resume";
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "PlayUrl") {
    LOG_WARN("[OnlineVod] PlayUrl is not treated as a playback control command");
    return false;
  }

  if (action == "Pause") {
    cmdParam["action"] = "pause";
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "Stop") {
    cmdParam["action"] = "stop";
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "Replay") {
    cmdParam["action"] = "replay";
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "SetVolume") {
    if (!param.isMember("volume") || !param["volume"].isNumeric()) return false;
    cmdParam["action"] = "setSystemVolume";
    const int volume = param["volume"].asInt();
    cmdParam["volume"] = (volume <= 1) ? 0.0f : static_cast<float>(volume) / 100.0f;
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "Mute") {
    cmdParam["action"] = "setSystemVolume";
    cmdParam["volume"] = 0.0f;
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "Unmute") {
    cmdParam["action"] = "setSystemVolume";
    int volume = (param.isMember("volume") && param["volume"].isNumeric())
                     ? param["volume"].asInt()
                     : static_cast<int>(systemConfig_->getSystemVolume() * 100.0f + 0.5f);
    if (volume <= 0) volume = 60;
    cmdParam["volume"] = static_cast<float>(volume) / 100.0f;
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "SwitchTrack") {
    // 严格按服务器命令处理：
    //   trackId=2/3 → 切换音轨（2=伴唱音轨，3=原唱音轨）
    //   trackId=4/5 → 切换声道（4=左声道伴唱，5=右声道原唱）
    int track = param.get("trackId", -1).asInt();
    if (track < 0) return false;
    int micStatus = param.get("micStatus", -1).asInt();

    LOG_INFO("[OnlineVod] SwitchTrack: trackId=%d micStatus=%d", track, micStatus);

    auto showSwitchTrackHint = [this, micStatus, track]() {
      if (!commandRouter_) return;
      if (micStatus == 1 || track == 3 || track == 5) {
        commandRouter_->triggerLayer41Hint(static_cast<int>(HintType::AUDIO_TRACK));
      } else if (micStatus == 0 || track == 2 || track == 4) {
        commandRouter_->triggerLayer41Hint(static_cast<int>(HintType::BACKING_TRACK));
      }
    };

    if (track == 4 || track == 5) {
      // 声道切换：4=左声道（伴唱），5=右声道（原唱）
      Json::Value channelParam(Json::objectValue);
      channelParam["layerId"] = audioLayerId;
      channelParam["action"] = "set_audioChannel";
      channelParam["audioChannel"] = (track == 5) ? "right" : "left";
      bool ok = dispatchPlaybackCommand(channelParam);
      if (ok) showSwitchTrackHint();
      return ok;
    }

    if (track == 2 || track == 3) {
      auto *audioLayer = dynamic_cast<LayerVideo *>(
          mubu_ ? mubu_->getLayer(audioLayerId) : nullptr);
      int trackCount = audioLayer ? audioLayer->getAudioTrackCount() : 0;
      int mappedAudioTrack = track == 2 ? 1 : 0;
      if (mappedAudioTrack < 0 || mappedAudioTrack >= trackCount) {
        LOG_INFO("[OnlineVod] Skip track switch: requested=%d mappedAudioTrack=%d available=%d",
                 track, mappedAudioTrack, trackCount);
        showSwitchTrackHint();
        return true;
      }
    }
    int audioTrack = track == 2 ? 1 : (track == 3 ? 0 : track);
    cmdParam["action"] = "switch_audioTrack";
    cmdParam["audioTrack"] = audioTrack;
    bool ok = dispatchPlaybackCommand(cmdParam);
    if (ok) showSwitchTrackHint();
    return ok;
  }

  if (action == "SkipSong") {
    cmdParam["action"] = "stop";
    return dispatchPlaybackCommand(cmdParam);
  }

  if (action == "Next") {
    LOG_WARN("[OnlineVod] Next command is not supported, use SkipSong instead");
    return false;
  }

  return false;
}

void Engine::stopOnlineVodSync() {
  bool stoppedAny = false;
  if (onlineVodWsClient_ && onlineVodWsClient_->isRunning()) {
    onlineVodWsEverConnected_.store(false);
    onlineVodWsClient_->stop();
    stoppedAny = true;
  }
  lastOnlineVodWsHost_.clear();
  lastOnlineVodWsPort_ = 0;
  lastOnlineVodWsRoomId_.clear();
  if (onlineVodHttpSyncClient_ && onlineVodHttpSyncClient_->isRunning()) {
    onlineVodHttpSyncClient_->stop();
    stoppedAny = true;
  }
  if (stoppedAny) LOG_INFO("OnlineVodSync: stop");
}

void Engine::applyVodConfigNow() {
  if (!systemConfig_) return;
  const int vodMode = systemConfig_->getVodMode();
  const bool anyVodEnabled = systemConfig_->isAnyVodEnabled();
  const bool localVodEnabled = systemConfig_->isLocalVodEnabled();
  const bool networkVodEnabled = systemConfig_->isNetworkVodEnabled();
  const std::string host = systemConfig_->getOnlineVodHost();
  const int port = 9898;
  const std::string roomId = systemConfig_->getOnlineVodRoomId().empty() ? "current" : systemConfig_->getOnlineVodRoomId();

  lastVodEnabled_ = anyVodEnabled;
  lastOnlineVodHost_ = host;
  lastOnlineVodPort_ = port;
  lastOnlineVodRoomId_ = roomId;
  hsvj::NetworkManager::getInstance().setVodEnabled(anyVodEnabled);

  const int localVodLayer = systemConfig_->getVodLayerId() > 0
                                ? systemConfig_->getVodLayerId()
                                : 1;
  LOG_INFO("[VOD] apply config: vodMode=%d localLayer=%d host=%s port=%d roomId=%s",
           vodMode, localVodLayer, host.c_str(), port, roomId.c_str());

  if (localVodEnabled) {
    if (!localSongDatabase_) {
      const std::string runtimeSongDbPath = DB_DIR + "song.db";
      localSongDatabase_ = std::make_unique<LocalSongDatabase>();
      if (!localSongDatabase_->initialize(runtimeSongDbPath)) {
        LOG_WARN("[VOD] LocalSongDatabase hot init failed: %s", runtimeSongDbPath.c_str());
        localSongDatabase_.reset();
      } else {
        LOG_INFO("[VOD] LocalSongDatabase hot init ok: %s", runtimeSongDbPath.c_str());
      }
    }
    if (!localVodDatabase_) {
      localVodDatabase_ = std::make_unique<LocalVodDatabase>();
      if (!localVodDatabase_->initialize(PLAYLIST_DB_PATH)) {
        LOG_WARN("[VOD] LocalVodDatabase hot init failed: %s", PLAYLIST_DB_PATH.c_str());
        localVodDatabase_.reset();
      }
    }
    if (!localVodManager_ && localSongDatabase_ && localVodDatabase_ && mubu_) {
      localVodManager_ = std::make_unique<LocalVodManager>();
      if (!localVodManager_->initialize(localVodDatabase_.get(), localSongDatabase_.get(), mubu_.get())) {
        LOG_WARN("[VOD] LocalVodManager hot init failed");
        localVodManager_.reset();
      }
    }
    if (localVodManager_) {
      if (!localVodManager_->setTargetLayerId(localVodLayer)) {
        LOG_WARN("[VOD] failed to apply local VOD target layer: %d", localVodLayer);
      } else {
        LOG_INFO("[VOD] local VOD target layer applied: %d", localVodLayer);
      }
    }
    if (!localVodPlayer_ && localVodManager_ && mubu_) {
      localVodPlayer_ = std::make_unique<LocalVodPlayer>();
      if (!localVodPlayer_->initialize(localVodManager_.get(), mubu_.get(), systemConfig_.get())) {
        LOG_WARN("[VOD] LocalVodPlayer hot init failed");
        localVodPlayer_.reset();
      } else {
        localVodPlayer_->enable();
      }
    }
  }

  if (!networkVodEnabled) {
    stopOnlineVodSync();
  } else if (!host.empty()) {
    startOnlineVodSync(host, port, roomId);
  } else {
    LOG_WARN("[VOD] network mode enabled but OnlineVodHost is empty");
  }
}

void Engine::onOnlineVodStateChanged(const VodDatabase::OnlineVodState& st) {
  // command-only：播放控制统一走 command；这里仅负责歌曲同步播放与必要提示
  if (!mubu_ || !systemConfig_) return;
  HintType hint = HintType::NONE;

  const bool hadPreviousRoomState = onlineVodStateSeen_;
  bool roomStatePlaySubmitted = false;
  if (!onlineVodStateSeen_) {
    onlineVodStateSeen_ = true;
    // 首次不预设 lastOnlineVodSongId_，确保 songChanged=true 触发播放
  }

  // 保存旧 songId，供函数末尾 hint 判断使用（播放分支会提前更新 lastOnlineVodSongId_）
  const std::string prevSongId = lastOnlineVodSongId_;

  // roomStateChanged 驱动播放：song 非空 -> 直接播放；song 为空 -> stop
  LOG_DEBUG("[OnlineVod] config: vodMode=%d", systemConfig_->getVodMode());
  if (systemConfig_->isNetworkVodEnabled()) {
    int vodLayerId = systemConfig_->getVodLayerId();
    if (vodLayerId < 1) vodLayerId = 1;
    Layer* layer = mubu_->getLayer(vodLayerId);
    LOG_DEBUG("[OnlineVod] config check: networkVod=true vodLayerId=%d layer=%s",
             vodLayerId, layer ? (layer->getType() == LayerType::VIDEO ? "VIDEO" : "NOT_VIDEO") : "NULL");
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* vl = static_cast<LayerVideo*>(layer);
      LOG_DEBUG("[OnlineVod] roomStateChanged: playState=%d songId=%s title=%s",
               st.playState, st.currentSongId.c_str(), st.currentSongTitle.c_str());

      // 核心逻辑：只关心 songPath，有路径就播，没有就不动
      // 从 rawJson 中提取 playingNow.songPath
      std::string songPath;
      {
        Json::Value root;
        std::string err;
        if (JsonUtils::parseJson(st.rawJson, root, err)) {
          const Json::Value* playingNow = nullptr;
          if (root.isMember("playingNow") && root["playingNow"].isObject()) {
            playingNow = &root["playingNow"];
          }
          if (playingNow) {
            songPath = playingNow->get("songPath", "").asString();
          }
        }
      }

      const bool invalidSongPath = songPath.empty() || songPath.back() == '/';
      if (invalidSongPath) {
        LOG_WARN("[OnlineVod] invalid songPath in roomState, will request skip if playback request fails: songId=%s title=%s path=%s",
                 st.currentSongId.c_str(), st.currentSongTitle.c_str(), songPath.c_str());
        requestOnlineVodSkipAsync("invalid_room_state_song_path");
        goto hint_section;
      }
      {
        // 去重：若图层正在播放同一路径，直接跳过，避免打断正在播放的视频
        // 额外保留 500ms 防抖兜底，防止服务器短时间内重复推送同一路径触发两次 open
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
        const bool incomingIdleFreesong =
            st.currentSongId == "IDLE" ||
            st.currentSongTitle.find("空闲") != std::string::npos;
        const std::string currentLayerPathForIdle =
            FileUtils::normalizePath(vl->getCurrentPath());
        const std::string incomingLayerPathForIdle =
            FileUtils::normalizePath(songPath);
        if (incomingIdleFreesong &&
            !incomingLayerPathForIdle.empty() &&
            currentLayerPathForIdle == incomingLayerPathForIdle &&
            (vl->getState() == LayerVideo::PlayState::PLAYING ||
             vl->getState() == LayerVideo::PlayState::PAUSED)) {
          LOG_INFO("[OnlineVod] idle freesong already playing, skip roomState interrupt: current=%s incoming=%s",
                   currentLayerPathForIdle.c_str(), songPath.c_str());
          goto hint_section;
        }
        if (!incomingIdleFreesong) {
          refreshOnlineVodQueueAndApplyAsync("room_state_changed");
        }
        if (!invalidSongPath && songPath == lastOnlineVodSongPath_) {
          // 先检查图层是否正在播放该路径（最优先）
          std::string normalizedSongPath = FileUtils::normalizePath(songPath);
          std::string currentLayerPath = FileUtils::normalizePath(vl->getCurrentPath());
          bool isAlreadyPlaying = (!currentLayerPath.empty() &&
                                   currentLayerPath == normalizedSongPath &&
                                   vl->getState() == LayerVideo::PlayState::PLAYING);
          if (isAlreadyPlaying) {
            LOG_INFO("[OnlineVod] same songPath already playing, skip: %s", songPath.c_str());
            goto hint_section;
          }
          // 图层已停止（播完）时：允许重播，不做防抖拦截
          bool isStopped = (vl->getState() == LayerVideo::PlayState::STOPPED);
          if (!isStopped) {
            // 图层未停止且路径相同，500ms 防抖兜底（防止连续推送触发两次 open）
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - onlineVodLastSwitchTime_).count();
            if (elapsed < 500) {
              LOG_INFO("[OnlineVod] debounce: same songPath within 500ms (%lldms), skip",
                       (long long)elapsed);
              goto hint_section;
            }
          }
        }
        onlineVodLastSwitchTime_ = now;
        lastOnlineVodSongPath_ = songPath;
      }
      LOG_DEBUG("[OnlineVod] songPath=%s, executing server order", songPath.c_str());
      // 收到推送直接播放，不做去重
      lastOnlineVodSongId_ = st.currentSongId;
      roomStatePlaySubmitted = true;
      {
        const VodDatabase::OnlineVodState roomState = st;
        const std::string songTitle = st.currentSongTitle;
        const bool showRoomStateSwitchHint = hadPreviousRoomState;
        trackEngineAsyncTask(std::async(std::launch::async, [this, vodLayerId, roomState, songTitle, showRoomStateSwitchHint]() {
          if (shuttingDown_.load()) return;
          bool played = tryPlayOnlineVodStateSongOnLayer(vodLayerId, roomState);
          if (!played) {
            LOG_WARN("[OnlineVod] play failed for '%s', requesting skip from server", songTitle.c_str());
            requestOnlineVodSkipAsync("room_state_play_failed");
            return;
          }
          if (showRoomStateSwitchHint && systemConfig_ && mubu_ &&
              systemConfig_->hasLayerConfig(41)) {
            Layer* layer41 = mubu_->getLayer(41);
            if (layer41 && layer41->getType() == LayerType::TEXT) {
              LayerText* hintLayer = static_cast<LayerText*>(layer41);
              hintLayer->setVisible(true);
              hintLayer->showOperationHint(HintType::NEXT, "");
            }
          }
        }));
      }
    }
  }
  hint_section:

  // 用函数入口保存的 prevSongId 判断歌曲是否切换，
  // 避免 lastOnlineVodSongId_ 已在播放分支提前更新为 st.currentSongId 导致比较失效。
  if (!roomStatePlaySubmitted && hadPreviousRoomState && st.currentSongId != prevSongId) {
    if (!st.currentSongId.empty() && st.currentSongId != "IDLE") {
      hint = HintType::NEXT;
    }
    onlineVodLastFinishedReportedSongId_.clear();
    LOG_INFO("OnlineVodStateChanged: songId changed (%s -> %s)",
             prevSongId.c_str(), st.currentSongId.c_str());
  }
  // 播放分支未触发时（songPath 不变/无路径），在这里补充更新 lastOnlineVodSongId_
  if (lastOnlineVodSongId_ != st.currentSongId) {
    lastOnlineVodSongId_ = (st.currentSongId == "IDLE") ? "" : st.currentSongId;
  }

  if (hint == HintType::NONE) return;

  LOG_INFO("OnlineVodStateChanged: hint=%d playState=%d volume=%d mute=%d song=%s",
           static_cast<int>(hint), st.playState, st.volume, st.muteStatus,
           st.currentSongId.c_str());

  if (systemConfig_->hasLayerConfig(41)) {
    Layer* layer41 = mubu_->getLayer(41);
    if (layer41 && layer41->getType() == LayerType::TEXT) {
      LayerText* hintLayer = static_cast<LayerText*>(layer41);
      hintLayer->setVisible(true);
      hintLayer->showOperationHint(hint, "");
    }
  }
}

void Engine::run() {
  if (!initialized_.load()) {
    LOG_ERROR("Engine not initialized");
    return;
  }
  // Android 下由应用层 Choreographer/Surface 驱动每帧调用 update()，此处无需主循环
}

void Engine::update(float deltaTime) {
  // 如果正在关闭，立即返回，不执行任何更新逻辑
  if (shuttingDown_.load()) {
    return;
  }

  // 如果还在初始化中，等待初始化完成后再执行业务逻辑
  if (!initialized_.load()) {
    return;
  }

  static int updateExceptionLogCount = 0;
  auto logUpdateException = [&](const char* stage, const char* type, const char* message) {
    ++updateExceptionLogCount;
    if (updateExceptionLogCount <= 20 || updateExceptionLogCount % 300 == 0) {
      LOG_ERROR("[Engine::update] stage=%s %s: %s",
                stage ? stage : "unknown",
                type ? type : "exception",
                message ? message : "");
    }
  };
  auto runStage = [&](const char* stage, auto&& fn) -> bool {
    try {
      fn();
      return true;
    } catch (const std::bad_alloc& e) {
      logUpdateException(stage, "bad_alloc", e.what());
    } catch (const std::exception& e) {
      logUpdateException(stage, "exception", e.what());
    } catch (...) {
      logUpdateException(stage, "unknown exception", "");
    }
    return false;
  };

  runStage("syncActiveVideoFramePools", [&]() { syncActiveVideoFramePools(); });
  runStage("syncAudioOutputLayer", [&]() { syncAudioOutputLayer(); });
  runStage("logVideoLayerDiagnostics", [&]() { logVideoLayerDiagnostics(); });

  runStage("performMemoryMonitorTick", [&]() { performMemoryMonitorTick(); });

  if (!initialized_.load()) {
    return;
  }

  // 更新授权管理器（检查到期提示）
  runStage("licenseManager", [&]() {
    if (!licenseManager_) return;
    licenseManager_->update(deltaTime);
    // 注意：license warning stage 会在 renderFrame() 中通过 RenderContext 传递
  });

  // 更新图层
  runStage("mubu.updateLayers", [&]() {
    if (mubu_) {
      mubu_->updateLayers(deltaTime);
    }
  });

  // OnlineVod WS 线程写入的待执行项（Stop / Play 媒体 / 外设类 command）在主线程消费
  runStage("processOnlineVodServerPendingActions", [&]() {
    processOnlineVodServerPendingActions();
  });

  runStage("updateRoamConfigTick", [&]() { updateRoamConfigTick(); });

  runStage("sceneManager.update", [&]() {
    if (sceneManager_) {
      sceneManager_->update(deltaTime);
    }
  });

  // 更新效果管理器时间（用于音频效果动画）
  runStage("effectManager.updateTime", [&]() {
    if (effectManager_) {
      effectManager_->updateTime(deltaTime);
    }
  });

  runStage("dmxChannelHandler.update", [&]() {
    if (dmxChannelHandler_) {
      dmxChannelHandler_->update();
    }
  });

  runStage("updateCaptureSignalTick", [&]() { updateCaptureSignalTick(); });

  runStage("updateCaptureAutoSceneSwitchTick", [&]() {
    updateCaptureAutoSceneSwitchTick();
  });

  runStage("updateLyricTick", [&]() { updateLyricTick(); });

  // 更新播放列表提示图层（基于视频剩余时间自动显示/隐藏）
  runStage("updatePlaylistHintLayer", [&]() { updatePlaylistHintLayer(); });

  if (systemConfig_ && systemConfig_->isLocalVodEnabled() && localVodPlayer_) {
    runStage("localVodPlayer.update", [&]() { localVodPlayer_->update(); });
  } else {
    // 检查视频播放完成并自动播放下一个（循环全部模式）
    runStage("checkAndPlayNextVideo", [&]() { checkAndPlayNextVideo(); });
  }

  runStage("enforceLicensePlaybackBlockTick", [&]() {
    enforceLicensePlaybackBlockTick();
  });

  // 渲染一帧
  static int renderFrameCount = 0;
  if (renderFrameCount < 20 || renderFrameCount % 60 == 0) {
    LOG_DEBUG("[RenderLoop] Engine::update calling renderFrame, count=%d", renderFrameCount);
  }
  runStage("renderFrame", [&]() { renderFrame(); });
  renderFrameCount++;
}

void Engine::enforceLicensePlaybackBlockTick() {
  // 15天后停止所有视频播放
  if (!licenseManager_ || !licenseManager_->shouldBlockVideoPlayback() || !mubu_) return;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
        PlaybackRequestDispatcher::stopLayer(mubu_.get(), layerId);
      }
    }
  }
}

// 预创建授权硬件图层池中的所有图层
//
// 架构说明：
// - license.dat: 定义当前授权可用的图层 ID
// - config.json: 决定实际启用的图层和运行时参数（visible、size、position 等）
//
// 此函数职责：
// 1. 从 License管理器 获取授权图层 ID
// 2. 根据内置 LayerDefinitions 预创建所有授权硬件图层
// 3. 预创建的图层默认设置为不可见（setVisible(false)）
// 4. 实际的图层参数（可见性、尺寸、位置等）由后续 createLayersFromConfig() 应用
//
// 未在 config 中配置的图层保持隐藏，不应用大屏运行参数
void Engine::preCreateAuthorizedLayers() {
  if (!licenseManager_) {
    LOG_WARN("[Step 3.1] LicenseManager 未初始化，跳过预创建");
    return;
  }

  std::vector<LayerDefinition> definitions =
      getAuthorizedLayerDefinitions(licenseManager_->getEnabledLayerIds());
  if (definitions.empty()) {
    LOG_WARN("[Step 3.1] 授权图层池为空，跳过预创建");
    return;
  }

  size_t totalLayers = definitions.size();
  int createdCount = 0;

  for (size_t i = 0; i < totalLayers; ++i) {
    const LayerDefinition &definition = definitions[i];
    int layerId = definition.id;
    LayerType type = definition.type;

    if (!mubu_->getLayer(layerId)) {
      if (mubu_->createLayer(layerId, type)) {
        Layer *layer = mubu_->getLayer(layerId);
        if (layer) {
          // 预创建的图层默认设置为不可见
          // 实际的可见性由 createLayersFromConfig() 从 config.json 中应用
          layer->setVisible(false);
          // 注意：渲染器_ 在 Step 4.2 才初始化，这里先不设置
          // 会在 Step 4.2 后统一为所有图层设置 渲染器_
          createdCount++;
        }
      }
    }

    // 主动出让 CPU 时间片，平滑 I/O 和内存分配压力
    if ((i + 1) % 4 == 0 && i < totalLayers - 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  if (createdCount > 0) {
    mubu_->sortLayersByPriority(true);
  }

  // USB/Mirror sources render through the app-owned MIRROR layer. Some older
  // licenses omit layer 31 even when config.json explicitly enables it, so keep
  // the layer dormant by default but create it when the runtime config asks for
  // layer31. This does not enable mirroring unless config.json contains layer31.
  if (systemConfig_ && systemConfig_->hasLayerConfig(31) && !mubu_->getLayer(31)) {
    if (mubu_->createLayer(31, LayerType::MIRROR)) {
      Layer *layer = mubu_->getLayer(31);
      if (layer) {
        layer->setVisible(false);
      }
      LOG_INFO("[Step 3.1] Config requested MIRROR layer31; created fallback LayerMirror");
      mubu_->sortLayersByPriority(true);
    } else {
      LOG_WARN("[Step 3.1] Config requested MIRROR layer31 but fallback creation failed");
    }
  }
}

// 根据 config.json 配置应用图层参数
//
// 架构说明：
// - 此函数在 preCreateAuthorizedLayers() 之后调用
// - preCreateAuthorizedLayers() 已根据授权图层池预创建了图层骨架
// - 此函数负责从 config.json 读取并应用实际的运行时参数
//
// 应用的参数包括：
// - 基本属性：visible、position、size、rotation、scale、alpha、priority
// - 图层特定属性：视频图层的音量、音轨；文本图层的字体、颜色等
// - 切片配置：从 config.json 读取的切片会应用到图层
//
// 注意：只有在 config.json 中配置的图层才会被应用参数并启用
void Engine::createLayersFromConfig() {
#ifdef USE_NEW_LAYER_INIT
  // 使用新的图层初始化器框架（重构版本）
  createLayersFromConfigNew();
#else
  // 使用旧的图层初始化方式（原版本）
  createLayersFromConfigOld();
#endif
}

// 新的图层初始化方法（使用工厂模式）
void Engine::createLayersFromConfigNew() {
  LOG_INFO("[Engine] Step 3.2: Loading layer configurations (NEW initializer framework)");

  if (!mubu_ || !systemConfig_) {
    LOG_ERROR("[Engine] Cannot create layers: mubu or systemConfig is null");
    return;
  }

  // 构建初始化上下文
  LayerInitContext context(
      mubu_.get(),           // 图层管理器
      systemConfig_.get(),   // 系统配置
      renderer_.get(),       // 渲染器
      ROOT_PATH,             // 根路径
      FONT_DIR,              // 字体目录
      LYRICS_DIR             // 歌词目录
  );

  // 使用工厂批量初始化所有图层
  LayerInitializerFactory& factory = LayerInitializerFactory::getInstance();
  int initializedCount = factory.initializeAllLayers(context);

  LOG_INFO("[Engine] Layer initialization complete: %d layers initialized using new framework",
           initializedCount);
}

// 旧的图层初始化方法（保留用于回退）
void Engine::createLayersFromConfigOld() {
  const auto &configLayers = systemConfig_->getAllLayerConfigs();

  if (!configLayers.empty()) {
    std::vector<int> createOrder;
    if (configLayers.find(21) != configLayers.end()) {
      createOrder.push_back(21);
    }

    for (const auto &pair : configLayers) {
      if (pair.first != 21) {
        createOrder.push_back(pair.first);
      }
    }

    for (int layerId : createOrder) {
      auto it = configLayers.find(layerId);
      if (it == configLayers.end())
        continue;
      const LayerConfigData &config = it->second;

      // 图层0是水印图层，允许创建
      if (layerId < 0) {
        LOG_WARN("[Step 3.2] 跳过无效的图层ID: %d", layerId);
        continue;
      }

      if (licenseManager_ && !isAuthorizedLayerId(licenseManager_->getEnabledLayerIds(), layerId)) {
        LOG_WARN("[Step 3.2] Config.json 引用了未授权图层 %d，跳过", layerId);
        continue;
      }

      // 获取图层（所有图层必须在 Step 3.1 从授权图层池预创建）
      Layer *layer = mubu_->getLayer(layerId);

      if (!layer) {
        // 严格模式：如果不属于授权图层池，视为非法配置
        // 授权图层池定义系统可用图层，config.json 只能引用已授权图层
        LOG_WARN("[Step 3.2] Config.json 引用了未定义或未授权图层 %d，跳过", layerId);
        continue;
      }

      LayerType layerType = layer->getType();

      // 无论是预创建还是新创建，都应用 config.json 中的配置参数
      if (layer) {
        // 设置通用属性（带验证）
        // 注意：这些参数来自 config.json，会覆盖授权图层定义中的默认值
        if (layerId == 71) {
          LOG_DEBUG("[图层71诊断] 从config.json读取visible=%s, image_file对应路径='%s', qrContent='%s'",
                   config.visible ? "true" : "false",
                   config.imagePath.c_str(),
                   config.qrContent.c_str());
        }
        // 应用 config.json 中的可见性设置（这是实际决定图层是否显示的配置）
        // 所有图层（包括采集图层）都严格遵循 config.visible 配置
        layer->setVisible(layerId == 60 ? false : config.visible);
        // 采集图层 10/11：只要出现在 config 中，就作为采集层渲染和自动启动。
        if (layerId == 10 || layerId == 11) {
          LayerVideo* videoLayer = dynamic_cast<LayerVideo*>(layer);
          if (videoLayer) {
            videoLayer->setConfiguredCaptureLayer(true);
          }
          LOG_INFO("  [采集诊断] createLayersFromConfig 图层 %d: config.visible=%d captureType=%s size=%dx%d",
                   layerId, config.visible ? 1 : 0,
                   (config.captureType.empty() ? "AUTO" : config.captureType.c_str()),
                   config.size.width, config.size.height);
        }

        // 设置位置（允许负坐标，图层可以部分或完全在屏幕外）
        layer->setPosition(config.position);

        // 所有尺寸必须从 config.json 获取，不能有任何硬编码和猜想
        // config.json 是图层参数的唯一权威来源
        if (config.size.width > 0 && config.size.height > 0) {
          layer->setSize(config.size);
        } else {
          // config.json中没有配置尺寸，保持为0（不设置任何默认值）
          // 图层尺寸必须由config.json明确配置
          layer->setSize(Size(0, 0));
          if (layerType == LayerType::VIDEO) {
            LOG_WARN("[Step 3.2] 图层 %d (视频图层) 在config.json中未配置尺寸，将无法正确渲染。请确保config.json中包含该图层的size配置", layerId);
          }
        }

        // 验证旋转角度和缩放
        if (config.rotation >= -360.0f && config.rotation <= 360.0f) {
          layer->setRotation(config.rotation);
        }

        if (config.scale > 0.0f && config.scale <= 10.0f) {
          layer->setScale(config.scale);
        } else {
          layer->setScale(1.0f);
        }

        if (config.alpha >= 0.0f && config.alpha <= 1.0f) {
          layer->setAlpha(config.alpha);
        } else {
          layer->setAlpha(1.0f);
        }

        // 设置优先级
        layer->setPriority(config.priority);

        // 设置几何遮罩参数
        layer->setShapeType(config.shapeType);
        layer->setShapeParam(config.shapeParam);
        layer->setBlackToTransparent(config.blackToTransparent);
        layer->setEffectLinkedSlices(
            config.effectLinkedSlices); // 应用效果关联切片参数
        layer->setInvert(config.invert);
        layer->setGaussianBlur(config.gaussianBlur); // 应用高斯模糊参数

        // 加载切片配置（从config.json读取的切片会应用到图层）
        if (!config.slices.empty()) {
          for (const auto &slicePair : config.slices) {
            const std::string &sliceKey = slicePair.first;
            const SliceConfig &sliceConfig = slicePair.second;

            // 将 SliceConfig 转换为 Json::Value
            layer->setSlice(sliceKey, sliceConfigToJson(sliceConfig));
          }
        }

        // 注意：在初始化流程中，渲染器_ 在 Step 4.2 才初始化
        // 这里设置 渲染器_ 是为了兼容后续可能动态创建的图层
        // 在 Step 4.2 后会统一为所有图层设置 渲染器_
        if (renderer_) {
          layer->setRenderer(renderer_.get());
        }

        // 根据图层类型设置特定属性
        if (layerType == LayerType::VIDEO) {
          LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);

          // 注意：path 字段已删除，播放现在完全基于播放列表。
          if (config.playbackRate > 0.0f && config.playbackRate <= 4.0f) {
            videoLayer->setPlaybackRate(config.playbackRate);
          }

          if (config.volume >= 0.0f && config.volume <= 1.0f) {
            videoLayer->setVolume(config.volume);
            LOG_INFO("[播放器] LayerVideo %d volume loaded from config: %.2f", layerId, config.volume);
            if (systemConfig_ && layerId == systemConfig_->getAudioOutputLayerId()) {
              LOG_INFO("[Audio] output layer %d volume from config: %.2f", layerId, config.volume);
            }
          }

          if (config.audioTrack > 0) {
            videoLayer->switchAudioTrack(config.audioTrack);
          }
          videoLayer->setAudioChannel(config.audioChannel);
          videoLayer->setAudioEffectType(config.effectLinkedSlices ? config.audioEffectType : 0);
          videoLayer->setAudioEffectStackPacked(
              config.effectLinkedSlices ? config.audioEffectStackPacked : 0);
          videoLayer->setAudioEffectColorPacked(config.audioEffectColor);
          videoLayer->setAudioEffectWidth(config.audioEffectWidth);

          if (layerId == 10 || layerId == 11) {
            videoLayer->setConfiguredCaptureLayer(true);
            videoLayer->setCaptureType(config.captureType.empty() ? "AUTO" : config.captureType);
            videoLayer->setCaptureRotation(config.captureRotation);
            videoLayer->setFitMode(std::clamp(config.fitMode, 0, 1));
          }

          // 歌词只做图层21和绑定视频层之间的时间回调，和采集显示参数没有关系。
          if (systemConfig_->isLyricEnabled() && systemConfig_->hasLayerConfig(21)) {
            Layer *lyricLayer = mubu_->getLayer(21);
            if (lyricLayer && lyricLayer->getType() == LayerType::TEXT) {
              LayerText *lyricTextLayer = static_cast<LayerText *>(lyricLayer);
              if (layerId == lyricTextLayer->getBindLayerId()) {
                lyricTextLayer->setCurrentTimeCallback([videoLayer]() {
                  return videoLayer->getCurrentPosition();
                });

                Size lyricSize = lyricLayer->getSize();
                if (lyricSize.width > 0 && lyricSize.height > 0) {
                  lyricTextLayer->setLyricRenderSize(lyricSize.width,
                                                     lyricSize.height);
                }

                if (std::string(LYRICS_DIR).empty()) {
                  LOG_WARN("[Step 3.2] 图层21 歌词目录未配置，歌词可能无法自动加载");
                }
              }
            }
          }
        } else if (layerType == LayerType::MIRROR) {
          LayerMirror *mirrorLayer = static_cast<LayerMirror *>(layer);
          mirrorLayer->setFitMode(std::clamp(config.fitMode, 0, 1));
          mirrorLayer->setReadyHintVisible(config.mirrorReadyHintVisible);
          mirrorLayer->setTvVerticalCropPx(config.tvVerticalCropPx);
        } else if (layerType == LayerType::IMAGE || layerType == LayerType::QRCODE) {
          LayerImage *imageLayer = static_cast<LayerImage *>(layer);

          // 在加载图片前先设置属性
          imageLayer->setFilterMode(config.filterMode);
          imageLayer->setFadeInTime(config.fadeInTime);
          imageLayer->setFadeOutTime(config.fadeOutTime);

          // Layer 70 (Logo) 和 Layer 71 (二维码) 是常驻图层，禁用自动淡出
          // displayDuration = 0 表示无限期显示
          if (layerId == 70 || layerId == 71) {
            imageLayer->setDisplayDuration(0.0f);
            imageLayer->setFadeInTime(0.0f); // 也可以禁用淡入，使其立即显示
          } else {
            imageLayer->setDisplayDuration(config.displayDuration);
          }

          // 尊重配置文件中的 animated 设置（用户可通过前端切换静态/动态模式）
          imageLayer->setAnimated(config.animated);

          // 图层71（二维码）使用当前根目录路径（ROOT_PATH 为 /huoshan/ 或 /sdcard/huoshan/ 等）
          if (layerId == 71) {
            std::string qrPath = ROOT_PATH + "QRCode/qrcode_71.png";
            std::string normalizedQRPath = FileUtils::normalizePath(qrPath);
            if (FileUtils::exists(normalizedQRPath) && imageLayer->loadImage(normalizedQRPath)) {
              if (config.size.width > 0 && config.size.height > 0) {
                layer->setSize(config.size);
              }
              layer->setPosition(config.position);
            }
            // 文件不存在时不打日志：初始化时可能尚未生成，后续 Scene管理器/重试 会再加载，且二维码已显示说明某次加载已成功
          }
          // 其他图片图层（非 Logo/QRCode）：imagePath 由 SystemConfig 从 image_file 拼出（ROOT_PATH+Image/+文件名）
          // Layer70 (Logo): 不使用 imagePath，始终从 logo/ 目录硬编码加载（见下方）
          // Layer71 (QRCode): 已在上方单独处理
          else if (layerId != 70 && !config.imagePath.empty()) {
            std::string imagePath = config.imagePath;

            // 处理相对路径：统一基于 ROOT_PATH（/huoshan/ 或 /sdcard/huoshan/）
            if (imagePath.length() > 0 && imagePath[0] != '/' &&
                imagePath[0] != '.') {
              if (imagePath.find("huoshan/") == 0) {
                imagePath = ROOT_PATH + imagePath.substr(8);
              } else if (imagePath.find("Logo/") == 0 ||
                       imagePath.find("QRCode/") == 0 ||
                       imagePath.find("qrcode/") == 0 ||
                       imagePath.find("Image/") == 0 ||
                       imagePath.find("image/") == 0) {
                imagePath = ROOT_PATH + imagePath;
              }
            }

            std::string normalizedImagePath =
                FileUtils::normalizePath(imagePath);

            if (FileUtils::exists(normalizedImagePath)) {
              if (imageLayer->loadImage(normalizedImagePath)) {
                if (config.size.width > 0 && config.size.height > 0) {
                  layer->setSize(config.size);
                }
                layer->setPosition(config.position);
              } else {
                // 同一图层+路径失败只打一次日志（路径由 image_file 与 ROOT_PATH 拼出，配置文件不再使用路径）
                static std::set<std::pair<int, std::string>> s_imageLoadFailLogged;
                auto key = std::make_pair(layerId, normalizedImagePath);
                if (s_imageLoadFailLogged.find(key) == s_imageLoadFailLogged.end()) {
                  s_imageLoadFailLogged.insert(key);
                  LOG_WARN("图层 %d 图片文件存在但加载失败（仅提示一次）: %s", layerId, normalizedImagePath.c_str());
                }
              }
            } else {
              static std::set<std::pair<int, std::string>> s_imagePathMissingLogged;
              auto key = std::make_pair(layerId, normalizedImagePath);
              if (s_imagePathMissingLogged.find(key) == s_imagePathMissingLogged.end()) {
                s_imagePathMissingLogged.insert(key);
                LOG_WARN("图层 %d 配置的图片路径不存在（仅提示一次）: %s", layerId, normalizedImagePath.c_str());
              }
            }
          }

          // Layer70 (Logo) 从 Logo/ 目录加载，不依赖 config.json 的 image_file
          if (layerId == 70) {
            std::string logoPath = ROOT_PATH + "Logo/logo.png";
            LOG_DEBUG("[Logo诊断] 尝试路径: %s, exists=%s", logoPath.c_str(),
                     FileUtils::exists(logoPath) ? "true" : "false");
            if (FileUtils::exists(logoPath)) {
              bool loadOk = imageLayer->loadImage(logoPath);
              LOG_DEBUG("[Logo诊断] loadImage(%s) = %s, width=%d, height=%d",
                       logoPath.c_str(), loadOk ? "成功" : "失败",
                       imageLayer->getWidth(), imageLayer->getHeight());
              if (loadOk) {
                if (config.size.width > 0 && config.size.height > 0) {
                  layer->setSize(config.size);
                } else {
                  layer->setSize(Size(1920, 1080));
                  LOG_WARN("[Logo诊断] config.json中未配置尺寸，使用默认 1920x1080");
                }
                layer->setPosition(config.position);
                LOG_DEBUG("[Logo诊断] Logo 已加载并设置: visible=%s, size=%dx%d, pos=%d,%d",
                         layer->isVisible() ? "true" : "false",
                         layer->getSize().width, layer->getSize().height,
                         layer->getPosition().x, layer->getPosition().y);
              }
            } else {
              LOG_WARN("[Logo诊断] Logo 文件不存在: %sLogo/logo.png", ROOT_PATH.c_str());
            }
          }
        } else if (layerType == LayerType::TEXT) {
          LayerText *textLayer = static_cast<LayerText *>(layer);
          // LayerText 创建阶段已通过 Mubu 注入 sharedTextOverlayHolder_，此处仅应用配置参数。

          textLayer->setText(config.text);
          // Layer40文本配置日志已清理
          if (layerId == 40 && config.text.empty()) {
            static int logEmpty40 = 0;
            if (logEmpty40++ < 3) {
              LOG_WARN("[L40] 配置加载后 text 为空，Layer40 欢迎词不会显示。请在 config.json 的 layer40 中配置非空 \"text\" 字段，参见 docs/Layer40-Layer41-配置与显示.md");
            }
          }
          if (config.size.width > 0 && config.size.height > 0) {
            layer->setSize(config.size);
          }
          layer->setPosition(config.position);
          // fontPath 从 config.json 读取的仅是文件名，需拼上 FONT_DIR 构成完整路径；
          // 始终设置路径以便重启后 API/页面能返回配置的 font_file，文件存在时用规范路径，不存在时仍写入路径供后续加载
          if (!config.fontPath.empty()) {
            std::string fullFontPath = FONT_DIR + config.fontPath;
            std::string normalizedFontPath =
                FileUtils::normalizePath(fullFontPath);
            textLayer->setFontPath(FileUtils::exists(normalizedFontPath)
                                      ? normalizedFontPath
                                      : fullFontPath);
          }
          textLayer->setFontSize(config.fontSize);
          textLayer->setTextColor(Color::fromString(config.textColor));
          textLayer->setBgColor(Color::fromString(config.bgColor));
          textLayer->setAlignment(static_cast<TextAlignment>(config.alignment));
          // 图层40为独立跑马灯层，不需要 bindLayerId；仅图层21/41从配置加载
          if (layerId != 40) {
            textLayer->setBindLayerId(config.bindLayerId);
          }
          textLayer->setScrollSpeed(config.scrollSpeed);

          // Layer 40 跑马灯使用内置默认值（无描边无阴影），不从配置加载，避免边缘闪烁
          if (layerId != 40) {
            textLayer->setOutlineWidth(config.outlineWidth);
            textLayer->setShadow(config.shadow);
            textLayer->setOutlineColor(Color::fromString(config.outlineColor));
          }

          // 如果是Layer21（歌词图层），更新歌词渲染尺寸
          // 注意：歌词不从配置恢复，而是在视频播放时根据视频文件名自动查找同名歌词
          if (layerId == 21 && layerType == LayerType::TEXT) {
            LayerText *lyricTextLayer = static_cast<LayerText *>(layer);
            Size lyricSize = layer->getSize();
            if (lyricSize.width > 0 && lyricSize.height > 0) {
              lyricTextLayer->setLyricRenderSize(lyricSize.width,
                                                 lyricSize.height);
            }
            // Layer21 以图层 visible 为准，不再单独加载 subtitleVisible
          }

          // Layer 41 (消息提示图层) 特有配置加载
          if (layerId == 41 && layerType == LayerType::TEXT) {
            LayerText *hintLayer = static_cast<LayerText *>(layer);
            if (!config.playlistId.empty()) {
              hintLayer->setPlaylistId(config.playlistId);
            }
            hintLayer->setShowCount(config.showCount);
            hintLayer->setDisplayAlign(config.displayAlign);

            // 注意：SystemConfig中字段是
            // l41DisplayDuration，LayerText中setter是 setDisplayDuration
            // 从图层配置中读取时间参数（不是硬编码）
            hintLayer->setDisplayDuration(config.l41DisplayDuration);
            hintLayer->setStartHintTime(config.startHintTime);
            hintLayer->setEndHintTime(config.endHintTime);
            hintLayer->setShowList(config.l41ShowList);
          }
        }
      }
    }
  }

  if (mubu_) {
    // createLayersFromConfig 会按 config.json 覆盖各图层 priority。
    // 应用完配置后必须重新排序，否则启动初始音频/渲染顺序可能仍沿用预创建顺序。
    mubu_->sortLayersByPriority(true);
  }

  // 删除 config 中未预创建图层的配置并写回 config.json
  if (mubu_ && systemConfig_) {
    std::vector<int> preCreatedIds = mubu_->getAllLayerIds();
    std::unordered_set<int> preCreatedSet(preCreatedIds.begin(), preCreatedIds.end());
    std::vector<int> toRemove;
    for (const auto &p : systemConfig_->getAllLayerConfigs()) {
      if (preCreatedSet.find(p.first) == preCreatedSet.end()) {
        toRemove.push_back(p.first);
      }
    }
    for (int id : toRemove) {
      systemConfig_->removeLayerConfig(id);
    }
    if (!toRemove.empty()) {
      std::string configPath = CONFIG_PATH;
      if (systemConfig_->save(configPath)) {
        LOG_INFO("[Step 3.2] 已从 config.json 移除 %zu 个未预创建图层的配置",
                 toRemove.size());
      }
    }
  }
}

std::vector<int> Engine::getConfiguredLayerIds() const {
  std::vector<int> ids;
  if (!systemConfig_)
    return ids;

  // 从 config.json 获取已配置的图层
  for (const auto &p : systemConfig_->getAllLayerConfigs())
    ids.push_back(p.first);

  if (ids.empty()) {
    LOG_INFO("[getConfiguredLayerIds] config.json 中没有图层配置");
  }

  return ids;
}

} // 命名空间 hsvj
