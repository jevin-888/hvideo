#include "core/Engine.h"
#include "NativeUsbAdbMirrorClient.h"
#include "decoder/frame/DecodedFrame.h"
#include "layer/LayerVideo.h"
#include "layer/LayerMirror.h"
#include "core/PathConfig.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <android/hardware_buffer_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <jni.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static hsvj::Engine *g_engine = nullptr;
static ANativeWindow *g_window = nullptr;
JavaVM *g_jvm = nullptr;
static jclass g_dspControlClass = nullptr;
static jmethodID g_setAudioDSPTypeMethod = nullptr;
static jmethodID g_setDSPVolumeMethod = nullptr;

// 应用版本号（由 Java 层在 initialize 时传入 BuildConfig.VERSION_NAME）
std::string g_appVersion = "1.0.0";

// 保存 HSVJEngine 类的引用，用于调用 executeRestart 方法
static jclass g_hsvjEngineClass = nullptr;
static jmethodID g_executeRestartMethod = nullptr;
static jmethodID g_scheduleWatchdogAlarmMethod = nullptr;
static jmethodID g_applyNetworkIpConfigMethod = nullptr;
static jmethodID g_applyPowerScheduleMethod = nullptr;
static jmethodID g_getDeviceHsNameMethod = nullptr;
static jmethodID g_setDeviceHsNameMethod = nullptr;
static jmethodID g_sendBootLogoChangeMethod = nullptr;
static jmethodID g_controlMirrorServiceMethod = nullptr;
static jmethodID g_controlMirrorServiceWithPayloadMethod = nullptr;

static bool isEngineReady() {
  return g_engine && g_engine->isInitialized();
}

static std::mutex g_nativeUsbMirrorMutex;
static std::unique_ptr<hsvj::NativeUsbAdbMirrorClient> g_nativeUsbMirrorClient;

static hsvj::LayerMirror* getMirrorLayerForUsbMirror(int layerId) {
  if (!isEngineReady()) return nullptr;
  hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
  if (!layer || layer->getType() != hsvj::LayerType::MIRROR) return nullptr;
  return static_cast<hsvj::LayerMirror*>(layer);
}

static void deliverUsbMirrorFrame(int layerId,
                                  hsvj::UsbH264RkmppFrame decodedFrame) {
  if (!decodedFrame.frame) return;
  hsvj::LayerMirror* mirrorLayer = getMirrorLayerForUsbMirror(layerId);
  if (!mirrorLayer) {
    decodedFrame.frame->release();
    return;
  }
  mirrorLayer->updateDecodedFrame(decodedFrame.frame,
                                  decodedFrame.originalWidth,
                                  decodedFrame.originalHeight,
                                  decodedFrame.cropOffsetY);
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved; // JNI标准接口参数，暂时未使用
  g_jvm = vm;

  // 初始化DSP控制相关JNI引用
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) {
    g_dspControlClass =
        (jclass)env->NewGlobalRef(env->FindClass("com/hsvj/engine/DSPControl"));
    if (g_dspControlClass) {
      g_setAudioDSPTypeMethod =
          env->GetStaticMethodID(g_dspControlClass, "setAudioDSPType", "(I)V");
      if (!g_setAudioDSPTypeMethod) {
        LOG_WARN("Failed to find setAudioDSPType method");
      }

      g_setDSPVolumeMethod =
          env->GetStaticMethodID(g_dspControlClass, "setDSPVolume", "(FI)V");
      if (!g_setDSPVolumeMethod) {
        LOG_WARN("Failed to find setDSPVolume method");
      }
    } else {
      LOG_WARN("Failed to find DSPControl class");
    }
  }

  return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
  (void)vm;       // JNI标准接口参数，暂时未使用
  (void)reserved; // JNI标准接口参数，暂时未使用
  if (g_engine) {
    g_engine->shutdown();
    delete g_engine;
    g_engine = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    if (g_nativeUsbMirrorClient) {
      g_nativeUsbMirrorClient->stop();
      g_nativeUsbMirrorClient.reset();
    }
  }

  // 清理 DSP 控制相关JNI引用
  if (g_jvm) {
    JNIEnv *env = nullptr;
    if (g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
        JNI_OK) {
      if (g_dspControlClass) {
        env->DeleteGlobalRef(g_dspControlClass);
        g_dspControlClass = nullptr;
      }
      if (g_hsvjEngineClass) {
        env->DeleteGlobalRef(g_hsvjEngineClass);
        g_hsvjEngineClass = nullptr;
      }
    }
  }
  g_setAudioDSPTypeMethod = nullptr;
  g_setDSPVolumeMethod = nullptr;
  g_executeRestartMethod = nullptr;
  g_scheduleWatchdogAlarmMethod = nullptr;
  g_applyNetworkIpConfigMethod = nullptr;
  g_applyPowerScheduleMethod = nullptr;
  g_getDeviceHsNameMethod = nullptr;
  g_setDeviceHsNameMethod = nullptr;
  g_sendBootLogoChangeMethod = nullptr;
  g_controlMirrorServiceMethod = nullptr;
  g_controlMirrorServiceWithPayloadMethod = nullptr;
  g_jvm = nullptr;
}

std::string controlJavaMirrorService(const std::string& action, int layerId,
                                     const std::string& payload) {
  if (!g_jvm) {
    return "{\"ok\":false,\"message\":\"JNI unavailable\"}";
  }
  JNIEnv *env = nullptr;
  bool attached = false;
  jint getEnvResult = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (getEnvResult == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      return "{\"ok\":false,\"message\":\"AttachCurrentThread failed\"}";
    }
    attached = true;
  } else if (getEnvResult != JNI_OK || !env) {
    return "{\"ok\":false,\"message\":\"GetEnv failed\"}";
  }

  if (!g_hsvjEngineClass) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    if (attached) g_jvm->DetachCurrentThread();
    return "{\"ok\":false,\"message\":\"HSVJEngine class unavailable\"}";
  }

  jmethodID method = nullptr;
  const bool hasPayload = !payload.empty();
  if (hasPayload) {
    if (!g_controlMirrorServiceWithPayloadMethod) {
      g_controlMirrorServiceWithPayloadMethod = env->GetStaticMethodID(
          g_hsvjEngineClass, "controlMirrorService",
          "(Ljava/lang/String;ILjava/lang/String;)Ljava/lang/String;");
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        g_controlMirrorServiceWithPayloadMethod = nullptr;
      }
    }
    method = g_controlMirrorServiceWithPayloadMethod;
  }
  if (!method) {
    if (!g_controlMirrorServiceMethod) {
      g_controlMirrorServiceMethod = env->GetStaticMethodID(
          g_hsvjEngineClass, "controlMirrorService",
          "(Ljava/lang/String;I)Ljava/lang/String;");
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        g_controlMirrorServiceMethod = nullptr;
      }
    }
    method = g_controlMirrorServiceMethod;
  }
  if (!method) {
    if (attached) g_jvm->DetachCurrentThread();
    return "{\"ok\":false,\"message\":\"controlMirrorService method not found\"}";
  }

  jstring jAction = env->NewStringUTF(action.c_str());
  if (!jAction) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    if (attached) g_jvm->DetachCurrentThread();
    return "{\"ok\":false,\"message\":\"Failed to create action string\"}";
  }

  jstring jPayload = nullptr;
  if (hasPayload && method == g_controlMirrorServiceWithPayloadMethod) {
    jPayload = env->NewStringUTF(payload.c_str());
    if (!jPayload) {
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      env->DeleteLocalRef(jAction);
      if (attached) g_jvm->DetachCurrentThread();
      return "{\"ok\":false,\"message\":\"Failed to create payload string\"}";
    }
  }

  jstring jResult = nullptr;
  if (jPayload) {
    jResult = static_cast<jstring>(env->CallStaticObjectMethod(
        g_hsvjEngineClass, method, jAction, layerId, jPayload));
  } else {
    jResult = static_cast<jstring>(env->CallStaticObjectMethod(
        g_hsvjEngineClass, method, jAction, layerId));
  }
  std::string result = "{\"ok\":false,\"message\":\"empty result\"}";
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    result = "{\"ok\":false,\"message\":\"controlMirrorService threw exception\"}";
  }
  if (jResult) {
    const char *chars = env->GetStringUTFChars(jResult, nullptr);
    if (chars) {
      result = chars;
      env->ReleaseStringUTFChars(jResult, chars);
    }
    env->DeleteLocalRef(jResult);
  }
  if (jPayload) env->DeleteLocalRef(jPayload);
  env->DeleteLocalRef(jAction);
  if (attached) g_jvm->DetachCurrentThread();
  return result;
}

std::string controlJavaMirrorService(const std::string& action, int layerId) {
  return controlJavaMirrorService(action, layerId, "");
}

std::string getJavaDeviceHsName() {
  if (!g_jvm || !g_hsvjEngineClass) {
    LOG_WARN("Java device_hsname getter not available");
    return "";
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread for getDeviceHsName");
      return "";
    }
  } else if (result != JNI_OK || !env) {
    LOG_ERROR("Failed to get JNIEnv for getDeviceHsName");
    return "";
  }

  if (!g_getDeviceHsNameMethod) {
    g_getDeviceHsNameMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "getDeviceHsName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_getDeviceHsNameMethod = nullptr;
    }
  }

  std::string value;
  if (g_getDeviceHsNameMethod) {
    jstring jValue = static_cast<jstring>(env->CallStaticObjectMethod(
        g_hsvjEngineClass, g_getDeviceHsNameMethod));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Java getDeviceHsName threw an exception");
    } else if (jValue) {
      const char *chars = env->GetStringUTFChars(jValue, nullptr);
      if (chars) {
        value = chars;
        env->ReleaseStringUTFChars(jValue, chars);
      }
      env->DeleteLocalRef(jValue);
    }
  }

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
  return value;
}

bool setJavaDeviceHsName(const std::string& name, std::string& error) {
  if (!g_jvm || !g_hsvjEngineClass) {
    error = "Java device_hsname setter not available";
    LOG_WARN("%s", error.c_str());
    return false;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      error = "AttachCurrentThread failed";
      LOG_ERROR("Failed to attach thread for setDeviceHsName");
      return false;
    }
  } else if (result != JNI_OK || !env) {
    error = "GetEnv failed";
    LOG_ERROR("Failed to get JNIEnv for setDeviceHsName");
    return false;
  }

  if (!g_setDeviceHsNameMethod) {
    g_setDeviceHsNameMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "setDeviceHsName", "(Ljava/lang/String;)Z");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_setDeviceHsNameMethod = nullptr;
    }
  }

  bool ok = false;
  if (!g_setDeviceHsNameMethod) {
    error = "setDeviceHsName method not found";
  } else {
    jstring jName = env->NewStringUTF(name.c_str());
    if (!jName) {
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      error = "Failed to create device_hsname string";
    } else {
      ok = env->CallStaticBooleanMethod(
          g_hsvjEngineClass, g_setDeviceHsNameMethod, jName) == JNI_TRUE;
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        ok = false;
        error = "setDeviceHsName threw an exception";
      } else if (!ok) {
        error = "Settings.System.putString returned false";
      }
      env->DeleteLocalRef(jName);
    }
  }

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
  return ok;
}

bool sendJavaBootLogoChange(int slot, std::string& error) {
  if (slot < 1 || slot > 5) {
    error = "invalid boot logo slot";
    return false;
  }
  if (!g_jvm || !g_hsvjEngineClass) {
    error = "Java boot logo sender not available";
    LOG_WARN("%s", error.c_str());
    return false;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      error = "AttachCurrentThread failed";
      LOG_ERROR("Failed to attach thread for sendBootLogoChange");
      return false;
    }
  } else if (result != JNI_OK || !env) {
    error = "GetEnv failed";
    LOG_ERROR("Failed to get JNIEnv for sendBootLogoChange");
    return false;
  }

  if (!g_sendBootLogoChangeMethod) {
    g_sendBootLogoChangeMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "sendBootLogoChange", "(I)Z");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_sendBootLogoChangeMethod = nullptr;
    }
  }

  bool ok = false;
  if (!g_sendBootLogoChangeMethod) {
    error = "sendBootLogoChange method not found";
  } else {
    ok = env->CallStaticBooleanMethod(
        g_hsvjEngineClass, g_sendBootLogoChangeMethod, slot) == JNI_TRUE;
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      ok = false;
      error = "sendBootLogoChange threw an exception";
    } else if (!ok) {
      error = "sendBootLogoChange returned false";
    }
  }

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
  return ok;
}

// C++ 全局函数：设置 DSP 音频类型（供 Video解码器 调用）
extern "C" void setDSPAudioType(int dspType) {
  if (!g_jvm || !g_dspControlClass || !g_setAudioDSPTypeMethod) {
    LOG_WARN("DSP control not available");
    return;
  }

  JNIEnv *env = nullptr;
  bool attached = false;

  // 获取 JNIEnv（可能在不同线程中调用）
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    // 当前线程未附加到 JVM，需要附件
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM");
      return;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv");
    return;
  }

  // 调用 Java 方法
  env->CallStaticVoidMethod(g_dspControlClass, g_setAudioDSPTypeMethod,
                            dspType);

  // 如果附加了线程，需要分离
  if (attached) {
    g_jvm->DetachCurrentThread();
  }

  }

// C++ 全局函数：设置 DSP 音量（供 LayerVideo/AudioPlayer 调用）
// V04设备使用12级音量控制
extern "C" void setDSPVolume(float volume, int isHdmin) {
  if (!g_jvm || !g_dspControlClass || !g_setDSPVolumeMethod) {
    LOG_WARN("DSP volume control not available");
    return;
  }

  JNIEnv *env = nullptr;
  bool attached = false;

  // 获取 JNIEnv（可能在不同线程中调用）
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    // 当前线程未附加到 JVM，需要附件
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM");
      return;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv");
    return;
  }

  // 调用 Java 方法
  env->CallStaticVoidMethod(g_dspControlClass, g_setDSPVolumeMethod, volume,
                            isHdmin);

  // 如果附加了线程，需要分离
  if (attached) {
    g_jvm->DetachCurrentThread();
  }

  }

extern "C" JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setDeviceInfoForReport(JNIEnv *env, jclass clazz,
                                                       jstring serial, jstring model, jstring mac) {
  (void)clazz;
  std::string s, m, a;
  if (serial) {
    const char* p = env->GetStringUTFChars(serial, nullptr);
    if (p) { s = p; env->ReleaseStringUTFChars(serial, p); }
  }
  if (model) {
    const char* p = env->GetStringUTFChars(model, nullptr);
    if (p) { m = p; env->ReleaseStringUTFChars(model, p); }
  }
  if (mac) {
    const char* p = env->GetStringUTFChars(mac, nullptr);
    if (p) { a = p; env->ReleaseStringUTFChars(mac, p); }
  }
  hsvj::SystemUtils::setDeviceInfoFromJava(s, m, a);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_initialize(JNIEnv *env, jobject thiz,
                                           jobject surface,
                                           jobject assetManager,
                                           jstring appDataDir,
                                           jstring rootPath,
                                           jboolean lowMemoryMode,
                                           jstring appVersion) {
  (void)thiz; // JNI标准接口参数，暂时未使用
  (void)assetManager; // 资源同步由 Java 启动流程完成，JNI 不复制 assets
  (void)appDataDir; // 保留 JNI 签名兼容，路径根目录以 rootPath 为唯一来源

  // 存储版本号供 status 接口使用
  if (appVersion) {
    const char* ver = env->GetStringUTFChars(appVersion, nullptr);
    if (ver) {
      g_appVersion = ver;
      env->ReleaseStringUTFChars(appVersion, ver);
    }
  }

  // 记录低内存模式状态
  if (lowMemoryMode) {
    LOG_INFO("[内存优化] 启用低内存模式 ");
  }

  // 如果 Engine 已经存在，先清理旧的实例（处理重启场景）
  if (g_engine) {
    LOG_WARN("Engine already initialized, cleaning up old instance before reinitializing");

    // Engine::shutdown() 内部同步等待所有解码器线程 join、异步任务完成、
    // 音频回调停止，返回后可安全 delete，无需额外 sleep。
    g_engine->shutdown();
    extern void setEngineJavaObject(jobject obj);
    setEngineJavaObject(nullptr);

    delete g_engine;
    g_engine = nullptr;

    if (g_window) {
      ANativeWindow_release(g_window);
      g_window = nullptr;
    }
  }

  // Surface mode receives an ANativeWindow. DRM/KMS mode is intentionally
  // headless and creates its output buffers directly from the DRM connector.
  if (surface) {
    g_window = ANativeWindow_fromSurface(env, surface);
    if (!g_window) {
      LOG_ERROR("Failed to get native window from surface");
      return JNI_FALSE;
    }
  } else {
    g_window = nullptr;
    LOG_INFO("Headless DRM/KMS engine initialization requested");
  }

  // ========== 步骤0: Java 已选根路径并复制资源，此处仅设置同一个根路径 ==========
  if (rootPath) {
    const char* root = env->GetStringUTFChars(rootPath, nullptr);
    if (root) {
      if (root[0]) {
        hsvj::setRootPath(root);
      }
      env->ReleaseStringUTFChars(rootPath, root);
    }
  }
  hsvj::initializePathConfig();
  // 资源目录和文件已由 Java 同步到 rootPath；Native 只使用当前根目录，不在 JNI 初始化阶段复制 APK assets。

  // 创建引擎实例
  g_engine = new hsvj::Engine();

  // 保存 Java 对象的全局引用，用于进度回调
  {
    // setEngineJavaObject 内部会创建/替换 GlobalRef，这里传局部引用即可。
    extern void setEngineJavaObject(jobject obj);
    setEngineJavaObject(thiz);
  }

  // 保存 HSVJEngine 类的引用，用于调用 executeRestart 方法
  if (!g_hsvjEngineClass) {
    jclass localClass = env->FindClass("com/hsvj/engine/HSVJEngine");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find HSVJEngine class due to JNI exception");
    } else if (localClass) {
      g_hsvjEngineClass = (jclass)env->NewGlobalRef(localClass);
      env->DeleteLocalRef(localClass);
    } else {
      LOG_ERROR("Failed to find HSVJEngine class - FindClass returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_executeRestartMethod) {
    g_executeRestartMethod = env->GetStaticMethodID(g_hsvjEngineClass, "executeRestart", "()V");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find executeRestart method due to JNI exception");
      g_executeRestartMethod = nullptr;
    } else if (!g_executeRestartMethod) {
      LOG_WARN("Failed to find executeRestart method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_scheduleWatchdogAlarmMethod) {
    g_scheduleWatchdogAlarmMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "scheduleWatchdogAlarm", "()V");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find scheduleWatchdogAlarm method due to JNI exception");
      g_scheduleWatchdogAlarmMethod = nullptr;
    } else if (!g_scheduleWatchdogAlarmMethod) {
      LOG_WARN("Failed to find scheduleWatchdogAlarm method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_applyNetworkIpConfigMethod) {
    g_applyNetworkIpConfigMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "applyNetworkIpConfig",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find applyNetworkIpConfig method due to JNI exception");
      g_applyNetworkIpConfigMethod = nullptr;
    } else if (!g_applyNetworkIpConfigMethod) {
      LOG_WARN("Failed to find applyNetworkIpConfig method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_applyPowerScheduleMethod) {
    g_applyPowerScheduleMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "applyPowerSchedule",
        "(ZZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;)V");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find applyPowerSchedule method due to JNI exception");
      g_applyPowerScheduleMethod = nullptr;
    } else if (!g_applyPowerScheduleMethod) {
      LOG_WARN("Failed to find applyPowerSchedule method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_controlMirrorServiceMethod) {
    g_controlMirrorServiceMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "controlMirrorService",
        "(Ljava/lang/String;I)Ljava/lang/String;");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find controlMirrorService method due to JNI exception");
      g_controlMirrorServiceMethod = nullptr;
    } else if (!g_controlMirrorServiceMethod) {
      LOG_WARN("Failed to find controlMirrorService method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_controlMirrorServiceWithPayloadMethod) {
    g_controlMirrorServiceWithPayloadMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "controlMirrorService",
        "(Ljava/lang/String;ILjava/lang/String;)Ljava/lang/String;");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find controlMirrorService(payload) method due to JNI exception");
      g_controlMirrorServiceWithPayloadMethod = nullptr;
    } else if (!g_controlMirrorServiceWithPayloadMethod) {
      LOG_WARN("Failed to find controlMirrorService(payload) method - GetStaticMethodID returned NULL");
    }
  }

  bool initResult = false;
  if (g_window) {
    LOG_INFO("NativeWindow received");
    initResult = g_engine->initialize(g_window);
  } else {
    LOG_INFO("Initializing Engine without Android Surface");
    initResult = g_engine->initialize();
  }

  LOG_INFO("返回: %s", initResult ? "true" : "false");

  if (!initResult) {
    LOG_ERROR("Failed to initialize engine");
    extern void setEngineJavaObject(jobject obj);
    setEngineJavaObject(nullptr);
    delete g_engine;
    g_engine = nullptr;
    if (g_window) {
      ANativeWindow_release(g_window);
      g_window = nullptr;
    }
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_initializeHeadless(
    JNIEnv *env, jobject thiz, jobject assetManager, jstring appDataDir,
    jstring rootPath, jboolean lowMemoryMode, jstring appVersion) {
  return Java_com_hsvj_engine_HSVJEngine_initialize(
      env, thiz, nullptr, assetManager, appDataDir, rootPath, lowMemoryMode,
      appVersion);
}

extern "C" JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_shutdown(JNIEnv *env, jobject thiz) {
  (void)env;  // JNI标准接口参数，暂时未使用
  (void)thiz; // JNI标准接口参数，暂时未使用
  if (g_engine) {
    g_engine->shutdown();
    extern void setEngineJavaObject(jobject obj);
    setEngineJavaObject(nullptr);
    delete g_engine;
    g_engine = nullptr;
  }

  if (g_window) {
    ANativeWindow_release(g_window);
    g_window = nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_notifySurfaceDestroyed(JNIEnv *env, jobject thiz) {
  (void)env;  // JNI标准接口参数，暂时未使用
  (void)thiz; // JNI标准接口参数，暂时未使用
  if (isEngineReady()) {
    g_engine->notifySurfaceDestroyed();
  } else {
    LOG_WARN("notifySurfaceDestroyed called before engine ready");
  }
}

extern "C" JNIEXPORT void JNICALL Java_com_hsvj_engine_HSVJEngine_update(
    JNIEnv *env, jobject thiz, jfloat deltaTime) {
  (void)env;  // JNI标准接口参数，暂时未使用
  (void)thiz; // JNI标准接口参数，暂时未使用

  static int updateLogCount = 0;
  updateLogCount++;

  if (g_engine) {
    auto updateStart = std::chrono::steady_clock::now();
    g_engine->update(deltaTime);
    auto updateEnd = std::chrono::steady_clock::now();
    auto updateMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        updateEnd - updateStart)
                        .count();
    if (updateMs >= 24) {
      static int slowUpdateCount = 0;
      static auto s_lastSlowUpdateLog = std::chrono::steady_clock::time_point{};
      const auto slowUpdateLogNow = std::chrono::steady_clock::now();
      if (++slowUpdateCount <= 1 ||
          s_lastSlowUpdateLog.time_since_epoch().count() == 0 ||
          slowUpdateLogNow - s_lastSlowUpdateLog >= std::chrono::seconds(10)) {
        s_lastSlowUpdateLog = slowUpdateLogNow;
        LOG_WARN("[JNIUpdatePerf] cost=%lldms delta=%.4f count=%d",
                 static_cast<long long>(updateMs),
                 static_cast<double>(deltaTime), slowUpdateCount);
      }
    }
  } else {
    if (updateLogCount <= 5 || updateLogCount % 300 == 0)
      LOG_WARN("[RenderLoop] JNI update called but g_engine is NULL, count=%d", updateLogCount);
  }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_processCommand(JNIEnv *env, jobject thiz,
                                               jstring jsonCommand) {
  (void)thiz; // JNI标准接口参数，暂时未使用
  if (!isEngineReady()) {
    return env->NewStringUTF(
        "{\"ok\":false,\"error\":\"Engine not initialized\"}");
  }

  if (!jsonCommand) {
    return env->NewStringUTF(
        "{\"ok\":false,\"error\":\"Command is null\"}");
  }

  const char *cmdStr = env->GetStringUTFChars(jsonCommand, nullptr);
  if (!cmdStr) {
    return env->NewStringUTF(
        "{\"ok\":false,\"error\":\"Failed to read command string\"}");
  }
  hsvj::CommandResponse response =
      g_engine->getCommandRouter().processCommand(cmdStr);
  std::string result = response.toJson();
  env->ReleaseStringUTFChars(jsonCommand, cmdStr);

  return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setCaptureAudioSource(JNIEnv *env, jclass clazz,
                                                       jint sourceType) {
  (void)clazz;
  // 通过反射调用 DSPControl.setCaptureAudioSource
  jclass dspControlClass = env->FindClass("com/hsvj/engine/DSPControl");
  if (!dspControlClass) {
    LOG_ERROR("Failed to find DSPControl class for setCaptureAudioSource");
    return;
  }

  jmethodID setCaptureAudioSourceMethod =
      env->GetStaticMethodID(dspControlClass, "setCaptureAudioSource", "(I)V");
  if (!setCaptureAudioSourceMethod) {
    LOG_ERROR("Failed to find setCaptureAudioSource method");
    env->DeleteLocalRef(dspControlClass);
    return;
  }

  env->CallStaticVoidMethod(dspControlClass, setCaptureAudioSourceMethod, sourceType);
  env->DeleteLocalRef(dspControlClass);
}

// 通知 Java 层采集源类型
extern "C" void notifyJavaCaptureSource(int sourceType) {
    if (!g_jvm) return;

    JNIEnv *env = nullptr;
    bool attached = false;
    jint res = g_jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        }
    }

    if (env) {
        jclass dspControlClass = env->FindClass("com/hsvj/engine/DSPControl");
        if (dspControlClass) {
            jmethodID setCaptureAudioSourceMethod =
                env->GetStaticMethodID(dspControlClass, "setCaptureAudioSource", "(I)V");
            if (setCaptureAudioSourceMethod) {
                env->CallStaticVoidMethod(dspControlClass, setCaptureAudioSourceMethod, sourceType);
            }
            env->DeleteLocalRef(dspControlClass);
        }
    }

    if (attached) {
        g_jvm->DetachCurrentThread();
    }
}

// 调用 Java 层的重启方法
extern "C" void callJavaRestartMethod() {
  if (!g_jvm || !g_hsvjEngineClass || !g_executeRestartMethod) {
    LOG_WARN("Java restart method not available");
    return;
  }

  JNIEnv *env = nullptr;
  bool attached = false;

  // 获取 JNIEnv（可能在不同线程中调用）
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    // 当前线程未附加到 JVM，需要附件
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM");
      return;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv");
    return;
  }

  // 调用 Java 方法
  LOG_DEBUG("Calling Java HSVJEngine.executeRestart()");
  env->CallStaticVoidMethod(g_hsvjEngineClass, g_executeRestartMethod);

  // 如果附加了线程，需要分离
  if (attached) {
    g_jvm->DetachCurrentThread();
  }

  LOG_DEBUG("Java restart method called");
}

extern "C" void callJavaScheduleWatchdogAlarm() {
  if (!g_jvm || !g_hsvjEngineClass || !g_scheduleWatchdogAlarmMethod) {
    LOG_WARN("Java watchdog alarm method not available");
    return;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM for watchdog alarm");
      return;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv for watchdog alarm");
    return;
  }

  LOG_DEBUG("Calling Java HSVJEngine.scheduleWatchdogAlarm()");
  env->CallStaticVoidMethod(g_hsvjEngineClass, g_scheduleWatchdogAlarmMethod);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    LOG_ERROR("Java scheduleWatchdogAlarm threw an exception");
  }

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
}

extern "C" void callJavaApplyNetworkIpConfig(const std::string& mode, const std::string& staticIp,
                                             const std::string& gateway, const std::string& dns) {
  if (!g_jvm || !g_hsvjEngineClass || !g_applyNetworkIpConfigMethod) {
    LOG_WARN("Java applyNetworkIpConfig method not available");
    return;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM for applyNetworkIpConfig");
      return;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv for applyNetworkIpConfig");
    return;
  }

  jstring jMode = env->NewStringUTF(mode.c_str());
  jstring jStaticIp = env->NewStringUTF(staticIp.c_str());
  jstring jGateway = env->NewStringUTF(gateway.c_str());
  jstring jDns = env->NewStringUTF(dns.c_str());
  env->CallStaticVoidMethod(g_hsvjEngineClass, g_applyNetworkIpConfigMethod, jMode, jStaticIp, jGateway, jDns);
  env->DeleteLocalRef(jMode);
  env->DeleteLocalRef(jStaticIp);
  env->DeleteLocalRef(jGateway);
  env->DeleteLocalRef(jDns);

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
}

extern "C" void callJavaApplyPowerSchedule(bool scheduleEnabled, bool powerOnEnabled,
                                           const std::string& powerOnDate, const std::string& powerOnTime,
                                           bool powerOffEnabled, const std::string& powerOffDate,
                                           const std::string& powerOffTime) {
  if (!g_jvm || !g_hsvjEngineClass || !g_applyPowerScheduleMethod) {
    LOG_WARN("Java applyPowerSchedule method not available");
    return;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM for applyPowerSchedule");
      return;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv for applyPowerSchedule");
    return;
  }

  jstring jPowerOnDate = env->NewStringUTF(powerOnDate.c_str());
  jstring jPowerOnTime = env->NewStringUTF(powerOnTime.c_str());
  jstring jPowerOffDate = env->NewStringUTF(powerOffDate.c_str());
  jstring jPowerOffTime = env->NewStringUTF(powerOffTime.c_str());
  env->CallStaticVoidMethod(g_hsvjEngineClass, g_applyPowerScheduleMethod,
                            static_cast<jboolean>(scheduleEnabled),
                            static_cast<jboolean>(powerOnEnabled),
                            jPowerOnDate, jPowerOnTime,
                            static_cast<jboolean>(powerOffEnabled),
                            jPowerOffDate, jPowerOffTime);
  env->DeleteLocalRef(jPowerOnDate);
  env->DeleteLocalRef(jPowerOnTime);
  env->DeleteLocalRef(jPowerOffDate);
  env->DeleteLocalRef(jPowerOffTime);

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
}

// JNI 推送音频数据接口
extern "C" JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_pushAudioData(JNIEnv *env, jclass clazz,
                                               jshortArray data, jint numFrames,
                                               jint sampleRate) {
  (void)clazz;
  if (!isEngineReady() || !data || numFrames <= 0) return;

  // 获取 PCM 数据指针（jshortArray 对应 int16_t）
  jshort *pcm = env->GetShortArrayElements(data, nullptr);
  if (pcm) {
    // 这里的 data 假定是立体声交错的，样本总数是 numFrames * 2
    // 我们将其分发给当前处于采集模式的图层（图层 10 或 11）
    // 也可以遍历所有图层，但通常只有 10/11 需要来自外部（HDMI/MIPI）的推送
    for (int layerId : {10, 11}) {
        hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
        if (layer && layer->getType() == hsvj::LayerType::VIDEO) {
            hsvj::LayerVideo* vl = static_cast<hsvj::LayerVideo*>(layer);
            if (vl->isCaptureMode()) {
                vl->pushAudioData(reinterpret_cast<const int16_t*>(pcm), numFrames, sampleRate);
            }
        }
    }
    // 释放资源，JNI_ABORT 表示不写回 Java 数组（因为我们只是读取）
    env->ReleaseShortArrayElements(data, pcm, JNI_ABORT);
  }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_pushMirrorFrame(JNIEnv *env, jclass clazz, jint layerId,
                                               jobject hardwareBuffer, jint bufferWidth,
                                               jint bufferHeight, jint visibleWidth,
                                               jint visibleHeight) {
    (void)clazz;
    if (!isEngineReady() || !hardwareBuffer) return;

    hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
    if (layer && layer->getType() == hsvj::LayerType::MIRROR) {
        hsvj::LayerMirror* mirrorLayer = static_cast<hsvj::LayerMirror*>(layer);

#ifdef __ANDROID__
        AHardwareBuffer* buffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
        if (buffer) {
            mirrorLayer->updateFrame(buffer, bufferWidth, bufferHeight,
                                     visibleWidth, visibleHeight);
        }
#endif
    }
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_startNativeUsbAdbMirror(
        JNIEnv *env, jclass clazz, jint layerId, jint usbFd,
        jint bulkInEndpoint, jint bulkOutEndpoint, jint preferredWidth,
        jint preferredHeight, jstring scrcpyServerPath, jstring keyDir,
        jboolean adbAlreadyConnected, jboolean foregroundMonitorEnabled) {
    (void)clazz;
    if (!getMirrorLayerForUsbMirror(layerId)) {
        LOG_ERROR("[NativeUsbAdbJNI] mirror layer %d unavailable", layerId);
        return JNI_FALSE;
    }
    if (usbFd < 0 || bulkInEndpoint <= 0 || bulkOutEndpoint <= 0) {
        LOG_ERROR("[NativeUsbAdbJNI] invalid USB fd/endpoints fd=%d in=0x%x out=0x%x",
                  usbFd, bulkInEndpoint, bulkOutEndpoint);
        return JNI_FALSE;
    }

    const char* scrcpyPathChars = scrcpyServerPath
        ? env->GetStringUTFChars(scrcpyServerPath, nullptr)
        : nullptr;
    const char* keyDirChars = keyDir
        ? env->GetStringUTFChars(keyDir, nullptr)
        : nullptr;
    std::string scrcpyPath = scrcpyPathChars ? scrcpyPathChars : "";
    std::string keyDirectory = keyDirChars ? keyDirChars : "";
    if (scrcpyPathChars) env->ReleaseStringUTFChars(scrcpyServerPath, scrcpyPathChars);
    if (keyDirChars) env->ReleaseStringUTFChars(keyDir, keyDirChars);

    hsvj::NativeUsbAdbMirrorClient::Config config;
    config.layerId = layerId;
    config.usbFd = usbFd;
    config.bulkInEndpoint = bulkInEndpoint;
    config.bulkOutEndpoint = bulkOutEndpoint;
    config.preferredWidth = preferredWidth;
    config.preferredHeight = preferredHeight;
    config.scrcpyServerPath = scrcpyPath;
    config.keyDir = keyDirectory;
    config.adbAlreadyConnected = adbAlreadyConnected == JNI_TRUE;
    config.foregroundMonitorEnabled = foregroundMonitorEnabled == JNI_TRUE;
    config.videoFrameCallback = [](int callbackLayerId,
                                   hsvj::UsbH264RkmppFrame& frame) {
        deliverUsbMirrorFrame(callbackLayerId, frame);
    };

    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    if (g_nativeUsbMirrorClient) {
        g_nativeUsbMirrorClient->stop();
        g_nativeUsbMirrorClient.reset();
    }
    auto client = std::make_unique<hsvj::NativeUsbAdbMirrorClient>();
    if (!client->start(config)) {
        LOG_ERROR("[NativeUsbAdbJNI] start failed: %s",
                  client->lastMessage().c_str());
        return JNI_FALSE;
    }
    g_nativeUsbMirrorClient = std::move(client);
    LOG_INFO("[NativeUsbAdbJNI] native USB ADB mirror started layer=%d fd=%d in=0x%x out=0x%x",
             layerId, usbFd, bulkInEndpoint, bulkOutEndpoint);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_stopNativeUsbAdbMirror(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    if (g_nativeUsbMirrorClient) {
        g_nativeUsbMirrorClient->stop();
        g_nativeUsbMirrorClient.reset();
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setNativeUsbAdbMirrorForegroundMonitorEnabled(
        JNIEnv *env, jclass clazz, jboolean enabled) {
    (void)env;
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    if (g_nativeUsbMirrorClient) {
        g_nativeUsbMirrorClient->setForegroundMonitorEnabled(enabled == JNI_TRUE);
    }
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_isNativeUsbAdbMirrorRunning(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    return (g_nativeUsbMirrorClient && g_nativeUsbMirrorClient->isRunning())
        ? JNI_TRUE
        : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_isNativeUsbAdbMirrorConnected(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    return (g_nativeUsbMirrorClient && g_nativeUsbMirrorClient->isConnected())
        ? JNI_TRUE
        : JNI_FALSE;
}

static jstring nativeUsbMirrorString(JNIEnv* env,
                                     const std::function<std::string()>& getter) {
    std::string value = getter();
    return env->NewStringUTF(value.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_getNativeUsbAdbMirrorLastMessage(JNIEnv *env, jclass clazz) {
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    return nativeUsbMirrorString(env, [] {
        return g_nativeUsbMirrorClient
            ? g_nativeUsbMirrorClient->lastMessage()
            : std::string("native USB mirror stopped");
    });
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_getNativeUsbAdbMirrorForegroundPackage(JNIEnv *env, jclass clazz) {
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    return nativeUsbMirrorString(env, [] {
        return g_nativeUsbMirrorClient
            ? g_nativeUsbMirrorClient->foregroundPackage()
            : std::string();
    });
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_getNativeUsbAdbMirrorForegroundRawFocus(JNIEnv *env, jclass clazz) {
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    return nativeUsbMirrorString(env, [] {
        return g_nativeUsbMirrorClient
            ? g_nativeUsbMirrorClient->foregroundRawFocus()
            : std::string();
    });
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_getNativeUsbAdbMirrorForegroundLaunchPackage(JNIEnv *env, jclass clazz) {
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    return nativeUsbMirrorString(env, [] {
        return g_nativeUsbMirrorClient
            ? g_nativeUsbMirrorClient->foregroundLaunchPackage()
            : std::string();
    });
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_updateMirrorSourceInfo(
        JNIEnv *env, jclass clazz, jint layerId, jint physicalWidth,
        jint physicalHeight, jint streamWidth, jint streamHeight) {
    (void)env;
    (void)clazz;
    if (!isEngineReady()) return;

    hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
    if (layer && layer->getType() == hsvj::LayerType::MIRROR) {
        hsvj::LayerMirror* mirrorLayer = static_cast<hsvj::LayerMirror*>(layer);
        mirrorLayer->updateSourceInfo(physicalWidth, physicalHeight,
                                      streamWidth, streamHeight);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setMirroringState(JNIEnv *env, jclass clazz, jboolean active) {
    (void)env;
    (void)clazz;
    if (isEngineReady()) {
        g_engine->setMirroringState(active);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setMirrorPin(JNIEnv *env, jclass clazz, jint layerId, jint pinCode) {
    (void)env;
    (void)clazz;
    if (isEngineReady()) {
        hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
        if (layer && layer->getType() == hsvj::LayerType::MIRROR) {
            hsvj::LayerMirror* mirrorLayer = static_cast<hsvj::LayerMirror*>(layer);
            mirrorLayer->setPinCode(pinCode);
        }
    }
}
extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_updateLayerSize(JNIEnv *env, jclass clazz, jint layerId, jint width, jint height) {
    (void)env;
    (void)clazz;
    if (isEngineReady()) {
        hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
        if (layer) {
            if (layer->getType() == hsvj::LayerType::MIRROR) {
                hsvj::LayerMirror* mirrorLayer = static_cast<hsvj::LayerMirror*>(layer);
                mirrorLayer->updateVideoSize(width, height);
            } else if (layer->getType() == hsvj::LayerType::VIDEO) {
                hsvj::LayerVideo* videoLayer = static_cast<hsvj::LayerVideo*>(layer);
                videoLayer->updateVideoSize(width, height);
            }
        }
    }
}

// [AdaptiveFPS] 诊断接口：返回当前 PLAYING 视频图层的最大 fps（不含采集层）。
// 渲染循环应优先使用 getRenderDemandFps()。
extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getActiveVideoFps(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getActiveVideoFps();
}

// [AdaptiveFPS] 返回当前画面更新需求帧率：有动态画面时 60，静态场景时 30。
extern "C"
JNIEXPORT jint JNICALL
Java_com_hsvj_engine_HSVJEngine_getRenderDemandFps(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 30;
    return g_engine->getRenderDemandFps();
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_getRenderFrameRateMode(JNIEnv *env, jobject thiz) {
    (void)thiz;
    std::string mode = "auto";
    if (isEngineReady()) {
        mode = g_engine->getRenderFrameRateMode();
    }
    return env->NewStringUTF(mode.c_str());
}

extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getLastFrameTotalMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getLastFrameTotalMs();
}

extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getLastCpuWorkMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getLastCpuWorkMs();
}

extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getLastBeginFrameMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getLastBeginFrameMs();
}

extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getLastPresentMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getLastPresentMs();
}

extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getLastAsyncPresentMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getLastAsyncPresentMs();
}

extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getLastAsyncAcquireMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getLastAsyncAcquireMs();
}

extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getLastAsyncAcquireFenceMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getLastAsyncAcquireFenceMs();
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_hsvj_engine_HSVJEngine_getSwapchainNoImageSkipCount(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0;
    return static_cast<jlong>(g_engine->getSwapchainNoImageSkipCount());
}

// [MirrorGate] 返回 config.json 中启用的第一个 MIRROR 图层 ID；若未启用任何
// MIRROR 图层则返回 -1。Java 侧 Mirror管理器 据此决定是否启动 Lymp 服务，
// 避免硬编码 Layer 31 白白消耗 HardwareBuffer pool + TCP/HTTP 监听线程。
extern "C"
JNIEXPORT jint JNICALL
Java_com_hsvj_engine_HSVJEngine_getFirstMirrorLayerId(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return -1;
    return g_engine->getFirstMirrorLayerId();
}
