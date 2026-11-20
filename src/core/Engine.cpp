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
#include "capture/Rk628Edid.h"
#include "core/AudioOutputLayerPolicy.h"
#include "core/InitProgress.h"
#include "core/LayerDefinitions.h"
#include "audio/AudioPlayerManager.h"
#include "audio/AudioProcessor.h"
#include "core/LicenseManager.h"
#include "core/LicenseVerify.h"
#include "foundation/PathConfig.h"
#include "core/PeripheralManager.h"
#include "platform/AssociatedDeviceName.h"
#include "core/StageControlManager.h"
#include "core/SystemConfig.h"
#include "core/StartupConfig.h"
#include "core/OnlineVodDiscovery.h"
#include "network/DeviceDiscoveryService.h"
#include "core/SliceConfigJson.h"
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

long long Engine::getSuccessfulPresentCount() const {
  return renderer_ ? renderer_->getSuccessfulPresentCount() : 0;
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

#ifdef __ANDROID__
bool postDeviceReportAcrossAndroidNetworks(const std::string& url,
                                           const std::string& body,
                                           int timeoutSeconds,
                                           std::string* response) {
  if (!response || !g_jvm || !g_engine_java_object) return false;

  JNIEnv* env = nullptr;
  bool attached = false;
  const jint envResult = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (envResult == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      LOG_WARN("[Step 4.9] 无法附加 JVM 线程，回退普通 HTTP");
      return false;
    }
    attached = true;
  } else if (envResult != JNI_OK || !env) {
    LOG_WARN("[Step 4.9] 无法获取 JNIEnv，回退普通 HTTP");
    return false;
  }

  bool invoked = false;
  jclass engineClass = env->GetObjectClass(g_engine_java_object);
  jmethodID method = engineClass
      ? env->GetMethodID(engineClass, "postDeviceReportAcrossNetworks",
                         "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;")
      : nullptr;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    method = nullptr;
  }

  if (method) {
    jstring jUrl = env->NewStringUTF(url.c_str());
    jstring jBody = env->NewStringUTF(body.c_str());
    jobject result = env->CallObjectMethod(
        g_engine_java_object, method, jUrl, jBody,
        static_cast<jint>(std::max(1, timeoutSeconds) * 1000));
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(jBody);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      LOG_WARN("[Step 4.9] Java 双网络上报抛出异常，回退普通 HTTP");
    } else if (result) {
      invoked = true;
      jstring resultString = static_cast<jstring>(result);
      const char* chars = env->GetStringUTFChars(resultString, nullptr);
      if (chars) {
        *response = chars;
        env->ReleaseStringUTFChars(resultString, chars);
      }
      env->DeleteLocalRef(result);
    }
  }

  if (engineClass) env->DeleteLocalRef(engineClass);
  if (attached) g_jvm->DetachCurrentThread();
  return invoked;
}
#endif

bool isDeviceReportResponseSuccessful(const std::string& response) {
  if (response.empty()) return false;
  Json::Value root;
  return JsonUtils::parseJson(response, root) && root.isObject() &&
         root.isMember("code") && root["code"].asInt() == 0;
}

// Terminal admission carries the exact rights from the signed server.lic.
enum class OnlineVodAdmissionStatus {
  AUTHORIZED,
  RETRYABLE_FAILURE,
  DENIED,
};

struct OnlineVodAdmissionResult {
  OnlineVodAdmissionStatus status = OnlineVodAdmissionStatus::RETRYABLE_FAILURE;
  Json::Value data;
  std::string error;
};

// Persist only routing/mode fields, preserving unrelated device settings.
// FileUtils uses a same-directory temporary file and atomic rename.
bool applyServerOnlineVodMode(SystemConfig& config, const std::string& path,
                             const std::string& host, const Json::Value& admission) {
  if (!requestsOnlineVodSwitch(admission)) return false;
  if (config.isNetworkVodEnabled() && config.getOnlineVodHost() == host) return true;
  if (FileUtils::isFile(path)) {
    Json::Value local, updated;
    if (!JsonUtils::parseJson(FileUtils::readTextFile(path), local) ||
        !prepareOnlineVodConfig(local, host, admission, updated) ||
        !FileUtils::writeTextFile(path, JsonUtils::toFormattedString(updated, 2))) {
      LOG_WARN("[OnlineVod] Auto-switch config save failed; retaining current mode");
      return false;
    }
  }
  // On first boot the normal default-config creation below saves these values.
  config.setOnlineVodHost(host);
  config.setOnlineVodRoomId("current");
  config.setVodMode(2);
  LOG_INFO("[OnlineVod] Server requested online VOD; selected server=%s", host.c_str());
  return true;
}

bool disableUnavailableOnlineVod(SystemConfig& config, const std::string& path) {
  if (!config.isNetworkVodEnabled()) return false;
  if (FileUtils::isFile(path)) {
    Json::Value local, updated;
    if (!JsonUtils::parseJson(FileUtils::readTextFile(path), local) ||
        !prepareOfflineVodConfig(local, updated) ||
        !FileUtils::writeTextFile(path, JsonUtils::toFormattedString(updated, 2))) {
      LOG_WARN("[OnlineVod] Offline fallback config save failed; retaining current mode");
      return false;
    }
  }
  config.setVodMode(0);
  LOG_WARN("[OnlineVod] Server unavailable after 3 admission rounds; VOD disabled, continuing discovery");
  return true;
}

std::string onlineVodServerBaseUrl(const SystemConfig& config, const std::string& serverHost = "") {
  std::string host = serverHost.empty() ? config.getOnlineVodHost() : serverHost;
  while (!host.empty() &&
         (host.back() == '/' || std::isspace(static_cast<unsigned char>(host.back())))) {
    host.pop_back();
  }
  if (host.empty()) {
    return {};
  }
  std::string baseUrl = (host.compare(0, 7, "http://") == 0 || host.compare(0, 8, "https://") == 0) ? host : "http://" + host;
  const size_t schemeEnd = baseUrl.find("://");
  const size_t authorityStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  if (baseUrl.find(':', authorityStart) == std::string::npos) {
    baseUrl += ":" + std::to_string(config.getOnlineVodPort());
  }
  return baseUrl;
}

bool loadServerStartupConfig(std::unique_ptr<SystemConfig>& config, const std::string& path) {
  if (!config->isNetworkVodEnabled()) {
    LOG_INFO("[StartupConfig] Online VOD disabled; using local config");
    return false;
  }
  const auto baseUrl = onlineVodServerBaseUrl(*config);
  if (baseUrl.empty()) return false;
  // Validate with the real config loader before atomically replacing config.json.
  struct StagingFile {
    std::string path;
    ~StagingFile() { std::remove(path.c_str()); }
  } staging{path + ".server-startup"};
  try {
    int status = 0;
    std::string response;
    for (int attempt = 0; attempt < 3; ++attempt) {
      response = httpGet(baseUrl + "/api/v1/terminals/startup-config", 3, "", &status);
      if (status > 0 && status < 500) break;
      if (attempt < 2) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    Json::Value envelope, local(Json::objectValue), merged;
    if (status != 200 || response.size() > 4 * 1024 * 1024 ||
        !JsonUtils::parseJson(response, envelope) || !envelope.isObject() ||
        !envelope["code"].isInt() || envelope["code"].asInt() != 0) {
      LOG_WARN("[StartupConfig] Server unavailable or invalid response (HTTP %d); keeping local config", status);
      return false;
    }
    if (envelope["data"].isNull()) {
      LOG_INFO("[StartupConfig] No server profile; keeping local config");
      return false;
    }
    const auto& data = envelope["data"];
    if (!data.isObject() || !data["mode"].isString() ||
        (data["mode"].asString() != "template" && data["mode"].asString() != "custom") ||
        (FileUtils::isFile(path) && !JsonUtils::parseJson(FileUtils::readTextFile(path), local)) ||
        !prepareStartupConfig(data["config"], local, merged)) {
      LOG_WARN("[StartupConfig] Invalid profile; keeping local config");
      return false;
    }
    SystemConfig::retainOnlyConfigJsonKeys(merged);
    const std::string content = JsonUtils::toString(merged);
    auto candidate = std::make_unique<SystemConfig>();
    if (!FileUtils::writeTextFile(staging.path, content) || !candidate->load(staging.path) ||
        !FileUtils::writeTextFile(path, content)) {
      LOG_WARN("[StartupConfig] Profile validation or save failed; keeping local config");
      return false;
    }
    config = std::move(candidate);
    LOG_INFO("[StartupConfig] Applied %s profile before layer initialization", data["mode"].asCString());
    return true;
  } catch (const std::exception& error) {
    LOG_WARN("[StartupConfig] Invalid profile: %s; keeping local config", error.what());
    return false;
  }
}

OnlineVodAdmissionResult requestOnlineVodAdmission(const SystemConfig& config, const std::string& serverHost = "") {
  OnlineVodAdmissionResult result;
  try {
  const std::string baseUrl = onlineVodServerBaseUrl(config, serverHost);
  if (baseUrl.empty()) {
    result.error = "onlineVodHost is empty";
    return result;
  }

  const std::string ethernetMac = SystemUtils::getMacAddress();
  if (ethernetMac.empty()) {
    result.error = "Ethernet MAC is not available yet";
    return result;
  }
  const std::string serial = SystemUtils::getRegistrationSerial();
  if (serial.empty()) {
    result.error = "Valid hardware serial is not available yet";
    return result;
  }

  Json::Value request;
  request["protocolVersion"] = "2.0";
  request["deviceRole"] = "player";
  request["appId"] = "com.hsvj.engine";
  request["deviceName"] = SystemUtils::getDeviceName();
  request["macAddress"] = ethernetMac;
  request["serial"] = serial;
  request["model"] = SystemUtils::getHardwareModel();
  request["softwareVersion"] = HSVJ_VERSION;
  request["httpPort"] = 8080;

  const std::string admissionUrl = baseUrl + "/api/v1/terminals/admission";
  LOG_INFO("[License] Online VOD admission POST %s", admissionUrl.c_str());
  int httpStatus = 0;
  const std::string response = httpPostJson(
      admissionUrl, [&request](const std::string& localIp) {
        const Json::Value network = SystemUtils::getNetworkReport(localIp);
        if (!network.isObject()) return std::string();
        request["network"] = network;
        return JsonUtils::toString(request);
      }, 3, "", &httpStatus);
  if (response.empty()) {
    result.error = httpStatus > 0
                       ? ("empty server response (HTTP " + std::to_string(httpStatus) + ")")
                       : "server did not respond";
    LOG_WARN("[License] Online VOD admission empty response: HTTP %d", httpStatus);
    return result;
  }
  LOG_INFO("[License] Online VOD admission response HTTP %d body=%zu", httpStatus, response.size());
  LOG_INFO("[License] Online VOD admission response body: %.512s", response.c_str());
  Json::Value root;
  std::string parseError;
  if (!JsonUtils::parseJson(response, root, parseError) || !root.isObject()) {
    result.error = "invalid server response: " + parseError;
    LOG_WARN("[License] Online VOD admission response: %.512s", response.c_str());
    return result;
  }
  const int responseCode = root["code"].isInt() ? root["code"].asInt() : -1;
  if (responseCode != 0) {
    result.error = root.get("message", "server denied terminal admission").asString();
    const Json::Value& denial = root["data"];
    if (denial.isObject() && denial.isMember("remainingPoints") &&
        denial["remainingPoints"].isIntegral()) {
      result.error += " (remaining points: " +
                       std::to_string(denial["remainingPoints"].asInt()) +
                       ", registered: " +
                       std::to_string(denial.get("registeredTerminals", 0).asInt()) +
                       "/" +
                       std::to_string(denial.get("terminalLimit", 0).asInt()) + ")";
    }
    result.status = responseCode >= 500 || responseCode < 0
                        ? OnlineVodAdmissionStatus::RETRYABLE_FAILURE
                        : OnlineVodAdmissionStatus::DENIED;
    return result;
  }
  const Json::Value& data = root["data"];
  if (!data.isObject()) {
    result.error = "server admission response is incomplete";
    return result;
  }
  if (!data["authorized"].isBool() || !data["authorized"].asBool()) {
    result.status = OnlineVodAdmissionStatus::DENIED;
    result.error = data.get("message", "server denied terminal admission").asString();
    if (result.error.empty()) result.error = "server denied terminal admission";
    return result;
  }
  if (!data["contractVersion"].isInt() || data["contractVersion"].asInt() != 2 ||
      !data["license"].isObject()) {
    result.status = OnlineVodAdmissionStatus::DENIED;
    result.error = "服务器未返回新版授权范围，请升级服务器并重新签发 server.lic";
    return result;
  }
  result.data = data;
  result.status = OnlineVodAdmissionStatus::AUTHORIZED;
  return result;
  } catch (const std::exception& error) {
    result.status = OnlineVodAdmissionStatus::RETRYABLE_FAILURE;
    result.error = std::string("invalid server admission response: ") + error.what();
    return result;
  }
}

bool reportDeviceToLicenseServer(const std::string& baseUrl,
                                 int64_t expiry,
                                 int64_t start,
                                 const std::vector<std::string>& modules,
                                 const std::vector<int>& enabledLayers,
                                 int inputChannelCount,
                                 const std::string& usageMode,
                                 const std::string& licenseSource,
                                 const std::string& licenseContent) {
  int64_t reportedExpiry = expiry;
  int64_t reportedStart = start;
  auto reportedModules = modules;
  auto reportedLayers = enabledLayers;
  int reportedInputChannelCount = inputChannelCount;
  std::string reportedUsageMode = usageMode;
  std::string reportedLicenseSource = licenseSource;
  if (!licenseContent.empty()) {
    const std::string licenseJson = licenseContent.front() == '{'
        ? licenseContent : licenseBase64Decode(licenseContent);
    Json::Value licenseRoot;
    std::string parseError;
    if (JsonUtils::parseJson(licenseJson, licenseRoot, parseError)) {
      const auto& validity = licenseRoot["validity"];
      const auto& license = licenseRoot["license"];
      reportedStart = validity.get("issue_time", 0).asInt64();
      reportedExpiry = validity.get("expiry_time", 0).asInt64();
      reportedModules.clear();
      for (const auto& module : license["modules"]) {
        if (module.isString()) reportedModules.push_back(module.asString());
      }
      reportedLayers.clear();
      for (const auto& layer : license["enabled_layers"]) {
        if (layer.isInt()) reportedLayers.push_back(layer.asInt());
      }
      reportedInputChannelCount = license.get("input_channel_count", 0).asInt();
      reportedUsageMode = license.get("usage_mode", "").asString();
      reportedLicenseSource = "local";
    } else {
      LOG_WARN("[Step 4.9] 本地授权文件无法解析，使用授权管理器快照: %s", parseError.c_str());
    }
  }
  std::string trimmedBaseUrl = baseUrl;
  while (!trimmedBaseUrl.empty() && (trimmedBaseUrl.back() == '/' || trimmedBaseUrl.back() == ' ')) trimmedBaseUrl.pop_back();
  if (trimmedBaseUrl.empty()) {
    LOG_WARN("[Step 4.9] 跳过设备上报：license server url 为空");
    return false;
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
  root["version"] = 2;
  root["export_time"] = static_cast<Json::Int64>(now);
  Json::Value device;
  device["model"] = SystemUtils::getHardwareModel();
  device["serial"] = SystemUtils::getHardwareSerial();
  device["cpu_serial"] = SystemUtils::getCpuSerial();
  device["storage_serial"] = SystemUtils::getStorageSerial();
  device["mac"] = SystemUtils::getMacAddress();
  device["fingerprint"] = fingerprint;
  if (reportedExpiry > 0) device["license_expiry"] = static_cast<Json::Int64>(reportedExpiry);
  if (reportedStart > 0) device["license_start"] = static_cast<Json::Int64>(reportedStart);
  Json::Value moduleArray(Json::arrayValue);
  for (const auto& module : reportedModules) moduleArray.append(module);
  device["license_modules"] = moduleArray;
  Json::Value layerArray(Json::arrayValue);
  for (const int layerId : reportedLayers) layerArray.append(layerId);
  device["license_enabled_layers"] = layerArray;
  if (reportedInputChannelCount > 0) {
    device["license_input_channel_count"] = reportedInputChannelCount;
  }
  if (!reportedUsageMode.empty()) device["license_usage_mode"] = reportedUsageMode;
  if (!reportedLicenseSource.empty()) device["license_source"] = reportedLicenseSource;
  if (!licenseContent.empty()) device["license_content"] = licenseContent;
  root["device"] = device;
  std::string body = JsonUtils::toString(root);
  LOG_INFO("[Step 4.9] 开始设备上报: expiry=%lld start=%lld modules=%zu layers=%zu source=%s bodyBytes=%zu",
           (long long)reportedExpiry, (long long)reportedStart, reportedModules.size(), reportedLayers.size(),
           reportedLicenseSource.empty() ? "unknown" : reportedLicenseSource.c_str(), body.size());
  std::string out;
  bool usedBoundAndroidNetworks = false;
#ifdef __ANDROID__
  usedBoundAndroidNetworks = postDeviceReportAcrossAndroidNetworks(url, body, 15, &out);
#endif
  if (!usedBoundAndroidNetworks) {
    out = hsvj::httpPostJson(url, body, 15);
  }
  if (isDeviceReportResponseSuccessful(out)) {
    LOG_INFO("[Step 4.9] 设备上报成功");
    return true;
  } else {
    LOG_WARN("[Step 4.9] 设备上报失败或未响应: %s", out.empty() ? "(无输出)" : out.substr(0, 120).c_str());
    return false;
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
constexpr const char* kDebugHotspotPassphrase = "88888888";

struct StartupNetworkInfo {
  bool debugHotspotMode = false;
  std::string hotspotIp;
  std::string ethernetIp;
  std::string wifiIp;

  std::string mobileAccessIp() const {
    if (debugHotspotMode) return hotspotIp;
    if (!ethernetIp.empty()) return ethernetIp;
    return wifiIp;
  }
};

bool isLikelyDebugHotspotInterface(const std::string& name) {
  // wlan*/wifi* are also used by the normal STA connection.  Treating every
  // wireless interface as a hotspot makes a stale ready status reuse the local
  // address after the boot provisioning window has ended.  The boot mode is
  // handled separately by isDebugHotspotModeActive(), so only AP-specific
  // interface names are accepted here.
  return startsWith(name, "softap") || startsWith(name, "ap") ||
         startsWith(name, "swlan") || startsWith(name, "ap_br") ||
         startsWith(name, "bridge");
}

bool isInterfaceCarrierUp(const std::string& name) {
  std::ifstream carrier("/sys/class/net/" + name + "/carrier");
  std::string value;
  if (!carrier.is_open() || !std::getline(carrier, value)) {
    return true;
  }
  return trimCopy(value) == "1";
}

std::string getInterfaceIp(const std::string& interfaceName, bool requireCarrier = false) {
  if (requireCarrier && !isInterfaceCarrierUp(interfaceName)) {
    return "";
  }

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

std::string getFirstInterfaceIp(const std::vector<std::string>& preferredNames,
                                bool requireCarrier = false) {
  for (const std::string& name : preferredNames) {
    std::string ip = getInterfaceIp(name, requireCarrier);
    if (!ip.empty()) return ip;
  }
  return "";
}

std::string getFirstDebugHotspotInterfaceIp() {
  const Json::Value interfaces = SystemUtils::getNetworkInterfaces();
  for (const auto& iface : interfaces) {
    if (!iface.isMember("name") || !iface.isMember("ip")) continue;
    const std::string name = iface["name"].asString();
    const std::string ip = iface["ip"].asString();
    if (isLikelyDebugHotspotInterface(name) && !ip.empty()) return ip;
  }
  return "";
}

bool isIpAssignedToDebugHotspotInterface(const std::string& ip) {
  if (ip.empty()) return false;

  Json::Value interfaces = SystemUtils::getNetworkInterfaces();
  for (const auto& iface : interfaces) {
    if (!iface.isMember("name") || !iface.isMember("ip")) continue;
    if (iface["ip"].asString() != ip) continue;
    if (isLikelyDebugHotspotInterface(iface["name"].asString())) return true;
  }
  return false;
}

std::string getDebugHotspotSsid() {
  return kDebugHotspotSsid;
}

std::string getDebugHotspotIp() {
  const std::string state = readDebugHotspotValueFromStatusFile("state");
  const std::string ip = readDebugHotspotValueFromStatusFile("ip");
  if (state == "ready" && isIpAssignedToDebugHotspotInterface(ip)) {
    return ip;
  }
  return "";
}

std::string readBootWifiMode() {
#ifdef __ANDROID__
  return trimCopy(FileUtils::executeCommand(
      "getprop persist.hsvj.boot_wifi_mode 2>/dev/null"));
#else
  return "";
#endif
}

bool isBootSoftApWindowActive() {
  const std::string mode = readBootWifiMode();
  // The boot Wi-Fi state machine exposes a short preparation state, the
  // provisioning window, and a client-connected steady state.  All three
  // mean that the debug hotspot is the active startup network.
  return mode == "softap_preparing" || mode == "softap_window" ||
         mode == "softap";
}

bool isDebugHotspotModeActive(SystemConfig* systemConfig) {
  if (readDebugHotspotValueFromStatusFile("managed") == "system_settings") {
    const std::string state = readDebugHotspotValueFromStatusFile("state");
    return state == "ready" || state == "starting";
  }
  const std::string bootMode = readBootWifiMode();
  // `sta` is the ROM's terminal decision when nobody joined the provisioning
  // hotspot.  It must override a stale ready/starting status file from the
  // previous boot, otherwise wlan0 can be reported as the hotspot forever.
  if (bootMode == "sta") return false;
  if (isBootSoftApWindowActive()) return true;

  const std::string hotspotState = readDebugHotspotValueFromStatusFile("state");
  if (hotspotState == "ready" && !getDebugHotspotIp().empty()) return true;

  const bool configured = systemConfig && systemConfig->getStartupWirelessMode() == "hotspot";
  if (!configured) return false;

  return hotspotState.empty() || hotspotState == "starting" ||
         hotspotState == "boot_window" ||
         hotspotState == "pending_java";
}

StartupNetworkInfo getStartupNetworkInfo(SystemConfig* systemConfig) {
  StartupNetworkInfo info;
  info.debugHotspotMode = isDebugHotspotModeActive(systemConfig);
  if (info.debugHotspotMode) {
    info.hotspotIp = getDebugHotspotIp();
    if (info.hotspotIp.empty()) {
      info.hotspotIp = getFirstInterfaceIp(
          {"softap0", "ap0", "swlan0", "wlan0", "wlan1"});
    }
    if (info.hotspotIp.empty()) {
      info.hotspotIp = getFirstDebugHotspotInterfaceIp();
    }
  }

  info.ethernetIp = getFirstInterfaceIp({"eth0", "eth1"}, true);
  if (info.ethernetIp.empty() && systemConfig &&
      systemConfig->getNetworkIpMode() == "static" &&
      !systemConfig->getNetworkStaticIp().empty() &&
      isInterfaceCarrierUp("eth0")) {
    info.ethernetIp = systemConfig->getNetworkStaticIp();
  }

  if (!info.debugHotspotMode) {
    info.wifiIp = getFirstInterfaceIp({"wlan0", "wlan1", "wifi0", "wifi1"});
  }
  return info;
}

std::string getStartupMobileAccessIp(SystemConfig* systemConfig) {
  return getStartupNetworkInfo(systemConfig).mobileAccessIp();
}

constexpr int kStartupQrVersion = 3;
constexpr int kStartupQrSize = 21 + 4 * (kStartupQrVersion - 1);
constexpr int kStartupQrDataCodewords = 44;
constexpr int kStartupQrEccCodewords = 26;
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

bool writeStartupQrPng(const std::string& text, const std::string& path,
                       int imageSize) {
  StartupQrGrid grid{};
  if (!makeStartupQrMatrix(text, grid)) return false;

  imageSize = std::clamp(imageSize, 64, 2048);

  const int quiet = 2;
  const int totalModules = kStartupQrSize + quiet * 2;
  const int scale = std::max(1, imageSize / totalModules);
  const int imageModulesPx = totalModules * scale;
  const int offset = std::max(0, (imageSize - imageModulesPx) / 2);
  constexpr uint8_t darkR = 0x11, darkG = 0x18, darkB = 0x27;

  std::vector<uint8_t> scanlines(static_cast<size_t>((imageSize * 4 + 1) * imageSize), 0);
  for (int y = 0; y < imageSize; ++y) {
    const size_t row = static_cast<size_t>(y * (imageSize * 4 + 1));
    scanlines[row] = 0;
    for (int x = 0; x < imageSize; ++x) {
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
  appendU32Be(ihdr, static_cast<uint32_t>(imageSize));
  appendU32Be(ihdr, static_cast<uint32_t>(imageSize));
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

std::string getStartupFusionDebugUrl(SystemConfig* systemConfig) {
  std::string ip = getStartupMobileAccessIp(systemConfig);
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
  const int configuredCanvasW = systemConfig && systemConfig->getInputWidth() > 0
                                    ? systemConfig->getInputWidth()
                                    : res.width;
  const int configuredCanvasH = systemConfig && systemConfig->getInputHeight() > 0
                                    ? systemConfig->getInputHeight()
                                    : res.height;
  const int canvasW = configuredCanvasW > 0 ? configuredCanvasW : 1920;
  const int canvasH = configuredCanvasH > 0 ? configuredCanvasH : 1080;
  const int inputCols = systemConfig ? std::max(1, systemConfig->getInputLayoutCols()) : 1;
  const int inputRows = systemConfig ? std::max(1, systemConfig->getInputLayoutRows()) : 1;
  const int derivedRegionW = std::max(1, canvasW / inputCols);
  const int derivedRegionH = std::max(1, canvasH / inputRows);
  int regionW = systemConfig ? systemConfig->getRegionWidth() : 0;
  int regionH = systemConfig ? systemConfig->getRegionHeight() : 0;
  // Older matrix files sometimes stored the complete canvas in tileInWidth/
  // tileInHeight. A startup QR must use one input tile, otherwise the region
  // mapper stretches the QR texture with the full-canvas aspect ratio.
  if (regionW <= 0 || (inputCols > 1 && regionW >= canvasW)) {
    regionW = derivedRegionW;
  }
  if (regionH <= 0 || (inputRows > 1 && regionH >= canvasH)) {
    regionH = derivedRegionH;
  }

  layout.x = 0;
  layout.y = 0;
  layout.width = std::clamp(regionW, 1, canvasW);
  layout.height = std::clamp(regionH, 1, canvasH);

  const int shortSide = std::min(layout.width, layout.height);
  const int margin = std::clamp(shortSide / 16, 18, 64);
  const int maxQrByRegion = std::max(1, shortSide - margin * 2);
  const int qrUpper = std::max(1, std::min(180, maxQrByRegion));
  const int qrLower = std::min(90, qrUpper);
  layout.qrSize = std::clamp(shortSide / 6, qrLower, qrUpper);

  layout.qrX = layout.x + layout.width - margin - layout.qrSize;
  layout.qrY = layout.y + layout.height - margin - layout.qrSize;

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
    auto modules = licenseManager_->getModules();
    auto enabledLayers = licenseManager_->getEnabledLayerIds();
    int inputChannelCount = licenseManager_->getInputChannelCount();
    std::string usageMode = licenseManager_->getUsageMode();
    std::string licenseSource = licenseManager_->getLicenseSource();
    std::string licenseContent = licenseManager_->getLocalLicenseContent();
    const bool hasLocalLicense =
        (!systemConfig_ || !systemConfig_->isNetworkVodEnabled()) &&
        licenseManager_->hasValidLocalLicense();
    if (hasLocalLicense) {
      licenseManager_->syncLocalLicenseToCloud(licenseContent);
      LOG_INFO("[Step 4.9] 有效本地授权已加入后台云端同步队列");
      return;
    }
    licenseContent.clear();
    const uint64_t reportGeneration = localLicenseSyncGeneration_.load(std::memory_order_acquire);
    LOG_INFO("[Step 4.9] 上报参数: expiry=%lld start=%lld",
             (long long)expiry, (long long)start);
    trackEngineAsyncTask(std::async(std::launch::async, [this, licenseServerUrl, expiry, start,
                                                          modules = std::move(modules),
                                                          enabledLayers = std::move(enabledLayers),
                                                          inputChannelCount, usageMode = std::move(usageMode),
                                                          licenseSource = std::move(licenseSource),
                                                          licenseContent = std::move(licenseContent),
                                                          reportGeneration, hasLocalLicense]() {
      if (shuttingDown_.load()) return;
      std::lock_guard<std::mutex> reportLock(licenseReportMutex_);
      if (!licenseManager_ ||
          localLicenseSyncGeneration_.load(std::memory_order_acquire) != reportGeneration ||
          (hasLocalLicense && licenseManager_->getLocalLicenseContent() != licenseContent)) {
        LOG_INFO("[Step 4.9] 启动授权上报已过期，跳过旧授权快照");
        return;
      }
      const bool reported = reportDeviceToLicenseServer(licenseServerUrl, expiry, start, modules, enabledLayers,
                                                        inputChannelCount, usageMode, licenseSource, licenseContent);
      if (localLicenseSyncGeneration_.load(std::memory_order_acquire) != reportGeneration) return;
      if (reported && !shuttingDown_.load() && licenseManager_) licenseManager_->triggerCloudSync();
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
          // 设置共享目录，使 mobile/ 可以访问父目录的 modules/、shared/ 等资源
          NetworkManager::getInstance().getMobileHttpServer()->setSharedStaticDir(webDir + "shared/");
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
      LOG_INFO("  SyncUdpGroup(4322)");
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
      LOG_INFO("  SYNC :4322 (UDP multicast, when enabled)");
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
  try {
    Rk628Edid(CONFIG_DIR).restoreSaved();
  } catch (const std::exception& error) {
    LOG_WARN("[RK628] Saved EDID restore failed: %s", error.what());
  }
  sendProgress(0, 1, "创建目录结构...", 10);

  // Step 1.2.1: 更新 apiConfig.js 中的 IP 地址
  updateApiConfigIp();

  systemConfig_ = std::make_unique<SystemConfig>();
  const std::string configPath = CONFIG_PATH;
  bool configLoaded = false;
  if (FileUtils::isFile(configPath)) {
    if (!systemConfig_->load(configPath)) {
      LOG_ERROR("  config.json load failed before license selection");
      return false;
    }
    configLoaded = true;
  }

  // Step 1.3: 授权检查并构建内存授权图层池
  LOG_INFO("[Step 1.3] 授权检查");
  licenseManager_ = std::make_unique<LicenseManager>();
  licenseManager_->setCloudLicenseAppliedCallback([this]() {
    if (!shuttingDown_.load(std::memory_order_acquire)) refreshLicenseLayers();
  });
  licenseManager_->initialize(LICENSE_DIR);
  licenseManager_->setLocalLicenseSyncCallback([this](const std::string& content) {
    const uint64_t generation = localLicenseSyncGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    trackEngineAsyncTask(std::async(std::launch::async, [this, content, generation]() {
      for (;;) {
        if (shuttingDown_.load(std::memory_order_acquire) ||
            localLicenseSyncGeneration_.load(std::memory_order_acquire) != generation ||
            !licenseManager_) return;
        bool reported = false;
        {
          std::lock_guard<std::mutex> reportLock(licenseReportMutex_);
          if (localLicenseSyncGeneration_.load(std::memory_order_acquire) != generation) return;
          licenseManager_->stopCloudSync();
          std::string serverUrl = systemConfig_ ? systemConfig_->getLicenseServerUrl() : "";
          if (serverUrl.empty()) {
            serverUrl = "http://" + std::string(LicenseManager::DEFAULT_CLOUD_HOST) + ":" +
                        std::to_string(LicenseManager::DEFAULT_CLOUD_PORT);
          }
          reported = reportDeviceToLicenseServer(serverUrl, licenseManager_->getExpiryTime(),
              licenseManager_->getStartTime(), licenseManager_->getModules(),
              licenseManager_->getEnabledLayerIds(), licenseManager_->getInputChannelCount(),
              licenseManager_->getUsageMode(), "local", content);
          if (localLicenseSyncGeneration_.load(std::memory_order_acquire) != generation) return;
          licenseManager_->completeLocalLicenseSync(reported);
        }
        if (reported) return;
        licenseManager_->setLastImportCloudSyncStatus(0);
        LOG_WARN("[License] 本地授权云端同步未成功，30 秒后重试");
        for (int second = 0; second < 30; ++second) {
          if (shuttingDown_.load(std::memory_order_acquire) ||
              localLicenseSyncGeneration_.load(std::memory_order_acquire) != generation) return;
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      }
    }));
  });
  // Network VOD mode uses the VOD server admission path exclusively. When it
  // is off, the normal local/cloud player authorization path is enabled.
  licenseManager_->checkLicense(!systemConfig_->isNetworkVodEnabled());
  LOG_INFO("[License] Selected authorization mode: %s", systemConfig_->isNetworkVodEnabled() ? "VOD server" : "cloud with local fallback");
  onlineVodAdmissionAuthorized_.store(false, std::memory_order_release);
  auto serverHosts = DeviceDiscoveryService::getInstance().probeOnlineVodServers();
  const auto configuredHost = systemConfig_->getOnlineVodHost();
  serverHosts.erase(std::remove(serverHosts.begin(), serverHosts.end(), configuredHost), serverHosts.end());
  if (!configuredHost.empty()) serverHosts.insert(serverHosts.begin(), configuredHost);
  for (const auto& host : serverHosts) {
    const auto admission = requestOnlineVodAdmission(*systemConfig_, host);
    if (host != configuredHost && !requestsOnlineVodSwitch(admission.data)) continue;
    if (admission.status == OnlineVodAdmissionStatus::AUTHORIZED) {
      if (requestsOnlineVodSwitch(admission.data) &&
          !applyServerOnlineVodMode(*systemConfig_, configPath, host, admission.data)) continue;
      if (!systemConfig_->isNetworkVodEnabled()) continue;
      if (!licenseManager_->authorizeServerLicense(admission.data)) continue;
      onlineVodAdmissionAuthorized_.store(true, std::memory_order_release);
      break;
    } else {
      LOG_WARN("[License] Server admission pending: %s", admission.error.c_str());
    }
  }
  // Continue after initialization; apply responses on the render thread so
  // permissions and the runtime layer pool change together, never mid-frame.
  onlineVodAdmissionRetryThread_ = std::thread([this]() {
    int ticks = 0;
    OnlineVodConnectionHealth health;
    while (!shuttingDown_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      if (shuttingDown_.load(std::memory_order_acquire)) break;
      const bool admitted = onlineVodAdmissionAuthorized_.load(std::memory_order_acquire);
      if (++ticks < (admitted && health.failures() == 0 ? 120 : 20)) continue;
      ticks = 0;
      if (!initialized_.load()) continue;
      if (!systemConfig_ || !licenseManager_) continue;
      const auto expectedHost = systemConfig_->getOnlineVodHost();
      const int expectedMode = systemConfig_->getVodMode();
      auto hosts = DeviceDiscoveryService::getInstance().onlineVodServers();
      hosts.erase(std::remove(hosts.begin(), hosts.end(), expectedHost), hosts.end());
      if (!expectedHost.empty()) hosts.insert(hosts.begin(), expectedHost);
      // Exhaust the candidates before counting a failed round. A dead saved
      // address must not hide a reachable server discovered on another subnet.
      std::string host = expectedHost;
      OnlineVodAdmissionResult admission;
      for (const auto& candidate : hosts) {
        auto reply = requestOnlineVodAdmission(*systemConfig_, candidate);
        if (candidate != expectedHost && !requestsOnlineVodSwitch(reply.data)) continue;
        if (reply.status == OnlineVodAdmissionStatus::RETRYABLE_FAILURE) continue;
        host = candidate;
        admission = std::move(reply);
        break;
      }
      const bool unavailable = admission.status == OnlineVodAdmissionStatus::RETRYABLE_FAILURE;
      const bool disableVod = health.observe(expectedMode, expectedHost, unavailable);
      if (unavailable) {
        if (expectedMode == 2) {
          LOG_WARN("[OnlineVod] Admission round failed (%d/%d), configured server=%s",
                   health.failures(), OnlineVodConnectionHealth::failureLimit, expectedHost.c_str());
        }
        if (disableVod) {
          enqueueOnlineVodServerAction([this, expectedHost, expectedMode]() {
            if (shuttingDown_.load() || !systemConfig_ || !licenseManager_ ||
                systemConfig_->getVodMode() != expectedMode ||
                systemConfig_->getOnlineVodHost() != expectedHost) return;
            if (!disableUnavailableOnlineVod(*systemConfig_, CONFIG_PATH)) return;
            onlineVodAdmissionAuthorized_.store(false, std::memory_order_release);
            stopOnlineVodSync();
            applyVodConfigNow();
            refreshLicenseScreenHint();
          });
        }
        continue;
      }
      if (host != expectedHost && !requestsOnlineVodSwitch(admission.data)) continue;
      enqueueOnlineVodServerAction([this, admission, host, expectedHost, expectedMode]() {
            if (shuttingDown_.load() || !systemConfig_ || !licenseManager_) return;
            if (systemConfig_->getOnlineVodHost() != expectedHost || systemConfig_->getVodMode() != expectedMode) return;
            const bool hadAdmission = onlineVodAdmissionAuthorized_.load(std::memory_order_acquire);
            if (!systemConfig_->isNetworkVodEnabled() &&
                !requestsOnlineVodSwitch(admission.data)) return;
            if (admission.status == OnlineVodAdmissionStatus::DENIED) {
              onlineVodAdmissionAuthorized_.store(false, std::memory_order_release);
              licenseManager_->revokeServerLicense(admission.error);
              if (hadAdmission) stopOnlineVodSync();
              activateOnlineVodAfterAdmission();
              return;
            }
            // Compare effective rights on the render thread against the grant
            // already applied at startup, not a retry-thread response cache.
            const auto previousLayers = licenseManager_->getEnabledLayerIds();
            const auto previousModules = licenseManager_->getModules();
            const bool modeChanged = requestsOnlineVodSwitch(admission.data) &&
                (!systemConfig_->isNetworkVodEnabled() || systemConfig_->getOnlineVodHost() != host);
            if (modeChanged && !applyServerOnlineVodMode(*systemConfig_, CONFIG_PATH, host, admission.data)) return;
            if (!systemConfig_->isNetworkVodEnabled()) return;
            if (!licenseManager_->authorizeServerLicense(admission.data)) {
              LOG_WARN("[License] Invalid server grant: %s", licenseManager_->getLastError().c_str());
              return; // Retain the last valid grant until its actual expiry.
            }
            onlineVodAdmissionAuthorized_.store(true, std::memory_order_release);
            const bool changed =
                previousLayers != licenseManager_->getEnabledLayerIds() ||
                previousModules != licenseManager_->getModules();
            if (!hadAdmission || modeChanged || changed) activateOnlineVodAfterAdmission();
            if (modeChanged) {
              if (localVodPlayer_) localVodPlayer_->disable();
              applyVodConfigNow();
            } else if (!hadAdmission && isOnlineVodRuntimeEnabled()) {
              startOnlineVodSync(systemConfig_->getOnlineVodHost(), systemConfig_->getOnlineVodPort(), "current");
            }
          });
    }
  });

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
  if (loadServerStartupConfig(systemConfig_, configPath)) configLoaded = true;
  LOG_INFO("  config.json OK");
  sendProgress(0, 2, "加载配置文件...", 15);

  if (!configLoaded && !systemConfig_->load(configPath)) {
    LOG_ERROR("  config.json 加载失败");
    return false;
  }

  if (!SystemUtils::applyUsbOtgMode(systemConfig_->getUsbOtgMode())) {
    LOG_WARN("  USB OTG mode apply failed: %s",
             systemConfig_->getUsbOtgMode().c_str());
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

  if (!isOnlineVodRuntimeEnabled()) {
    triggerStartupDeviceReport();
  }

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
    sharedTextOverlayHolder_ = std::make_shared<SharedTextOverlayHolder>();
    LOG_INFO("  SharedTextOverlayHolder（Layer 40/41 共享字体）已创建");
  }
  mubu_ = std::make_unique<Mubu>();
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
    if (isVodRuntimeEnabled() && playlistManager_->getDatabase()) {
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
    cloudSyncService_->start(config, playlistManager_->getDatabase(),
        [](const std::string& path, const std::string& displayName, const CloudMaterialMetadata& metadata) {
          NetworkManager::getInstance().indexCloudMaterial(path, displayName, metadata);
        });
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
  // Admission is valid only for this initialization and its exact config.
  onlineVodAdmissionAuthorized_.store(false, std::memory_order_release);

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
    adaptCanvasToPhysicalOutput();
    configRes = systemConfig_->getResolution();

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

    // Online VOD owns the authorized video layer even when config.json only
    // contains the status/hint layer (for example layer 41). Its sync database
    // must therefore not depend on a configured local KTV layer.
    // 网络 VOD 的数据库必须在“准入成功”之前就准备好。启动时网线未插入
    // 时，准入会先失败并进入后台重试；如果这里仅按运行态授权判断，
    // VodDatabase_ 会被跳过初始化，后台准入成功后 startOnlineVodSync()
    // 只能得到“VodDatabase not ready”，随后重试线程退出，画面永久黑屏。
    const bool networkVodConfigured =
        systemConfig_ && systemConfig_->isNetworkVodEnabled();
    if (hasKtvLayer || networkVodConfigured || isOnlineVodRuntimeEnabled()) {
      {
        auto tCall = std::chrono::steady_clock::now();
        const int vodMode = systemConfig_ ? systemConfig_->getVodMode() : 0;
        const bool LocalVodEnabled = systemConfig_ && vodMode == 1 && systemConfig_->isLocalVodEnabled();
        const bool networkVodEnabled = isOnlineVodRuntimeEnabled();
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
              LOG_INFO("  LocalSongDatabase 已就绪: %s temporary=%d", runtimeSongDbPath.c_str(),
                       localSongDatabase_->isTemporary() ? 1 : 0);
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
        } else if (networkVodConfigured) {
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
        if (systemConfig_->isLocalSongFileScanEnabled() && localSongDatabase_ &&
            localSongDatabase_->isOpen() && !localSongDatabase_->isTemporary()) {
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
        } else if (systemConfig_->isLocalSongFileScanEnabled()) {
          LOG_WARN("  [LocalVod] 正式曲库不可用，暂缓本地歌曲文件扫描: %s", runtimeSongDbPath.c_str());
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

    StageControlManager::getInstance().initialize(mubu_.get(), commandRouter_.get(),
                                                       playlistManager_.get());
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
          videoLayer->checkAndAutoCapture(preferredType, width, height, cfg->captureIndex,
                                          cfg->captureResolutionLocked);
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

    // Local-license device reporting is not needed after online VOD admission.
    if (!isOnlineVodRuntimeEnabled()) {
      triggerStartupDeviceReport();
      sendProgress(3, 8, "设备上报...", 93);
    }

    // 初始化完成后在屏幕上提示本地 IP 与授权信息；授权到期后保持永久提示。
    refreshLicenseScreenHint();

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
  if (licenseManager_ && !licenseManager_->isLicensed()) {
    // Keep both access addresses visible when a network save replaces the hint.
    refreshLicenseScreenHint();
    return;
  }
  // A network/configuration event supersedes the asynchronous startup hint.
  // Invalidate its cleanup worker before touching Layer 41, otherwise the
  // worker can clear a freshly displayed status message a few seconds later.
  startupHintGeneration_.fetch_add(1, std::memory_order_relaxed);
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
  // Startup/license hints use protected renderer state. Release it before
  // replacing the startup text with a higher-priority network message.
  if (auto* hint = textLayer->getMessageHintRenderer()) {
    hint->clearOperationHint();
  }
  textLayer->showOperationHint(HintType::CUSTOM, msg, 30.0f);
}

void Engine::showNetworkStatusHint(const std::string& message) {
  if (!mubu_ || message.empty()) return;
  if (licenseManager_ && !licenseManager_->isLicensed()) {
    LOG_INFO("网络状态变化，刷新未授权连接提示: %s", message.c_str());
    refreshLicenseScreenHint();
    return;
  }
  // Network loss/restoration messages have priority over the startup hint.
  startupHintGeneration_.fetch_add(1, std::memory_order_relaxed);
  Layer* layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT) return;
  LayerText* textLayer = static_cast<LayerText*>(layer41);
  LOG_INFO("网络状态提示: %s", message.c_str());
  textLayer->setVisible(true);
  // Network loss must replace a protected startup hint immediately.
  if (auto* hint = textLayer->getMessageHintRenderer()) {
    hint->clearOperationHint();
  }
  textLayer->showOperationHint(HintType::CUSTOM, message, 30.0f);
}

void Engine::showStartupFusionQrOverlay() {
  if (!mubu_ || !systemConfig_) return;
  // The IP hint switch controls all startup connection hints, including this QR.
  if (!systemConfig_->isStartupIpHintEnabled() ||
      !systemConfig_->isFusionDebugEnabled()) {
    restoreStartupFusionQrOverlay();
    return;
  }
#ifdef __ANDROID__
  if (!renderer_) return;
#endif

  Layer* layer41 = mubu_->getLayer(41);
  const std::string qrUrl = getStartupFusionDebugUrl(systemConfig_.get());
  if (qrUrl.empty()) {
    LOG_WARN("启动融合二维码没有可用网络地址，跳过显示");
    return;
  }
  const std::string qrPath =
      FileUtils::normalizePath(QR_CODE_DIR + "fusion_mobile_debug_qr_runtime.png");
  const StartupRegionHintLayout layout =
      resolveStartupRegionHintLayout(systemConfig_.get());

  std::lock_guard<std::mutex> lock(startupQrMutex_);
  if (!startupQrOverlayLayer_) {
    // 启动二维码是系统临时覆盖层，不能复用业务二维码 Layer71。
    // 两者一对一持有各自图片与几何状态，避免启动提示覆盖正常二维码边框/内容。
    startupQrOverlayLayer_ =
        std::make_unique<LayerImage>(-71, LayerType::QRCODE);
    startupQrOverlayLayer_->initialize();
#ifdef __ANDROID__
    startupQrOverlayLayer_->setRenderer(renderer_.get());
#endif
    startupQrOverlayLayer_->setFilterMode(1);
    startupQrOverlayLayer_->setScaleMode(3);
    startupQrOverlayLayer_->setFadeInTime(0.0f);
    startupQrOverlayLayer_->setFadeOutTime(0.0f);
    startupQrOverlayLayer_->setDisplayDuration(0.0f);
    startupQrOverlayLayer_->setVisible(false);
  }

  const bool qrNeedsUpdate =
      startupQrActiveUrl_ != qrUrl ||
      startupQrGeneratedSize_ != layout.qrSize || !FileUtils::exists(qrPath);
  if (qrNeedsUpdate) {
    if (!writeStartupQrPng(qrUrl, qrPath, layout.qrSize)) {
      LOG_WARN("启动融合二维码生成失败: %s -> %s", qrUrl.c_str(), qrPath.c_str());
      return;
    }
    startupQrActiveUrl_ = qrUrl;
    startupQrGeneratedSize_ = layout.qrSize;
    startupQrOverlayLayer_->invalidateCache();
    LOG_INFO("启动融合二维码已按输入区域生成: region=%dx%d qr=%dx%d url=%s",
             layout.width, layout.height, layout.qrSize, layout.qrSize,
             qrUrl.c_str());
  }

  if (startupQrOverlayLayer_->getImagePath() != qrPath || qrNeedsUpdate) {
    if (!startupQrOverlayLayer_->loadImage(qrPath)) {
      LOG_WARN("启动融合二维码加载失败: %s", qrPath.c_str());
      return;
    }
  }

  if (layer41) {
    layer41->setVisible(true);
  }
  startupQrOverlayLayer_->setSize(Size(layout.qrSize, layout.qrSize));
  startupQrOverlayLayer_->setPosition(Position(layout.qrX, layout.qrY));
  startupQrOverlayLayer_->setAlpha(1.0f);
  startupQrOverlayLayer_->setVisible(true);
#ifdef __ANDROID__
  if (regionRotationRenderer_) {
    regionRotationRenderer_->invalidateQrOverlayCache();
  }
#endif
}

void Engine::restoreStartupFusionQrOverlay() {
  if (!mubu_) return;

  std::lock_guard<std::mutex> lock(startupQrMutex_);
  const bool qrWasVisible =
      startupQrOverlayLayer_ && startupQrOverlayLayer_->isVisible();
  if (startupQrOverlayLayer_) {
    startupQrOverlayLayer_->setVisible(false);
  }

  // This helper owns only the temporary QR layer. Layer 41 may already be
  // displaying a network or operation hint and must not be hidden here.
#ifdef __ANDROID__
  if (qrWasVisible && regionRotationRenderer_) {
    regionRotationRenderer_->invalidateQrOverlayCache();
  }
#endif
  if (qrWasVisible) {
    LOG_INFO("启动融合二维码提示已关闭，业务二维码 Layer71 保持不变");
  }
}
void Engine::refreshLicenseScreenHint() {
  if (!mubu_) return;
  Layer* layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT) return;
  LayerText* textLayer = static_cast<LayerText*>(layer41);

  struct StartupHintState {
    std::string message;
    bool permanent = false;
    bool visible = true;
  };

  auto buildStartupHintState = [this]() -> StartupHintState {
    const StartupNetworkInfo network =
        getStartupNetworkInfo(systemConfig_.get());

    StartupHintState state;
    const bool ipHintEnabled = !systemConfig_ || systemConfig_->isStartupIpHintEnabled();
    state.visible = ipHintEnabled;
    state.message = "引擎就绪";
    bool hasConfiguredVideoLayer = false;
    if (systemConfig_) {
      for (const auto& pair : systemConfig_->getAllLayerConfigs()) {
        if (pair.first >= 1 && pair.first <= 4) {
          hasConfiguredVideoLayer = true;
          break;
        }
      }
    }
    if (!hasConfiguredVideoLayer && systemConfig_ &&
        (systemConfig_->isLocalVodEnabled() || isOnlineVodRuntimeEnabled())) {
      state.message += " | 未配置视频图层";
      state.permanent = true;
      state.visible = true;
    }

    if (licenseManager_) {
      if (!licenseManager_->isLicensed()) {
        state.message += " | 未授权 | 请连接设备完成授权";
        state.visible = true;
        // Keep connection details visible; LicenseManager still blocks licensed features.
        state.permanent = true;
      } else {
        const int days = licenseManager_->getDaysUntilExpiry();
        const std::string daysSource = licenseManager_->getDaysSource();
        if (days <= 15) {
          state.visible = true;
          if (days < 0) {
            state.message += " | 授权已过期" + std::to_string(std::abs(days)) + " 天";
          } else {
            state.message += " | 授权剩" + std::to_string(days) + " 天";
          }
          state.permanent = days <= 0 && daysSource != "time_invalid" &&
              daysSource != "license_duration";
        }
      }
    }
    // The display switch hides connection details while preserving warnings.
    if (!ipHintEnabled) return state;
    if (network.debugHotspotMode) {
      state.message += "\n热点: " + getDebugHotspotSsid();
      state.message += " | IP: " +
          (network.hotspotIp.empty() ? std::string("获取中") : network.hotspotIp);
      state.message += " | 密码: " + std::string(kDebugHotspotPassphrase);
    } else {
      state.message += "\n热点: 未开启";
      if (!network.wifiIp.empty()) {
        state.message += " | Wi-Fi IP: " + network.wifiIp;
      }
    }
    state.message += "\n有线IP: " +
        (network.ethernetIp.empty() ? std::string("未连接或获取中") : network.ethernetIp);
    return state;
  };

  const StartupHintState initialState = buildStartupHintState();
  textLayer->setVisible(true);
  const unsigned long long hintGeneration =
      startupHintGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (initialState.visible) {
    textLayer->showProtectedOperationHint(HintType::CUSTOM, initialState.message,
                                          !initialState.permanent);
  } else if (auto* hint = textLayer->getMessageHintRenderer()) {
    hint->clearProtectedOperationHint();
  }

  showStartupFusionQrOverlay();
  trackEngineAsyncTask(std::async(std::launch::async,
      [this, hintGeneration, buildStartupHintState, initialState]() {
    const auto startedAt = std::chrono::steady_clock::now();
    std::string previousMessage = initialState.message;
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (shuttingDown_.load()) return;
      if (startupHintGeneration_.load(std::memory_order_relaxed) != hintGeneration) return;
      if (!mubu_) return;

      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - startedAt).count();
      const StartupHintState currentState = buildStartupHintState();
      Layer* currentLayer41 = mubu_->getLayer(41);
      if (!currentLayer41 || currentLayer41->getType() != LayerType::TEXT) return;
      LayerText* currentTextLayer = static_cast<LayerText*>(currentLayer41);
      if (currentState.message != previousMessage) {
        LOG_INFO("Startup connection hint updated elapsed=%lld: %s",
                 static_cast<long long>(elapsed), currentState.message.c_str());
        previousMessage = currentState.message;
      }
      if (elapsed >= 35 && !currentState.permanent) break;
      if (!currentState.visible) {
        if (auto* hint = currentTextLayer->getMessageHintRenderer()) {
          hint->clearProtectedOperationHint();
        }
      } else if (!currentTextLayer->showProtectedOperationHint(
              HintType::CUSTOM, currentState.message, !currentState.permanent, true)) {
        restoreStartupFusionQrOverlay();
        LOG_INFO("Startup hint dismissed by operation; background refresh stopped");
        return;
      }
      if (elapsed < 20) {
        showStartupFusionQrOverlay();
      } else {
        restoreStartupFusionQrOverlay();
      }
      // Permanent hints continue sampling both interfaces, including hotspot
      // address changes that the primary-IP network monitor does not observe.
    }

    if (startupHintGeneration_.load(std::memory_order_relaxed) == hintGeneration &&
        !shuttingDown_.load() && mubu_) {
      Layer* currentLayer41 = mubu_->getLayer(41);
      if (currentLayer41 && currentLayer41->getType() == LayerType::TEXT) {
        LayerText* currentTextLayer = static_cast<LayerText*>(currentLayer41);
        if (auto* hint = currentTextLayer->getMessageHintRenderer()) {
          if (hint->clearProtectedOperationHint()) {
            LOG_INFO("Startup network hint completed; protection cleared");
          }
        }
        // The renderer cleared only its own startup content. Do not hide the
        // layer here: a concurrent PAD/web action may already have replaced it.
      }
      restoreStartupFusionQrOverlay();
    }
  }));

  LOG_INFO("Startup license hint: %s", initialState.message.c_str());
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
        videoLayer->checkAndAutoCapture(captureType, width, height, runtimeConfig.captureIndex,
                                        runtimeConfig.captureResolutionLocked);
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
  if (lyricLoadCancelled_) lyricLoadCancelled_->store(true);
  if (lyricLoadFuture_.valid()) lyricLoadFuture_.wait();
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
      q.songId = it.get("songId", it.get("songNo", "")).asString();
      q.songTitle = it.get("songName", it.get("songTitle", "")).asString();
      q.artistName =
          it.get("singerNames",
                 it.get("singerName", it.get("artistName", "")))
              .asString();
      q.position = it.get("position", 0).asInt();
      q.status = it.get("status", 0).asInt();
      q.isPriority = it.get("isPriority", 0).asInt();
      q.addedAt = it.get("addedAt", "").asString();
      q.songNo = it.get("songNo", it.get("songId", "")).asString();
      q.languageCode = it.get("languageCode", "").asString();
      q.classifyCode =
          it.get("categoryCode", it.get("classifyCode", "")).asString();
      q.lightCode = it.get("lightCode", "").asString();
      q.songPath =
          it.get("relativePath", it.get("songPath", "")).asString();
      q.videoFileType = it.get("videoFileType", "").asString();
      q.track = it.get("track", 0).asInt();
      q.scoreEnabled = it.get("scoreEnabled", 0).asInt();
      q.singerNo =
          it.get("primarySingerNo", it.get("singerNo", "")).asString();
      q.rawJson = JsonUtils::toString(it);
      refreshed.push_back(std::move(q));
    }

    {
      std::lock_guard<std::mutex> lock(onlineVodQueueMutex_);
      onlineVodQueue_ = std::move(refreshed);
      LOG_DEBUG("[OnlineVod] async queue refresh ok: %zu items", onlineVodQueue_.size());
    }

  }));
}

void Engine::requestOnlineVodSkipAsync(const std::string& reason,
                                           const std::string& queueItemId) {
  if (shuttingDown_.load() || onlineVodPauseRequested_.load()) return;
  if (queueItemId.empty()) {
    LOG_ERROR("[OnlineVod] skip request rejected: missing queueItemId reason=%s",
              reason.c_str());
    return;
  }

  {
    std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
    if (onlineVodCompletedQueueItemId_ == queueItemId) return;
    // Keep the current room's intent even while a previous skip is in flight
    // or rate limited. Consecutive failed opens may arrive only milliseconds apart.
    onlineVodPendingSkipQueueItemId_ = queueItemId;
    onlineVodPendingSkipReason_ = reason;
    onlineVodPendingSkipGeneration_ = onlineVodRoomStateGeneration_.load();
  }
  retryOnlineVodSkipAsync();
}

void Engine::retryOnlineVodSkipAsync() {
  if (shuttingDown_.load()) return;
  std::string queueItemId;
  std::string reason;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
    if (onlineVodPendingSkipQueueItemId_.empty()) return;
    if (onlineVodPauseRequested_.load() ||
        onlineVodPendingSkipGeneration_ != onlineVodRoomStateGeneration_.load() ||
        onlineVodCompletedQueueItemId_ == onlineVodPendingSkipQueueItemId_) {
      onlineVodPendingSkipQueueItemId_.clear();
      onlineVodPendingSkipReason_.clear();
      return;
    }
    if (std::chrono::steady_clock::now() < onlineVodSkipRetryAfter_ ||
        onlineVodSkipInFlight_.exchange(true)) return;
    onlineVodSkipRetryAfter_ = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
    queueItemId = onlineVodPendingSkipQueueItemId_;
    reason = onlineVodPendingSkipReason_;
    generation = onlineVodPendingSkipGeneration_;
  }

  std::string host = systemConfig_ ? systemConfig_->getOnlineVodHost() : "";
  int port = 9898;
  if (host.empty()) host = lastOnlineVodWsHost_;
  if (host.empty()) {
    onlineVodSkipInFlight_.store(false);
    LOG_WARN("[OnlineVod] skip request ignored, missing server host: %s", reason.c_str());
    return;
  }

  std::string roomId = lastOnlineVodWsRoomId_.empty() ? "current" : lastOnlineVodWsRoomId_;
  std::string url = "http://" + host;
  if (port != 80 && port > 0) url += ":" + std::to_string(port);
  url += "/api/v1/rooms/" + roomId + "/skip";
  Json::Value request(Json::objectValue);
  request["queueItemId"] = queueItemId;
  const std::string requestBody = JsonUtils::toString(request);
  trackEngineAsyncTask(std::async(
      std::launch::async, [this, url, reason, queueItemId, requestBody, generation]() {
        struct ResetInFlight {
          std::atomic<bool>& flag;
          ~ResetInFlight() { flag.store(false); }
        } reset{onlineVodSkipInFlight_};
        if (shuttingDown_.load() || onlineVodPauseRequested_.load() ||
            generation != onlineVodRoomStateGeneration_.load()) return;
        const std::string resp = httpPostJson(url, requestBody, 3);
        Json::Value result;
        std::string error;
        if (JsonUtils::parseJson(resp, result, error) &&
            result.get("code", -1).asInt() == 0) {
          std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
          // advanced=false is also an acknowledgement: the exact queue row
          // was already consumed or is no longer current. Never skip its successor.
          if (result["data"].get("advanced", false).asBool()) {
            onlineVodCompletedQueueItemId_ = queueItemId;
          }
          if (onlineVodPendingSkipQueueItemId_ == queueItemId &&
              onlineVodPendingSkipGeneration_ == generation) {
            onlineVodPendingSkipQueueItemId_.clear();
            onlineVodPendingSkipReason_.clear();
          }
        }
        LOG_INFO(
            "[OnlineVod] skip request (%s): queueItemId=%s %s -> %s",
            reason.c_str(), queueItemId.c_str(), url.c_str(), resp.c_str());
      }));
}

void Engine::refreshLicenseLayers() {
  enqueueOnlineVodServerAction([this]() { activateOnlineVodAfterAdmission(); });
}

void Engine::activateOnlineVodAfterAdmission() {
  if (shuttingDown_.load() || !mubu_ || !systemConfig_ || !licenseManager_) {
    return;
  }

  for (int layerId : mubu_->getAllLayerIds()) {
    if (!isAuthorizedLayerId(licenseManager_->getEnabledLayerIds(), layerId)) {
      if (Layer* layer = mubu_->getLayer(layerId)) layer->setVisible(false);
      mubu_->removeLayer(layerId);
    }
  }

  const auto preservedLayerIds = mubu_->getAllLayerIds();
  const size_t layersBefore = preservedLayerIds.size();
  preCreateAuthorizedLayers();
  // Reapply configuration only to newly admitted layers. Existing layers own
  // live playback, audio, geometry and lyric state that must survive refresh.
  createLayersFromConfig(preservedLayerIds);

  // The normal startup path wires these callbacks after the renderer and Mubu
  // are ready. Repeat that small piece for layers that were intentionally
  // absent while the device was waiting for network admission.
  for (int layerId : mubu_->getAllLayerIds()) {
    if (std::find(preservedLayerIds.begin(), preservedLayerIds.end(), layerId) !=
        preservedLayerIds.end()) continue;
    Layer* layer = mubu_->getLayer(layerId);
    if (!layer) continue;
#ifdef __ANDROID__
    if (!layer->getRenderer() && renderer_) {
      layer->setRenderer(renderer_.get());
    }
#endif
    if (layer->getType() != LayerType::VIDEO) continue;
    auto* videoLayer = static_cast<LayerVideo*>(layer);
    videoLayer->setPreloadNextPathCallback([this](int lid) -> std::string {
      if (!playlistManager_) return "";
      const std::string playlistId = playlistManager_->getActivePlaylistId(lid);
      if (playlistId.empty()) return "";
      const NextVideoInfo info = playlistManager_->peekNextVideoInfo(playlistId);
      if (!info.valid || info.item.uri.empty()) return "";
      const std::string normalized = FileUtils::normalizePath(info.item.uri);
      if (normalized.empty() || !FileUtils::isFile(normalized)) return "";
      return normalized;
    });
  }

  refreshLicenseScreenHint();
  NetworkManager::getInstance().refreshAudioEffectRoutes();
  const size_t layersAfter = mubu_->getAllLayerIds().size();
  LOG_INFO("[License] Online VOD layers activated after admission: before=%zu after=%zu preserved=%zu",
           layersBefore, layersAfter, preservedLayerIds.size());
}

bool Engine::startOnlineVodSync(const std::string& host, int port, const std::string& roomId) {
  if (!isOnlineVodRuntimeEnabled()) {
    LOG_WARN("startOnlineVodSync: Online VOD admission is not active, sync is disabled");
    return false;
  }
  if (!vodDatabase_) {
    auto database = std::make_unique<VodDatabase>();
    if (!database->initOnlineVodSyncDb()) {
      LOG_WARN("startOnlineVodSync: failed to initialize online VOD database");
      return false;
    }
    vodDatabase_ = std::move(database);
  }
  setOnlineVodFreesongSource(host, port);
#if defined(_WIN32) || defined(_WIN64)
  if (!onlineVodHttpSyncClient_) {
    onlineVodHttpSyncClient_ = std::make_unique<OnlineVodHttpSyncClient>();
  }
  onlineVodHttpSyncClient_->setOnRoomStateChanged(
      [this](const VodDatabase::OnlineVodState& state) {
        onlineVodPauseRequested_.store(state.playState != 1);
        enqueueOnlineVodServerAction([this, state]() {
          onOnlineVodStateChanged(state);
        });
      });
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
      // Idle-media list changes are not playback state. Refresh the cache only;
      // actual playback remains driven by the server's complete room state.
      if (shuttingDown_.load()) return;
      onlineVodNextEmptyCount_ = 0;
      scheduleOnlineVodFreesongFetchAsync(
          -1, false, true, "idle_media_list_changed");
      LOG_INFO("[OnlineVod] idle-media list changed; cache refresh requested, waiting for authoritative room state");
    });

    onlineVodWsClient_->setOnConnectionStateChanged([this](bool connected) {
      if (!connected) {
        if (mubu_) {
          Layer* layer41 = mubu_->getLayer(41);
          if (layer41 && layer41->getType() == LayerType::TEXT) {
            LayerText* textLayer = static_cast<LayerText*>(layer41);
            if (!onlineVodWsEverConnected_.load()) {
              // Keep the configured Layer 41 color for transient connection hints.
              // Mutating the layer style here made later playlist text stay red/orange.
              textLayer->showOperationHint(HintType::CUSTOM, "连接服务器失败\n请检查网络", 12.0f);
            } else {
              textLayer->showOperationHint(HintType::CUSTOM, "点播服务器已断开\n正在重连...", 8.0f);
            }
          }
        }
        LOG_WARN("[OnlineVod] WebSocket disconnected, showing hint (everConnected=%d)",
                 onlineVodWsEverConnected_.load() ? 1 : 0);
      } else {
        onlineVodTransportGeneration_.fetch_add(1);
        scheduleOnlineVodFreesongFetchAsync(-1, false, true, "ws_connected");
        bool wasEverConnected = onlineVodWsEverConnected_.exchange(true);
        if (wasEverConnected) {
          if (mubu_) {
            Layer* layer41 = mubu_->getLayer(41);
            if (layer41 && layer41->getType() == LayerType::TEXT) {
              LayerText* textLayer = static_cast<LayerText*>(layer41);
              textLayer->showOperationHint(HintType::CUSTOM, "已重新连接", 2.0f);
            }
          }
          // The WS client owns join_room and retries until a valid room state arrives.
          LOG_INFO("[OnlineVod] WebSocket reconnected successfully; waiting for authoritative room state");
        } else {
          LOG_INFO("[OnlineVod] first WebSocket connection established; waiting for authoritative room state");
        }
      }
    });

    // 注入状态变化回调，用于触发图层41操作提示
    onlineVodWsClient_->setOnRoomStateChanged([this](const VodDatabase::OnlineVodState& st) {
      onlineVodPauseRequested_.store(st.playState != 1);
      enqueueOnlineVodServerAction([this, st]() {
        onOnlineVodStateChanged(st);
      });
    });

    onlineVodWsClient_->setOnPlaybackCommand([this](const std::string& action,
                                                 const Json::Value& param,
                                                 const std::string& rawJson) {
      if (action == "Pause" || action == "Stop") {
        onlineVodPauseRequested_.store(true);
      } else if (action == "Play" || action == "Resume") {
        onlineVodPauseRequested_.store(false);
      }
      return enqueueOnlineVodServerAction([this, action, param, rawJson]() {
        const bool ok = handleOnlineVodPlaybackCommand(action, param, rawJson);
        if (onlineVodWsClient_) {
          onlineVodWsClient_->postCommandResult(
              action, ok, ok ? 0 : 1, ok ? "执行成功" : "播放命令执行失败");
        }
      });
    });

    onlineVodWsClient_->setOnPeripheralCommand([this](const std::string& action,
                                                   const Json::Value& param,
                                                   const std::string& rawJson) {
      return enqueueOnlineVodServerAction([this, action, param, rawJson]() {
        const auto forwarded = PeripheralManager::getInstance().dispatchVodEvent(action, param);
        if (forwarded.get("matched", 0).asInt() > 0 || !forwarded.get("ok", true).asBool()) {
          const bool ok = forwarded.get("ok", false).asBool();
          if (onlineVodWsClient_) onlineVodWsClient_->postCommandResult(
              action, ok, ok ? 0 : 1, forwarded.get("message", "VOD转发完成").asString());
          return;
        }
        (void)rawJson;
        CommandResponse response;
        response.ok = false;
        response.error = 1;
        response.message = "外设命令路由不可用";
        if (commandRouter_) {
          std::string routerAction;
          Json::Value routerParam(Json::objectValue);

          if (action == "SetLight") {
            const std::string scene = param.get("scene", "").asString();
            if (scene.empty()) {
              response.error = 0x0002;
              response.message = "SetLight requires scene";
            } else {
              routerAction = "execute_function_button";
              routerParam["region"] = "lighting";
              routerParam["button_id"] = scene;
            }
          } else if (action == "SetEffect") {
            const std::string mode = param.get("mode", "").asString();
            if (mode.empty()) {
              response.error = 0x0002;
              response.message = "SetEffect requires mode";
            } else {
              routerAction = "execute_function_button";
              routerParam["region"] = "effect";
              routerParam["button_id"] = mode;
            }
          } else if (action == "SetAC") {
            // SetAC is an idempotent absolute-state API. Native HVAC buttons are
            // relative/toggle actions and cannot safely map power/temp/mode/wind.
            response.error = 0x000A;
            response.message =
                "SetAC absolute-state hardware adapter is not configured";
          } else {
            response.error = 0x000A;
            response.message = "unsupported online VOD peripheral action: " + action;
          }

          if (!routerAction.empty()) {
            routerParam["action"] = routerAction;
            Json::Value routerCommand;
            routerCommand["type"] = 0;
            routerCommand["code"] = 0x50;
            routerCommand["param"] = routerParam;
            response = commandRouter_->processCommand(
                JsonUtils::toString(routerCommand));
          }
        }
        LOG_INFO("[OnlineVod] peripheral command execute: action=%s ok=%d error=0x%04X message=%s",
                 action.c_str(), response.ok ? 1 : 0, response.error,
                 response.message.c_str());
        if (onlineVodWsClient_) {
          onlineVodWsClient_->postCommandResult(action, response.ok, response.error,
                                                response.message);
        }
      });
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

  // 连接线程可能立即回调，必须在 start() 前发布房间和媒体地址快照。
  lastOnlineVodWsHost_ = host;
  lastOnlineVodWsPort_ = port;
  lastOnlineVodWsRoomId_ = roomId;

  LOG_INFO("[OnlineVod] starting WebSocket connection: host=%s port=%d roomId=%s",
           host.c_str(), port, roomId.c_str());
  bool started = onlineVodWsClient_->start(host, port, roomId, vodDatabase_.get());
  if (!started) {
    LOG_WARN("[OnlineVod] failed to start WebSocket connection: host=%s port=%d roomId=%s",
             host.c_str(), port, roomId.c_str());
    lastOnlineVodWsHost_.clear();
    lastOnlineVodWsPort_ = 0;
    lastOnlineVodWsRoomId_.clear();
  }
  return started;
#endif
}

static bool isVideoExtension(const std::string& name) {
  static const char* exts[] = {
    ".hvideo", ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv",
    ".ts", ".m4v", ".webm", ".mpg", ".mpeg", ".3gp", ".vob", ".dat"
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

void Engine::setOnlineVodFreesongSource(const std::string& host, int port) {
  std::string baseUrl = host.empty() ? "" : "http://" + host;
  if (!baseUrl.empty() && port != 80 && port > 0) baseUrl += ":" + std::to_string(port);
  std::lock_guard<std::mutex> fetchLock(onlineVodFreesongFetchMutex_);
  std::lock_guard<std::mutex> listLock(onlineVodFreesongsMutex_);
  if (baseUrl == onlineVodFreesongBaseUrl_) return;
  onlineVodFreesongBaseUrl_ = baseUrl;
  onlineVodFreesongsList_.clear();
  onlineVodFreesongLastAttemptPath_.clear();
  onlineVodFreesongPendingPlayLayerId_ = -1;
  onlineVodFreesongPendingPlayGeneration_ = 0;
  onlineVodFreesongPendingPlayPaused_ = false;
  onlineVodFreesongNextFetchAllowed_ = {};
  onlineVodFreesongNextRefresh_ = {};
  onlineVodFreesongFetchRetryCount_.store(0);
  if (onlineVodFreesongFetchInFlight_) onlineVodFreesongRefetchPending_ = true;
}

bool Engine::fetchAndCacheFreesongs() {
  std::string baseUrl;
  {
    std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
    baseUrl = onlineVodFreesongBaseUrl_;
  }
  if (baseUrl.empty()) return false;

  auto normalizeSubPath = [](std::string path) -> std::string {
    for (auto& c : path) {
      if (c == '\\') c = '/';
    }
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
  };

  auto fetchFromApi = [baseUrl, normalizeSubPath](const std::string& apiUrl,
                                                int timeoutSec,
                                                std::vector<std::string>& out) -> bool {
    LOG_INFO("[OnlineVod] fetching freesongs: %s", apiUrl.c_str());
    int status = 0;
    const std::string resp = hsvj::httpGet(apiUrl, timeoutSec, "", &status);
    if (status != 200 || resp.empty()) {
      LOG_WARN("[OnlineVod] freesongs fetch failed: HTTP=%d url=%s",
               status, apiUrl.c_str());
      return false;
    }

    Json::Value root;
    std::string err;
    if (!JsonUtils::parseJson(resp, root, err)) {
      LOG_WARN("[OnlineVod] freesongs parse failed: %s, url=%s",
               err.c_str(), apiUrl.c_str());
      return false;
    }
    if (!root.isObject() || !root["code"].isInt() || root["code"].asInt() != 0 ||
        !root.isMember("data") || !root["data"].isObject()) {
      LOG_WARN("[OnlineVod] freesongs schema error: data must be an object, url=%s",
               apiUrl.c_str());
      return false;
    }

    const auto& data = root["data"];
    if (!data.isMember("media_root") || !data["media_root"].isString() ||
        !data.isMember("idle_song_path") ||
        !data["idle_song_path"].isString() ||
        !data.isMember("files") || !data["files"].isArray()) {
      LOG_WARN("[OnlineVod] freesongs schema error: expected media_root, idle_song_path and files, url=%s",
               apiUrl.c_str());
      return false;
    }

    std::string subPath =
        normalizeSubPath(data["idle_song_path"].asString());
    // The scan API returns the configured Windows directory, while room state
    // uses a path relative to media_root. Use the same root mapping for both.
    if (subPath.size() >= 3 && subPath[1] == ':' && subPath[2] == '/') {
      const auto lowerPath = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return value;
      };
      const std::string pathLower = lowerPath(subPath);
      std::istringstream roots(data["media_root"].asString());
      std::string mediaRoot;
      bool matchedRoot = false;
      while (std::getline(roots, mediaRoot, ';')) {
        const auto first = mediaRoot.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        mediaRoot = normalizeSubPath(mediaRoot.substr(
            first, mediaRoot.find_last_not_of(" \t\r\n") - first + 1));
        const std::string rootLower = lowerPath(mediaRoot);
        if (pathLower == rootLower ||
            pathLower.rfind(rootLower + "/", 0) == 0) {
          subPath = normalizeSubPath(subPath.substr(mediaRoot.size()));
          matchedRoot = true;
          break;
        }
      }
      if (!matchedRoot) {
        LOG_WARN("[OnlineVod] idle_song_path is outside media_root; ignoring scan");
        return false;
      }
    }
    std::set<std::string> dedup;
    int skipped = 0;
    out.clear();
    for (Json::ArrayIndex i = 0; i < data["files"].size(); ++i) {
      if (!data["files"][i].isString()) {
        ++skipped;
        continue;
      }
      std::string relativePath = data["files"][i].asString();
      for (char& c : relativePath) {
        if (c == '\\') c = '/';
      }
      bool safeRelativePath = !relativePath.empty() && relativePath.front() != '/';
      size_t segmentStart = 0;
      while (safeRelativePath && segmentStart <= relativePath.size()) {
        const size_t slash = relativePath.find('/', segmentStart);
        const std::string segment = relativePath.substr(
            segmentStart, slash == std::string::npos
                              ? std::string::npos
                              : slash - segmentStart);
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.find(':') != std::string::npos) {
          safeRelativePath = false;
          break;
        }
        if (slash == std::string::npos) break;
        segmentStart = slash + 1;
      }
      if (!safeRelativePath || !isVideoExtension(relativePath)) {
        ++skipped;
        continue;
      }

      std::string url = baseUrl + "/media/";
      if (!subPath.empty()) url += subPath + "/";
      url += relativePath;
      if (dedup.insert(url).second) out.push_back(std::move(url));
    }

    if (skipped > 0) {
      LOG_INFO("[OnlineVod] freesongs scan: skipped %d invalid entries", skipped);
    }
    return true;
  };

  std::vector<std::string> found;
  const std::string apiUrl =
      baseUrl + "/api/v1/idle-media/scan";

  const bool ok = fetchFromApi(apiUrl, 5, found);

  {
    std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
    if (baseUrl != onlineVodFreesongBaseUrl_) {
      LOG_INFO("[OnlineVod] discard freesongs response from previous server: %s", baseUrl.c_str());
      return false;
    }
    if (!ok) {
      LOG_WARN("[OnlineVod] freesongs fetch failed; retain %zu cached files and retry",
               onlineVodFreesongsList_.size());
      return false;
    }
    // A valid empty list is authoritative (e.g. files were removed).
    onlineVodFreesongsList_ = found;
  }
  LOG_INFO("[OnlineVod] freesongs indexed: %zu video files", found.size());
  const size_t sampleCount = std::min<size_t>(found.size(), 3);
  for (size_t i = 0; i < sampleCount; ++i) {
    LOG_INFO("[OnlineVod] freesongs sample[%zu]=%s", i, found[i].c_str());
  }
  return !found.empty();
}

bool Engine::scheduleOnlineVodFreesongFetchAsync(
    int layerId, bool playAfterFetch, bool force, const char* reason,
    uint64_t stateGeneration, bool paused) {
  const std::string fetchReason = reason ? reason : "unknown";
  const auto now = std::chrono::steady_clock::now();
  {
    // Serialize backoff, pending playback and in-flight transitions. Without a
    // single completion boundary, a request can arrive after pending is consumed
    // but before in-flight is cleared and then remain unprocessed indefinitely.
    std::lock_guard<std::mutex> lock(onlineVodFreesongFetchMutex_);
    if (shuttingDown_.load()) return false;
    if (playAfterFetch && layerId >= 0) {
      if (stateGeneration != onlineVodRoomStateGeneration_.load()) return false;
      // Preserve the requested playback even when a previous scan is backing off.
      onlineVodFreesongPendingPlayLayerId_ = layerId;
      onlineVodFreesongPendingPlayGeneration_ = stateGeneration;
      onlineVodFreesongPendingPlayPaused_ = paused;
    }
    if (!force &&
        onlineVodFreesongNextFetchAllowed_ !=
            std::chrono::steady_clock::time_point{} &&
        now < onlineVodFreesongNextFetchAllowed_) {
      const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          onlineVodFreesongNextFetchAllowed_ - now).count();
      LOG_INFO("[OnlineVod] freesongs fetch deferred by backoff: reason=%s wait_ms=%lld",
               fetchReason.c_str(), static_cast<long long>(waitMs));
      return false;
    }

    if (onlineVodFreesongFetchInFlight_) {
      // A settings notification can arrive while an older scan is still in flight.
      if (force) onlineVodFreesongRefetchPending_ = true;
      LOG_INFO("[OnlineVod] freesongs fetch already in flight: reason=%s",
               fetchReason.c_str());
      return false;
    }
    onlineVodFreesongFetchInFlight_ = true;
  }

  trackEngineAsyncTask(std::async(
      std::launch::async, [this, fetchReason]() {
        for (;;) {
          const bool available = !shuttingDown_.load() && fetchAndCacheFreesongs();
          int delaySeconds = 0;
          int failureCount = 0;
          if (available) {
            onlineVodFreesongFetchRetryCount_.store(0, std::memory_order_relaxed);
          } else {
            failureCount = onlineVodFreesongFetchRetryCount_.fetch_add(
                               1, std::memory_order_relaxed) + 1;
            delaySeconds = 30;
            switch (failureCount) {
              case 1: delaySeconds = 1; break;
              case 2: delaySeconds = 2; break;
              case 3: delaySeconds = 4; break;
              case 4: delaySeconds = 8; break;
              case 5: delaySeconds = 16; break;
              default: break;
            }
            LOG_WARN("[OnlineVod] freesongs fetch unavailable; retry backoff=%ds failures=%d reason=%s",
                     delaySeconds, failureCount, fetchReason.c_str());
          }

          int pendingLayer = -1;
          uint64_t pendingGeneration = 0;
          bool pendingPaused = false;
          {
            std::lock_guard<std::mutex> lock(onlineVodFreesongFetchMutex_);
            if (onlineVodFreesongRefetchPending_ && !shuttingDown_.load()) {
              onlineVodFreesongRefetchPending_ = false;
              continue;
            }
            onlineVodFreesongRefetchPending_ = false;
            onlineVodFreesongNextFetchAllowed_ =
                available
                    ? std::chrono::steady_clock::time_point{}
                    : std::chrono::steady_clock::now() +
                          std::chrono::seconds(delaySeconds);
            onlineVodFreesongNextRefresh_ = std::chrono::steady_clock::now() +
                std::chrono::seconds(available ? 30 : delaySeconds);
            pendingLayer = onlineVodFreesongPendingPlayLayerId_;
            pendingGeneration = onlineVodFreesongPendingPlayGeneration_;
            pendingPaused = onlineVodFreesongPendingPlayPaused_;
            // A failed scan is retried by the update loop. It must not consume
            // the sole startup play request and wait for another room event.
            if (available) {
              onlineVodFreesongPendingPlayLayerId_ = -1;
              onlineVodFreesongPendingPlayGeneration_ = 0;
              onlineVodFreesongPendingPlayPaused_ = false;
            }
            onlineVodFreesongFetchInFlight_ = false;
          }

          if (!available || pendingLayer < 0 || shuttingDown_.load()) return;
          // The scan can finish after a new song or pause state arrives. Apply
          // cached idle playback under the same lock as room-state media opens.
          std::unique_lock<std::mutex> applyLock(onlineVodPlaybackApplyMutex_);
          if (shuttingDown_.load() ||
              pendingGeneration != onlineVodRoomStateGeneration_.load() ||
              !isOnlineVodRuntimeEnabled()) {
            LOG_INFO("[OnlineVod] discard stale freesong fetch playback: generation=%llu",
                     static_cast<unsigned long long>(pendingGeneration));
            return;
          }
          if (onlineVodPauseRequested_.load() && !pendingPaused) {
            deferOnlineVodFreesongPlayback(pendingLayer, pendingGeneration,
                                          pendingPaused);
            return;
          }
          bool stillIdle = false;
          {
            std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
            stillIdle = onlineVodLastPlaybackWasIdle_;
          }
          if (stillIdle) {
            if (!tryPlayFreesongOnLayer(pendingLayer, pendingGeneration,
                                       pendingPaused)) {
              deferOnlineVodFreesongPlayback(pendingLayer, pendingGeneration,
                                            pendingPaused);
            }
          } else {
            LOG_INFO("[OnlineVod] freesongs fetched but room is no longer idle; playback not started");
          }
          return;
        }
      }));
  LOG_INFO("[OnlineVod] freesongs fetch scheduled: reason=%s force=%d play_after=%d",
           fetchReason.c_str(), force ? 1 : 0, playAfterFetch ? 1 : 0);
  return true;
}

void Engine::deferOnlineVodFreesongPlayback(int layerId,
                                           uint64_t stateGeneration,
                                           bool paused) {
  std::lock_guard<std::mutex> lock(onlineVodFreesongFetchMutex_);
  if (shuttingDown_.load() ||
      stateGeneration != onlineVodRoomStateGeneration_.load()) return;
  onlineVodFreesongPendingPlayLayerId_ = layerId;
  onlineVodFreesongPendingPlayGeneration_ = stateGeneration;
  onlineVodFreesongPendingPlayPaused_ = paused;
  // Bound media-open retries too; the server can be reachable before its media
  // disk/share is ready. No decoder/network work runs on the update thread.
  onlineVodFreesongNextFetchAllowed_ = std::max(
      onlineVodFreesongNextFetchAllowed_,
      std::chrono::steady_clock::now() + std::chrono::seconds(2));
}

void Engine::retryOnlineVodFreesongPlayback() {
  if (shuttingDown_.load() || !isOnlineVodRuntimeEnabled()) return;
  int layerId = -1;
  uint64_t generation = 0;
  bool paused = false;
  {
    std::lock_guard<std::mutex> lock(onlineVodFreesongFetchMutex_);
    if (onlineVodFreesongFetchInFlight_) return;
    if (onlineVodFreesongPendingPlayLayerId_ >= 0 &&
        onlineVodFreesongPendingPlayGeneration_ !=
        onlineVodRoomStateGeneration_.load()) {
      onlineVodFreesongPendingPlayLayerId_ = -1;
      onlineVodFreesongPendingPlayGeneration_ = 0;
      onlineVodFreesongPendingPlayPaused_ = false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < onlineVodFreesongNextFetchAllowed_) return;
    if (onlineVodFreesongPendingPlayLayerId_ >= 0 && !onlineVodPauseRequested_.load()) {
      layerId = onlineVodFreesongPendingPlayLayerId_;
      generation = onlineVodFreesongPendingPlayGeneration_;
      paused = onlineVodFreesongPendingPlayPaused_;
    } else if (now < onlineVodFreesongNextRefresh_) {
      return;
    }
  }
  scheduleOnlineVodFreesongFetchAsync(layerId, layerId >= 0, false,
      layerId >= 0 ? "idle_playback_retry" : "idle_list_reconcile", generation, paused);
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

bool Engine::enqueueOnlineVodServerAction(std::function<void()> action) {
  if (!action || shuttingDown_.load()) return false;
  std::lock_guard<std::mutex> lock(onlineVodPendingActionsMutex_);
  if (shuttingDown_.load()) return false;
  onlineVodPendingActions_.push_back(std::move(action));
  return true;
}

void Engine::processOnlineVodServerPendingActions() {
  if (shuttingDown_.load()) return;

  // This runs on the render/update thread. Never drain an unbounded command
  // burst in one frame: stream open/close and resource state changes can be
  // expensive even when their I/O preparation is delegated to workers.
  constexpr size_t kMaxActionsPerFrame = 2;
  constexpr auto kFrameBudget = std::chrono::milliseconds(2);
  const auto start = std::chrono::steady_clock::now();
  size_t processed = 0;
  while (processed < kMaxActionsPerFrame && !shuttingDown_.load()) {
    std::function<void()> action;
    size_t remaining = 0;
    {
      std::lock_guard<std::mutex> lock(onlineVodPendingActionsMutex_);
      if (onlineVodPendingActions_.empty()) break;
      action = std::move(onlineVodPendingActions_.front());
      onlineVodPendingActions_.pop_front();
      remaining = onlineVodPendingActions_.size();
    }
    if (action) action();
    ++processed;
    if (std::chrono::steady_clock::now() - start >= kFrameBudget) {
      if (remaining > 0) {
        static int budgetLogCount = 0;
        if (++budgetLogCount <= 5 || budgetLogCount % 300 == 0) {
          LOG_WARN("[RenderBudget] OnlineVod actions deferred: processed=%zu "
                   "remaining=%zu budgetMs=2",
                   processed, remaining);
        }
      }
      break;
    }
  }
}

bool Engine::handleOnlineVodPlaybackCommand(const std::string& action,
                                     const Json::Value& param,
                                     const std::string& rawJson) {
  (void)rawJson;
  if (!commandRouter_ || !systemConfig_) return false;
  // A control must affect the decoder committed by any in-flight room open.
  // Otherwise Pause can hit the outgoing decoder and the new song starts again.
  std::unique_lock<std::mutex> playbackLock(onlineVodPlaybackApplyMutex_);

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
  const int configuredAudioLayerId = systemConfig_->getAudioOutputLayerId();
  const int audioLayerId =
      configuredAudioLayerId > 0 &&
              AudioOutputLayerPolicy::isValidConfiguredLayer(
                  mubu_.get(), configuredAudioLayerId)
          ? configuredAudioLayerId
          : 1;
  if (configuredAudioLayerId > 0 && audioLayerId != configuredAudioLayerId) {
    LOG_WARN("[OnlineVod] Ignore non-audio configured layer %d; use layer 1",
             configuredAudioLayerId);
  }
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
    if (!param.isMember("volume") || !param["volume"].isNumeric()) {
      LOG_ERROR("[OnlineVod] Unmute rejected: missing numeric volume");
      return false;
    }
    const int volume = param["volume"].asInt();
    if (volume < 0 || volume > 100) {
      LOG_ERROR("[OnlineVod] Unmute rejected: volume out of range (%d)", volume);
      return false;
    }
    cmdParam["action"] = "setSystemVolume";
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

void Engine::applyPlaybackOperationHintConfig() {
  if (!systemConfig_ || !mubu_) return;
  Layer* layer = mubu_->getLayer(41);
  if (!layer || layer->getType() != LayerType::TEXT) return;
  auto* renderer = static_cast<LayerText*>(layer)->getMessageHintRenderer();
  if (renderer) {
    renderer->setPlaybackOperationHintEnabled(
        systemConfig_->isPlaybackOperationHintEnabled());
  }
}

void Engine::applyVodConfigNow() {
  if (!systemConfig_) return;
  applyPlaybackOperationHintConfig();
  const bool serverMode = systemConfig_->isNetworkVodEnabled();
  // Leave a valid player grant active when network VOD is enabled. Only a
  // server-only grant needs to be restored to the normal local/cloud path
  // when network VOD is turned off.
  if (licenseManager_ && !serverMode && licenseManager_->isServerLicenseMode()) {
    onlineVodAdmissionAuthorized_.store(false, std::memory_order_release);
    stopOnlineVodSync();
    licenseManager_->checkLicense(true);
    refreshLicenseLayers();
  }
  const int vodMode = systemConfig_->getVodMode();
  const bool anyVodEnabled = isVodRuntimeEnabled();
  const bool localVodEnabled = systemConfig_->isLocalVodEnabled();
  const bool networkVodEnabled = isOnlineVodRuntimeEnabled();
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
    if (!localSongDatabase_ || !localSongDatabase_->isOpen() || localSongDatabase_->isTemporary()) {
      const std::string runtimeSongDbPath = DB_DIR + "song.db";
      if (!localSongDatabase_) {
        localSongDatabase_ = std::make_unique<LocalSongDatabase>();
      }
      if (!localSongDatabase_->initialize(runtimeSongDbPath)) {
        LOG_WARN("[VOD] LocalSongDatabase hot init failed: %s", runtimeSongDbPath.c_str());
      } else {
        LOG_INFO("[VOD] LocalSongDatabase hot init ok: %s temporary=%d", runtimeSongDbPath.c_str(),
                 localSongDatabase_->isTemporary() ? 1 : 0);
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

void Engine::applyOnlineVodRoomDeviceName(const std::string& roomName) {
  std::string normalized = roomName;
  normalized.erase(normalized.begin(),
                   std::find_if(normalized.begin(), normalized.end(),
                                [](unsigned char ch) { return !std::isspace(ch); }));
  normalized.erase(
      std::find_if(normalized.rbegin(), normalized.rend(),
                   [](unsigned char ch) { return !std::isspace(ch); }).base(),
      normalized.end());
  const bool hasControlCharacter = std::any_of(
      normalized.begin(), normalized.end(),
      [](unsigned char ch) { return std::iscntrl(ch) != 0; });
  if (normalized.empty() || normalized.size() > 63 || hasControlCharacter) {
    if (!normalized.empty()) {
      LOG_WARN("[OnlineVod] ignored invalid roomName for device naming");
    }
    return;
  }

#ifdef __ANDROID__
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(onlineVodDeviceNameMutex_);
    requestedOnlineVodDeviceName_ = normalized;
    if (normalized == appliedOnlineVodDeviceName_ ||
        onlineVodDeviceNameApplyInFlight_) {
      return;
    }
    if (normalized == failedOnlineVodDeviceName_ &&
        onlineVodDeviceNameLastFailure_.time_since_epoch().count() != 0 &&
        now - onlineVodDeviceNameLastFailure_ < std::chrono::seconds(30)) {
      return;
    }
    onlineVodDeviceNameApplyInFlight_ = true;
  }

  trackEngineAsyncTask(std::async(std::launch::async, [this]() {
    while (!shuttingDown_.load()) {
      std::string target;
      {
        std::lock_guard<std::mutex> lock(onlineVodDeviceNameMutex_);
        target = requestedOnlineVodDeviceName_;
      }

      std::string error;
      const bool ok = applyJavaAssociatedDeviceName(target, error);
      bool needsAnotherPass = false;
      {
        std::lock_guard<std::mutex> lock(onlineVodDeviceNameMutex_);
        if (ok) {
          appliedOnlineVodDeviceName_ = target;
          failedOnlineVodDeviceName_.clear();
          onlineVodDeviceNameLastFailure_ = {};
        } else {
          failedOnlineVodDeviceName_ = target;
          onlineVodDeviceNameLastFailure_ = std::chrono::steady_clock::now();
        }
        needsAnotherPass = requestedOnlineVodDeviceName_ != target;
        if (!needsAnotherPass) onlineVodDeviceNameApplyInFlight_ = false;
      }

      if (ok) {
        LOG_INFO("[OnlineVod] associated device name applied from roomName: %s",
                 target.c_str());
      } else {
        LOG_ERROR("[OnlineVod] associated device name apply failed: %s",
                  error.empty() ? "unknown" : error.c_str());
      }
      if (!needsAnotherPass) return;
    }

    std::lock_guard<std::mutex> lock(onlineVodDeviceNameMutex_);
    onlineVodDeviceNameApplyInFlight_ = false;
  }));
#else
  (void)normalized;
#endif
}

void Engine::onOnlineVodStateChanged(const VodDatabase::OnlineVodState& st) {
  applyOnlineVodRoomDeviceName(st.name);
  if (!mubu_ || !systemConfig_) return;

  HintType hint = HintType::NONE;
  const bool hadPreviousRoomState = onlineVodStateSeen_;
  onlineVodStateSeen_ = true;
  const std::string prevSongId = lastOnlineVodSongId_;
  if (st.currentQueueItemId.empty()) {
    lastVodForwardQueueId_.clear();
  } else if (st.playState == 1 && st.currentQueueItemId != lastVodForwardQueueId_) {
    lastVodForwardQueueId_ = st.currentQueueItemId;
    Json::Value fields(Json::objectValue);
    fields["lightCode"] = st.currentSongLightCode;
    fields["categoryCode"] = st.currentSongCategoryCode;
    Json::Value raw;
    std::string error;
    if (JsonUtils::parseJson(st.rawJson, raw, error)) {
      fields["songNo"] = raw["playingNow"].get("songNo", "");
    }
    PeripheralManager::getInstance().dispatchVodEvent("Song", fields);
  }
  const uint64_t stateGeneration = onlineVodRoomStateGeneration_.fetch_add(1) + 1;
  bool roomStatePlaySubmitted = false;

  // 房态是首次启动和断线重连后的权威值；音量/静音不能只依赖增量 command。
  const int serverVolume = std::max(0, std::min(100, st.volume));
  const float appliedVolume = st.muteStatus != 0
                                  ? 0.0f
                                  : static_cast<float>(serverVolume) / 100.0f;
  systemConfig_->setSystemVolume(appliedVolume);
  LOG_INFO("[OnlineVod] authoritative room state: generation=%llu playState=%d volume=%d mute=%d micStatus=%d songId=%s",
           static_cast<unsigned long long>(stateGeneration), st.playState,
           serverVolume, st.muteStatus, st.micStatus, st.currentSongId.c_str());

  if (isOnlineVodRuntimeEnabled()) {
    int vodLayerId = systemConfig_->getVodLayerId();
    if (vodLayerId < 1) vodLayerId = 1;
    Layer* layer = mubu_->getLayer(vodLayerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      // Serialize room-state decisions with asynchronous media opens. A newer
      // state increments the generation before waiting here, so it always gets
      // the final stop/pause/play decision after an older open completes.
      const auto decisionLockWaitStart = std::chrono::steady_clock::now();
      std::unique_lock<std::mutex> playbackApplyLock(onlineVodPlaybackApplyMutex_);
      const auto decisionLockWaitMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - decisionLockWaitStart)
              .count();
      if (decisionLockWaitMs >= 100) {
        LOG_WARN("[OnlineVodTiming] room-state decision waited %lldms for playback lock: generation=%llu songId=%s",
                 static_cast<long long>(decisionLockWaitMs),
                 static_cast<unsigned long long>(stateGeneration),
                 st.currentSongId.c_str());
      }
      if (stateGeneration != onlineVodRoomStateGeneration_.load()) {
        LOG_INFO("[OnlineVod] discard stale room-state decision: generation=%llu",
                 static_cast<unsigned long long>(stateGeneration));
        return;
      }

      Json::Value roomStateJson(Json::objectValue);
      std::string parseError;
      Json::Value playingNow(Json::nullValue);
      if (JsonUtils::parseJson(st.rawJson, roomStateJson, parseError) &&
          roomStateJson.isObject() && roomStateJson.isMember("playingNow") &&
          roomStateJson["playingNow"].isObject()) {
        playingNow = roomStateJson["playingNow"];
      }

      std::string songPath = playingNow.isObject()
                                 ? playingNow.get("songPath", "").asString()
                                 : "";
      for (char& c : songPath) {
        if (c == '\\') c = '/';
      }
      const int configuredTrack = playingNow.isObject()
                                      ? playingNow.get("track", 0).asInt()
                                      : 0;
      const int desiredTrack = (configuredTrack == 4 || configuredTrack == 5)
                                   ? (st.micStatus == 1 ? 5 : 4)
                                   : (st.micStatus == 1 ? 3 : 2);
      const bool isIdleState = st.currentSongId == "IDLE";
      const bool hasQueueSong = !st.currentSongId.empty() &&
                                st.currentSongId != "IDLE" &&
                                st.currentSongId != "ERROR";
      const bool validSongPath = !songPath.empty() && songPath.back() != '/';
      const auto layerState = videoLayer->getState();
      const std::string currentLayerPath = videoLayer->getCurrentPath();
      const bool layerHasActiveMedia =
          layerState == LayerVideo::PlayState::PLAYING ||
          layerState == LayerVideo::PlayState::PAUSED;
      std::string boundPlaybackPath;
      std::string boundQueueItemId;
      bool boundPlaybackWasIdle = false;
      {
        std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
        boundPlaybackPath = onlineVodPlaybackMediaPath_;
        boundQueueItemId = onlineVodPlaybackQueueItemId_;
        boundPlaybackWasIdle = onlineVodLastPlaybackWasIdle_;
      }
      const bool samePlaybackIdentity =
          isIdleState
              ? boundPlaybackWasIdle
              : (!st.currentQueueItemId.empty() &&
                 boundQueueItemId == st.currentQueueItemId);
      const std::string serverMediaPath =
          normalizeOnlineVodMediaPathForCompare(songPath);
      const bool sameIdleSelection =
          isIdleState && onlineVodIdleStatePath_.has_value() &&
          *onlineVodIdleStatePath_ == serverMediaPath;
      // Idle songs advance locally at EOF. An unchanged server selection is a
      // state update, even if the layer has already advanced to another file.
      const bool sameMedia =
          (layerHasActiveMedia ||
           (hasQueueSong && (videoLayer->isPlaybackFinished() ||
                            videoLayer->isPlaybackFailed() ||
                            onlineVodDurationFinished_))) &&
          samePlaybackIdentity &&
          isSameOnlineVodMediaPath(currentLayerPath, boundPlaybackPath) &&
          (!isIdleState || sameIdleSelection ||
           (validSongPath && isSameOnlineVodMediaPath(currentLayerPath, songPath)));
      if (isIdleState) {
        onlineVodIdleStatePath_ = serverMediaPath;
      } else {
        onlineVodIdleStatePath_.reset();
      }
      const uint64_t transportGeneration = onlineVodTransportGeneration_.load();
      const bool reconnectSameMedia =
          sameMedia && layerHasActiveMedia && st.playState == 1 &&
          transportGeneration != onlineVodPlaybackTransportGeneration_;
      const double resumePosition = reconnectSameMedia
                                        ? videoLayer->getCurrentPosition()
                                        : -1.0;

      if (!isIdleState && hasQueueSong && st.playState != 0 &&
          st.currentQueueItemId.empty()) {
        LOG_ERROR("[OnlineVod] active room state rejected: missing playingNow.queueItemId songId=%s path=%s",
                  st.currentSongId.c_str(), songPath.c_str());
      } else if (!isIdleState && st.playState == 0) {
        // 正式歌曲处于停止态时不得因 roomStateChanged 被重新拉起。
        if (layerState != LayerVideo::PlayState::STOPPED) {
          videoLayer->stop();
          LOG_INFO("[OnlineVod] synchronized stopped state for songId=%s",
                   st.currentSongId.c_str());
        }
        std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
        lastOnlineVodSongPath_.clear();
        onlineVodLastPlaybackWasIdle_ = false;
        onlineVodPlaybackQueueItemId_.clear();
        onlineVodPlaybackMediaPath_.clear();
      } else if (sameMedia && !reconnectSameMedia) {
        // 相同媒体仍必须同步播放/暂停、原伴唱，不能把房态误当成重复播放而整体跳过。
        if (st.playState == 2 && layerState == LayerVideo::PlayState::PLAYING) {
          videoLayer->pause();
        } else if (st.playState == 1 && layerState == LayerVideo::PlayState::PAUSED) {
          videoLayer->resume();
        }
        if (videoLayer->getState() != LayerVideo::PlayState::STOPPED) {
          applyOnlineVodTrackType(videoLayer, desiredTrack);
        }
        {
          std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
          lastOnlineVodSongPath_ = normalizeOnlineVodMediaPathForCompare(currentLayerPath);
          onlineVodLastPlaybackWasIdle_ = isIdleState;
          onlineVodPlaybackMediaPath_ = lastOnlineVodSongPath_;
          onlineVodPlaybackQueueItemId_ =
              isIdleState ? std::string() : st.currentQueueItemId;
        }
        LOG_INFO("[OnlineVod] synchronized same media: state=%d path=%s serverPath=%s",
                 st.playState, currentLayerPath.c_str(), songPath.c_str());
      } else if (!validSongPath && !reconnectSameMedia) {
        // 空闲态没有配置媒体是合法状态，不能循环向服务器请求切歌。
        if (isIdleState) {
          bool previousPlaybackWasIdle = false;
          {
            std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
            previousPlaybackWasIdle = onlineVodLastPlaybackWasIdle_;
            onlineVodLastPlaybackWasIdle_ = true;
            if (!previousPlaybackWasIdle) lastOnlineVodSongPath_.clear();
          }
          if (layerHasActiveMedia && !previousPlaybackWasIdle) {
            videoLayer->stop();
            LOG_INFO("[OnlineVod] idle room state stopped the completed queue song");
          }
          const bool freesongStarted = tryPlayFreesongOnLayer(
              vodLayerId, stateGeneration, st.playState == 2);
          if (freesongStarted) {
            LOG_INFO("[OnlineVod] idle room state has no media path; cached freesong is playing");
          } else {
            LOG_INFO("[OnlineVod] idle room state has no media path; freesong fetch scheduled or deferred, no skip requested");
          }
        } else if (hasQueueSong && st.playState == 1) {
          LOG_WARN("[OnlineVod] active queue song has invalid path, requesting skip: songId=%s path=%s",
                   st.currentSongId.c_str(), songPath.c_str());
          requestOnlineVodSkipAsync("invalid_active_room_state_song_path",
                                     st.currentQueueItemId);
        } else {
          LOG_INFO("[OnlineVod] room state has no playable media and is not active; no skip requested");
        }
      } else {
        if (!isIdleState) {
          refreshOnlineVodQueueAndApplyAsync("room_state_changed");
        }
        onlineVodDurationFinished_ = false;
        {
          std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
          onlineVodLastSwitchTime_ = std::chrono::steady_clock::now();
        }
        lastOnlineVodSongId_ = st.currentSongId;
        roomStatePlaySubmitted = true;

        VodDatabase::OnlineVodState roomState = st;
        if (reconnectSameMedia) {
          // Reopen the actual local idle song at its current position, rather
          // than returning to the server's earlier idle selection.
          roomStateJson["playingNow"]["songPath"] = currentLayerPath;
          roomState.rawJson = JsonUtils::toString(roomStateJson);
        }
        const std::string songTitle = st.currentSongTitle;
        const bool showSwitchHint = hadPreviousRoomState && !reconnectSameMedia &&
                                   (isIdleState || !samePlaybackIdentity);
        if (reconnectSameMedia) {
          LOG_INFO("[OnlineVod] rebuilding media after control reconnect: transport=%llu position=%.3f path=%s",
                   static_cast<unsigned long long>(transportGeneration),
                   resumePosition, songPath.c_str());
        }
        trackEngineAsyncTask(std::async(
            std::launch::async,
            [this, vodLayerId, roomState, songTitle, showSwitchHint,
             stateGeneration, desiredTrack, isIdleState, hasQueueSong,
             transportGeneration, resumePosition, samePlaybackIdentity]() {
              const auto applyLockWaitStart = std::chrono::steady_clock::now();
              std::unique_lock<std::mutex> applyLock(onlineVodPlaybackApplyMutex_);
              const auto applyLockWaitMs =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - applyLockWaitStart)
                      .count();
              if (applyLockWaitMs >= 100) {
                LOG_WARN("[OnlineVodTiming] media open waited %lldms for playback lock: generation=%llu songId=%s",
                         static_cast<long long>(applyLockWaitMs),
                         static_cast<unsigned long long>(stateGeneration),
                         roomState.currentSongId.c_str());
              }
              if (shuttingDown_.load() ||
                  stateGeneration != onlineVodRoomStateGeneration_.load()) {
                LOG_INFO("[OnlineVod] discard stale room-state playback before open: generation=%llu",
                         static_cast<unsigned long long>(stateGeneration));
                return;
              }

              const PlaybackResult playResult = tryPlayOnlineVodStateSongOnLayer(
                  vodLayerId, roomState, resumePosition);
              if (shuttingDown_.load() ||
                  stateGeneration != onlineVodRoomStateGeneration_.load()) {
                LOG_INFO("[OnlineVod] room-state playback became stale after open: generation=%llu",
                         static_cast<unsigned long long>(stateGeneration));
                return;
              }

              bool played = playResult.isSuccess();
              if (isIdleState && !played) {
                // Startup can also fail before a decoder opens (resource busy,
                // release timeout, etc.). Every unsuccessful idle request must
                // reach the fallback's bounded retry path, even without another
                // room-state event. Generation/admission/pause guards still apply.
                {
                  std::lock_guard<std::mutex> lock(onlineVodSwitchMutex_);
                  onlineVodLastPlaybackWasIdle_ = true;
                  lastOnlineVodSongPath_.clear();
                  onlineVodPlaybackMediaPath_.clear();
                  onlineVodPlaybackQueueItemId_.clear();
                }
                LOG_WARN("[OnlineVod] idle room-state playback failed: result=%s; trying freesong fallback",
                         toString(playResult.code));
                played = tryPlayFreesongOnLayer(
                    vodLayerId, stateGeneration, roomState.playState == 2);
                if (!played) {
                  LOG_WARN("[OnlineVod] idle freesong fallback unavailable or fetch pending; no skip requested");
                  return;
                }
              }
              if (!played) {
                const bool permanentMediaFailure =
                    playResult.code == PlaybackResultCode::OpenFailed ||
                    playResult.code == PlaybackResultCode::DecodeFailed ||
                    playResult.code == PlaybackResultCode::UnsupportedFormat;
                if (!isIdleState && hasQueueSong && roomState.playState == 1 &&
                    !samePlaybackIdentity && resumePosition < 0.0 &&
                    !playResult.softBlocked &&
                    permanentMediaFailure) {
                  LOG_WARN("[OnlineVod] play failed for '%s', requesting skip from server",
                           songTitle.c_str());
                  requestOnlineVodSkipAsync("room_state_play_failed",
                                                roomState.currentQueueItemId);
                } else {
                  LOG_WARN("[OnlineVod] playback deferred/failed; keep queue: state=%d result=%s resume=%.3f title=%s",
                           roomState.playState, toString(playResult.code),
                           resumePosition, songTitle.c_str());
                }
                return;
              }

              if (stateGeneration != onlineVodRoomStateGeneration_.load()) {
                LOG_INFO("[OnlineVod] room-state playback became stale after open: generation=%llu",
                         static_cast<unsigned long long>(stateGeneration));
                return;
              }

              onlineVodPlaybackTransportGeneration_ = transportGeneration;

              Layer* currentLayer = mubu_ ? mubu_->getLayer(vodLayerId) : nullptr;
              if (currentLayer && currentLayer->getType() == LayerType::VIDEO) {
                LayerVideo* currentVideo = static_cast<LayerVideo*>(currentLayer);
                applyOnlineVodTrackType(currentVideo, desiredTrack);
                if ((roomState.playState == 2 || onlineVodPauseRequested_.load()) &&
                    currentVideo->getState() == LayerVideo::PlayState::PLAYING) {
                  currentVideo->pause();
                }
              }

              if (showSwitchHint && systemConfig_ && mubu_ &&
                  systemConfig_->hasLayerConfig(41)) {
                Layer* hintBaseLayer = mubu_->getLayer(41);
                if (hintBaseLayer && hintBaseLayer->getType() == LayerType::TEXT) {
                  LayerText* hintLayer = static_cast<LayerText*>(hintBaseLayer);
                  hintLayer->setVisible(true);
                  hintLayer->showOperationHint(HintType::NEXT, "");
                }
              }
            }));
      }
    }
  }

  if (!roomStatePlaySubmitted && hadPreviousRoomState &&
      st.currentSongId != prevSongId) {
    if (!st.currentSongId.empty() && st.currentSongId != "IDLE" &&
        st.currentSongId != "ERROR") {
      hint = HintType::NEXT;
    }
    LOG_INFO("OnlineVodStateChanged: songId changed (%s -> %s)",
             prevSongId.c_str(), st.currentSongId.c_str());
  }
  lastOnlineVodSongId_ = st.currentSongId;

  if (hint == HintType::NONE) return;
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
      applyPlaybackOperationHintConfig();
      mubu_->updateLayers(deltaTime);
    }
  });

  runStage("stageControl.update", [&]() {
    if (licenseManager_ && licenseManager_->isModuleEnabled("stage"))
      StageControlManager::getInstance().update();
  });

  // OnlineVod WS 线程写入的待执行项（Stop / Play 媒体 / 外设类 command）在主线程消费
  runStage("processOnlineVodServerPendingActions", [&]() {
    processOnlineVodServerPendingActions();
  });
  runStage("retryOnlineVodFreesongPlayback", [&]() {
    retryOnlineVodFreesongPlayback();
  });
  runStage("retryOnlineVodSkipAsync", [&]() { retryOnlineVodSkipAsync(); });

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

  runStage("updateLyricTick", [&]() { updateLyricTick(); });
  runStage("refreshConfiguredQrCode", [&]() { refreshConfiguredQrCode(); });

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

    if (mubu_->getLayer(layerId)) continue;
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

    // 主动出让 CPU 时间片，平滑 I/O 和内存分配压力
    if ((i + 1) % 4 == 0 && i < totalLayers - 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  if (createdCount > 0) {
    mubu_->sortLayersByPriority(true);
    applyPlaybackOperationHintConfig();
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
void Engine::createLayersFromConfig(const std::vector<int>& preservedLayerIds) {
#ifdef USE_NEW_LAYER_INIT
  // 使用新的图层初始化器框架（重构版本）
  createLayersFromConfigNew(preservedLayerIds);
#else
  // 使用旧的图层初始化方式（原版本）
  createLayersFromConfigOld(preservedLayerIds);
#endif
}

// 新的图层初始化方法（使用工厂模式）
void Engine::createLayersFromConfigNew(const std::vector<int>& preservedLayerIds) {
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
  int initializedCount = factory.initializeAllLayers(context, preservedLayerIds);

  LOG_INFO("[Engine] Layer initialization complete: %d layers initialized using new framework",
           initializedCount);
}

// 旧的图层初始化方法（保留用于回退）
void Engine::createLayersFromConfigOld(const std::vector<int>& preservedLayerIds) {
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
      if (std::find(preservedLayerIds.begin(), preservedLayerIds.end(), layerId) !=
          preservedLayerIds.end()) continue;
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

          // Engine restores QR cards only after resolving device-specific content.
          if (layerId == 71) {
            imageLayer->setContentReady(false);
          }
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
          // 歌词字体只使用公共配置；普通文本图层保留各自字体。
          const std::string configuredFont =
              layerId == 21 ? systemConfig_->getLyricFont() : config.fontPath;
          if (!configuredFont.empty()) {
            std::string fullFontPath = FONT_DIR + configuredFont;
            std::string normalizedFontPath =
                FileUtils::normalizePath(fullFontPath);
            textLayer->setFontPath(FileUtils::exists(normalizedFontPath)
                                      ? normalizedFontPath
                                      : fullFontPath);
          } else if (layerId == 21) {
            textLayer->setFontPath("");
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
