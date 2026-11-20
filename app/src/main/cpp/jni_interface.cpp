#include "core/Engine.h"
#include "core/LicenseManager.h"
#include "platform/JavaHttpsBridge.h"
#include "platform/NetworkHttpBridge.h"
#include "platform/ConfigBackupBridge.h"
#include "platform/AssociatedDeviceName.h"
#include "platform/GroupClockBridge.h"
#include "audio/AudioPlayerManager.h"
#include "NativeUsbAdbMirrorClient.h"
#include "AirPlayRkmppSession.h"
#include "decoder/frame/DecodedFrame.h"
#include "layer/LayerVideo.h"
#include "layer/LayerMirror.h"
#include "foundation/PathConfig.h"
#include "utils/Logger.h"
#include "utils/JsonUtils.h"
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

std::string g_appVersion = "1.0.0";

static jclass g_hsvjEngineClass = nullptr;
static jmethodID g_executeRestartMethod = nullptr;
static jmethodID g_scheduleWatchdogAlarmMethod = nullptr;
static jmethodID g_offerGroupLeaderTimeMethod = nullptr;
static jmethodID g_httpsGetForNativeMethod = nullptr;
static jmethodID g_applyNetworkIpConfigMethod = nullptr;
static jmethodID g_applyPowerScheduleMethod = nullptr;
static jmethodID g_getDeviceHsNameMethod = nullptr;
static jmethodID g_applyAssociatedDeviceNameMethod = nullptr;
static jmethodID g_sendBootLogoChangeMethod = nullptr;
static jmethodID g_controlBluetoothMethod = nullptr;
static jmethodID g_controlMirrorServiceMethod = nullptr;
static jmethodID g_controlMirrorServiceWithPayloadMethod = nullptr;

static bool isEngineReady() {
  return g_engine && g_engine->isInitialized();
}

static std::string jstringToUtf8(JNIEnv* env, jstring value) {
  std::string result;
  if (!env || !value) {
    return result;
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars) {
    result = chars;
    env->ReleaseStringUTFChars(value, chars);
  } else if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  return result;
}

static std::mutex g_nativeUsbMirrorMutex;
static std::unique_ptr<hsvj::NativeUsbAdbMirrorClient> g_nativeUsbMirrorClient;
static std::mutex g_nativeAirPlayMirrorMutex;
static std::unique_ptr<hsvj::AirPlayRkmppSession> g_nativeAirPlayMirrorSession;

static hsvj::LayerMirror* getMirrorLayer(int layerId) {
  if (!isEngineReady()) return nullptr;
  hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
  if (!layer || layer->getType() != hsvj::LayerType::MIRROR) return nullptr;
  return static_cast<hsvj::LayerMirror*>(layer);
}

static void deliverMirrorDecodedFrame(int layerId,
                                      hsvj::H264RkmppFrame& decodedFrame) {
  hsvj::DecodedFrame* frame = decodedFrame.frame;
  decodedFrame.frame = nullptr;
  if (!frame) return;

  hsvj::LayerMirror* mirrorLayer = getMirrorLayer(layerId);
  if (!mirrorLayer) {
    frame->release();
    return;
  }
  mirrorLayer->updateDecodedFrame(frame,
                                  decodedFrame.originalWidth,
                                  decodedFrame.originalHeight,
                                  decodedFrame.cropOffsetY);
}

static void stopNativeAirPlayMirrorSession() {
  std::unique_ptr<hsvj::AirPlayRkmppSession> session;
  {
    std::lock_guard<std::mutex> lock(g_nativeAirPlayMirrorMutex);
    session = std::move(g_nativeAirPlayMirrorSession);
  }
  if (session) session->stop();
}

static void stopNativeUsbMirrorClient() {
  std::unique_ptr<hsvj::NativeUsbAdbMirrorClient> client;
  {
    std::lock_guard<std::mutex> lock(g_nativeUsbMirrorMutex);
    client = std::move(g_nativeUsbMirrorClient);
  }
  if (client) client->stop();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  g_jvm = vm;

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
  (void)vm;
  (void)reserved;
  // Decoder callbacks access LayerMirror through g_engine. Stop every native
  // producer before destroying the engine and its layers.
  stopNativeAirPlayMirrorSession();
  stopNativeUsbMirrorClient();
  if (g_engine) {
    g_engine->shutdown();
    delete g_engine;
    g_engine = nullptr;
  }

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
  g_offerGroupLeaderTimeMethod = nullptr;
  g_httpsGetForNativeMethod = nullptr;
  g_applyNetworkIpConfigMethod = nullptr;
  g_applyPowerScheduleMethod = nullptr;
  g_getDeviceHsNameMethod = nullptr;
  g_applyAssociatedDeviceNameMethod = nullptr;
  g_sendBootLogoChangeMethod = nullptr;
  g_controlBluetoothMethod = nullptr;
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

namespace hsvj {

std::string callJavaControlBluetooth(
    const std::string& action, const std::string& payload) {
  if (!g_jvm || !g_hsvjEngineClass) {
    LOG_WARN("Java bluetooth control not available");
    return "{\"ok\":false,\"message\":\"Java bluetooth control not available\"}";
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread for controlBluetooth");
      return "{\"ok\":false,\"message\":\"AttachCurrentThread failed\"}";
    }
  } else if (result != JNI_OK || !env) {
    LOG_ERROR("Failed to get JNIEnv for controlBluetooth");
    return "{\"ok\":false,\"message\":\"GetEnv failed\"}";
  }

  if (!g_controlBluetoothMethod) {
    g_controlBluetoothMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "controlBluetooth",
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_controlBluetoothMethod = nullptr;
    }
  }
  if (!g_controlBluetoothMethod) {
    if (attached) g_jvm->DetachCurrentThread();
    return "{\"ok\":false,\"message\":\"controlBluetooth method not found\"}";
  }

  jstring jAction = env->NewStringUTF(action.c_str());
  jstring jPayload = env->NewStringUTF(payload.c_str());
  jstring jResult = static_cast<jstring>(env->CallStaticObjectMethod(
      g_hsvjEngineClass, g_controlBluetoothMethod, jAction, jPayload));
  std::string value = "{\"ok\":false,\"message\":\"empty bluetooth result\"}";
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    value = "{\"ok\":false,\"message\":\"controlBluetooth threw exception\"}";
  } else if (jResult) {
    const char *chars = env->GetStringUTFChars(jResult, nullptr);
    if (chars) {
      value = chars;
      env->ReleaseStringUTFChars(jResult, chars);
    }
    env->DeleteLocalRef(jResult);
  }
  if (jAction) env->DeleteLocalRef(jAction);
  if (jPayload) env->DeleteLocalRef(jPayload);
  if (attached) g_jvm->DetachCurrentThread();
  return value;
}

} // namespace hsvj

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

bool applyJavaAssociatedDeviceName(const std::string& name, std::string& error) {
  if (!g_jvm || !g_hsvjEngineClass) {
    error = "Java associated-name setter not available";
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
      LOG_ERROR("Failed to attach thread for applyAssociatedDeviceName");
      return false;
    }
  } else if (result != JNI_OK || !env) {
    error = "GetEnv failed";
    LOG_ERROR("Failed to get JNIEnv for applyAssociatedDeviceName");
    return false;
  }

  if (!g_applyAssociatedDeviceNameMethod) {
    g_applyAssociatedDeviceNameMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "applyAssociatedDeviceName", "(Ljava/lang/String;)Z");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_applyAssociatedDeviceNameMethod = nullptr;
    }
  }

  bool ok = false;
  if (!g_applyAssociatedDeviceNameMethod) {
    error = "applyAssociatedDeviceName method not found";
  } else {
    jstring jName = env->NewStringUTF(name.c_str());
    if (!jName) {
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      error = "Failed to create device_hsname string";
    } else {
      ok = env->CallStaticBooleanMethod(
          g_hsvjEngineClass, g_applyAssociatedDeviceNameMethod, jName) == JNI_TRUE;
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        ok = false;
        error = "applyAssociatedDeviceName threw an exception";
      } else if (!ok) {
        error = "associated-name target update returned false";
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

// Forward DSP type changes to the Java DSP controller.
extern "C" void setDSPAudioType(int dspType) {
  if (!g_jvm || !g_dspControlClass || !g_setAudioDSPTypeMethod) {
    LOG_WARN("DSP control not available");
    return;
  }
  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
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
  env->CallStaticVoidMethod(g_dspControlClass, g_setAudioDSPTypeMethod, dspType);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    LOG_ERROR("Java DSP type callback threw an exception");
  }
  if (attached) {
    g_jvm->DetachCurrentThread();
  }
}

// Forward DSP volume changes to the Java DSP controller.
extern "C" void setDSPVolume(float volume, int isHdmin) {
  if (!g_jvm || !g_dspControlClass || !g_setDSPVolumeMethod) {
    LOG_WARN("DSP volume control not available");
    return;
  }
  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
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
  env->CallStaticVoidMethod(g_dspControlClass, g_setDSPVolumeMethod, volume, isHdmin);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    LOG_ERROR("Java DSP volume callback threw an exception");
  }
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

extern "C" JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_getAppUpdateDeviceInfo(JNIEnv *env, jclass) {
  Json::Value device;
  device["device_serial"] = hsvj::SystemUtils::getHardwareSerial();
  device["device_model"] = hsvj::SystemUtils::getHardwareModel();
  device["device_fingerprint"] = hsvj::SystemUtils::generateDeviceFingerprint();
  device["cloud_url"] = "http://" + std::string(hsvj::LicenseManager::DEFAULT_CLOUD_HOST)
      + ":" + std::to_string(hsvj::LicenseManager::DEFAULT_CLOUD_PORT);
  return env->NewStringUTF(hsvj::JsonUtils::toString(device).c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_initialize(JNIEnv *env, jobject thiz,
                                           jobject surface,
                                           jobject assetManager,
                                           jstring appDataDir,
                                           jstring rootPath,
                                           jboolean lowMemoryMode,
                                           jstring appVersion) {
  (void)thiz;
  (void)assetManager;
  (void)appDataDir;

  // Cache app version from Java before engine initialization.
  if (appVersion) {
    const char* ver = env->GetStringUTFChars(appVersion, nullptr);
    if (ver) {
      g_appVersion = ver;
      env->ReleaseStringUTFChars(appVersion, ver);
    }
  }
  if (lowMemoryMode) {
    LOG_INFO("[Engine] Low memory mode enabled");
  }

  if (g_engine) {
    LOG_WARN("Engine already initialized, cleaning up old instance before reinitializing");
    // Stop all engine workers before replacing the instance.
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

  // Java has already synchronized resources; native only adopts rootPath.
  if (rootPath) {
    const char *root = env->GetStringUTFChars(rootPath, nullptr);
    if (root) {
      if (root[0]) {
        hsvj::setRootPath(root);
      }
      env->ReleaseStringUTFChars(rootPath, root);
    }
  }
  hsvj::initializePathConfig();

  g_engine = new hsvj::Engine();

  // Keep a global reference to the Java engine object for callbacks.
  {
    extern void setEngineJavaObject(jobject obj);
    setEngineJavaObject(thiz);
  }

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
    g_executeRestartMethod = env->GetStaticMethodID(g_hsvjEngineClass, "executeRestart", "()Z");
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
        g_hsvjEngineClass, "scheduleWatchdogAlarm", "()Z");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find scheduleWatchdogAlarm method due to JNI exception");
      g_scheduleWatchdogAlarmMethod = nullptr;
    } else if (!g_scheduleWatchdogAlarmMethod) {
      LOG_WARN("Failed to find scheduleWatchdogAlarm method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_offerGroupLeaderTimeMethod) {
    g_offerGroupLeaderTimeMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "offerGroupLeaderTime", "(JJ)V");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_offerGroupLeaderTimeMethod = nullptr;
      LOG_ERROR("Failed to find offerGroupLeaderTime JNI callback");
    }
  }

  if (g_hsvjEngineClass && !g_httpsGetForNativeMethod) {
    g_httpsGetForNativeMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "httpsGetForNative",
        "(Ljava/lang/String;II)Ljava/lang/String;");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find httpsGetForNative method due to JNI exception");
      g_httpsGetForNativeMethod = nullptr;
    } else if (!g_httpsGetForNativeMethod) {
      LOG_WARN("Failed to find httpsGetForNative method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_applyNetworkIpConfigMethod) {
    g_applyNetworkIpConfigMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "applyNetworkIpConfig",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
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
        "(ZZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;)Z");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find applyPowerSchedule method due to JNI exception");
      g_applyPowerScheduleMethod = nullptr;
    } else if (!g_applyPowerScheduleMethod) {
      LOG_WARN("Failed to find applyPowerSchedule method - GetStaticMethodID returned NULL");
    }
  }

  if (g_hsvjEngineClass && !g_controlBluetoothMethod) {
    g_controlBluetoothMethod = env->GetStaticMethodID(
        g_hsvjEngineClass, "controlBluetooth",
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Failed to find controlBluetooth method due to JNI exception");
      g_controlBluetoothMethod = nullptr;
    } else if (!g_controlBluetoothMethod) {
      LOG_WARN("Failed to find controlBluetooth method - GetStaticMethodID returned NULL");
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

  LOG_INFO("Engine initialize result: %s", initResult ? "true" : "false");

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
  (void)env;
  (void)thiz;
  // The library may remain loaded after shutdown(), so stop producer threads
  // here as well as in JNI_OnUnload().
  stopNativeAirPlayMirrorSession();
  stopNativeUsbMirrorClient();
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
  (void)env;
  (void)thiz;
  if (isEngineReady()) {
    g_engine->notifySurfaceDestroyed();
  } else {
    LOG_WARN("notifySurfaceDestroyed called before engine ready");
  }
}

extern "C" JNIEXPORT void JNICALL Java_com_hsvj_engine_HSVJEngine_update(
    JNIEnv *env, jobject thiz, jfloat deltaTime) {
  (void)env;
  (void)thiz;

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
  (void)thiz;
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
  // Bridge capture-audio source changes to DSPControl.setCaptureAudioSource.
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_restartActiveRk628CaptureForAudioRecovery(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  if (!isEngineReady()) {
    LOG_WARN("RK628 audio recovery restart requested before engine ready");
    return JNI_FALSE;
  }

  bool accepted = false;
  for (const auto &layer : g_engine->getMubu().getAllLayers()) {
    if (!layer || layer->getType() != hsvj::LayerType::VIDEO) {
      continue;
    }
    auto *videoLayer = static_cast<hsvj::LayerVideo *>(layer.get());
    if (!videoLayer->isCaptureMode() || videoLayer->getCaptureType() == "USB") {
      continue;
    }

    Json::Value command(Json::objectValue);
    Json::Value param(Json::objectValue);
    command["type"] = 0;
    command["code"] = 2;
    param["action"] = "restartCapture";
    param["layerId"] = layer->getLayerId();
    command["param"] = param;

    const hsvj::CommandResponse response =
        g_engine->getCommandRouter().processCommand(
            hsvj::JsonUtils::toString(command));
    LOG_WARN("RK628 audio recovery ordinary capture restart layer=%d accepted=%d error=0x%04X",
             layer->getLayerId(), response.ok ? 1 : 0, response.error);
    accepted = response.ok || accepted;
  }
  return accepted ? JNI_TRUE : JNI_FALSE;
}
// Notify Java when the native capture source changes.
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

// Invoke HSVJEngine.executeRestart() from a native thread.
extern "C" bool callJavaRestartMethod() {
  if (!g_jvm || !g_hsvjEngineClass || !g_executeRestartMethod) {
    LOG_WARN("Java restart method not available");
    return false;
  }
  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM");
      return false;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv");
    return false;
  }
  LOG_DEBUG("Calling Java HSVJEngine.executeRestart()");
  bool ok = env->CallStaticBooleanMethod(g_hsvjEngineClass, g_executeRestartMethod) == JNI_TRUE;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    LOG_ERROR("Java restart callback threw an exception");
    ok = false;
  }
  if (attached) {
    g_jvm->DetachCurrentThread();
  }
  LOG_DEBUG("Java restart method returned %s", ok ? "true" : "false");
  return ok;
}

std::string callJavaHttpsGet(const std::string& url,
                                          int connectTimeoutMs,
                                          int readTimeoutMs) {
  if (!g_jvm || !g_hsvjEngineClass || !g_httpsGetForNativeMethod) {
    LOG_WARN("Java HTTPS bridge is not available");
    return {};
  }
  JNIEnv* env = nullptr;
  bool attached = false;
  const jint result =
      g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      LOG_ERROR("Failed to attach thread for HTTPS bridge");
      return {};
    }
    attached = true;
  } else if (result != JNI_OK || !env) {
    LOG_ERROR("Failed to get JNIEnv for HTTPS bridge");
    return {};
  }

  std::string response;
  jstring jUrl = env->NewStringUTF(url.c_str());
  if (jUrl) {
    auto jResponse = static_cast<jstring>(env->CallStaticObjectMethod(
        g_hsvjEngineClass, g_httpsGetForNativeMethod, jUrl,
        static_cast<jint>(connectTimeoutMs), static_cast<jint>(readTimeoutMs)));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      LOG_ERROR("Java HTTPS bridge threw an exception");
    } else {
      response = jstringToUtf8(env, jResponse);
    }
    if (jResponse) env->DeleteLocalRef(jResponse);
    env->DeleteLocalRef(jUrl);
  }
  if (attached) g_jvm->DetachCurrentThread();
  return response;
}

std::string callJavaHttpRequestAcrossNetworks(const std::string& method, const std::string& url,
                                             const std::string& body, const std::string& token,
                                             int timeoutMs) {
  if (!g_jvm || !g_hsvjEngineClass) return {};
  JNIEnv* env = nullptr;
  bool attached = false;
  const jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};
    attached = true;
  } else if (result != JNI_OK || !env) return {};
  std::string response;
  if (env->PushLocalFrame(8) == JNI_OK) {
    const auto callback = env->GetStaticMethodID(g_hsvjEngineClass,
        "requestHttpAcrossNetworks",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    if (callback && !env->ExceptionCheck()) {
      jstring args[4] = {};
      const std::string* values[] = {&method, &url, &body, &token};
      bool valid = true;
      for (int i = 0; i < 4; ++i) {
        args[i] = env->NewStringUTF(values[i]->c_str());
        if (!args[i]) { valid = false; break; }
      }
      if (valid) {
        auto reply = static_cast<jstring>(env->CallStaticObjectMethod(
            g_hsvjEngineClass, callback, args[0], args[1], args[2], args[3], timeoutMs));
        if (!env->ExceptionCheck()) response = jstringToUtf8(env, reply);
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
  } else if (env->ExceptionCheck()) env->ExceptionClear();
  if (attached) g_jvm->DetachCurrentThread();
  return response;
}

std::string callJavaFileDownloadAcrossNetworks(const std::string& url, const std::string& filePath,
                                               int timeoutMs) {
  if (!g_jvm || !g_hsvjEngineClass) return {};
  JNIEnv* env = nullptr;
  bool attached = false;
  const jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};
    attached = true;
  } else if (result != JNI_OK || !env) return {};

  std::string response;
  if (env->PushLocalFrame(8) == JNI_OK) {
    const auto callback = env->GetStaticMethodID(g_hsvjEngineClass,
        "downloadFileAcrossNetworks",
        "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    if (callback && !env->ExceptionCheck()) {
      auto jUrl = env->NewStringUTF(url.c_str());
      auto jPath = env->NewStringUTF(filePath.c_str());
      if (jUrl && jPath) {
        auto reply = static_cast<jstring>(env->CallStaticObjectMethod(
            g_hsvjEngineClass, callback, jUrl, jPath, timeoutMs));
        if (!env->ExceptionCheck()) response = jstringToUtf8(env, reply);
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
  } else if (env->ExceptionCheck()) env->ExceptionClear();
  if (attached) g_jvm->DetachCurrentThread();
  return response;
}

std::string callJavaDisplayResolution(const std::string& mode) {
  if (!g_jvm || !g_hsvjEngineClass) return {};
  JNIEnv* env = nullptr;
  bool attached = false;
  const jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};
    attached = true;
  } else if (result != JNI_OK || !env) return {};
  std::string response;
  if (env->PushLocalFrame(4) == JNI_OK) {
    const auto callback = env->GetStaticMethodID(g_hsvjEngineClass, "controlDisplayResolution",
        "(Ljava/lang/String;)Ljava/lang/String;");
    if (callback && !env->ExceptionCheck()) {
      auto arg = env->NewStringUTF(mode.c_str());
      if (arg) {
        auto reply = static_cast<jstring>(env->CallStaticObjectMethod(g_hsvjEngineClass, callback, arg));
        if (!env->ExceptionCheck()) response = jstringToUtf8(env, reply);
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
  } else if (env->ExceptionCheck()) env->ExceptionClear();
  if (attached) g_jvm->DetachCurrentThread();
  return response;
}

std::string callJavaConfigBackup(const std::string& operation,
                                const std::string& root, const std::string& work) {
  if (!g_jvm || !g_hsvjEngineClass) return {};
  JNIEnv* env = nullptr;
  bool attached = false;
  const jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};
    attached = true;
  } else if (result != JNI_OK || !env) return {};
  std::string response;
  if (env->PushLocalFrame(8) == JNI_OK) {
    const auto callback = env->GetStaticMethodID(g_hsvjEngineClass, "processConfigBackup",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (callback && !env->ExceptionCheck()) {
      auto op = env->NewStringUTF(operation.c_str());
      auto rootArg = env->NewStringUTF(root.c_str());
      auto workArg = env->NewStringUTF(work.c_str());
      if (op && rootArg && workArg) {
        auto reply = static_cast<jstring>(env->CallStaticObjectMethod(
            g_hsvjEngineClass, callback, op, rootArg, workArg));
        if (!env->ExceptionCheck()) response = jstringToUtf8(env, reply);
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
  } else if (env->ExceptionCheck()) env->ExceptionClear();
  if (attached) g_jvm->DetachCurrentThread();
  return response;
}

extern "C" bool callJavaScheduleWatchdogAlarm() {
  if (!g_jvm || !g_hsvjEngineClass || !g_scheduleWatchdogAlarmMethod) {
    LOG_WARN("Java watchdog alarm method not available");
    return false;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM for watchdog alarm");
      return false;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv for watchdog alarm");
    return false;
  }

  LOG_DEBUG("Calling Java HSVJEngine.scheduleWatchdogAlarm()");
  bool ok = env->CallStaticBooleanMethod(g_hsvjEngineClass, g_scheduleWatchdogAlarmMethod) == JNI_TRUE;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    LOG_ERROR("Java scheduleWatchdogAlarm threw an exception");
    ok = false;
  }

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
  return ok;
}

extern "C" void callJavaOfferGroupLeaderTime(std::int64_t utcMs,
                                            std::int64_t localMonoNs) {
  if (!g_jvm || !g_hsvjEngineClass || !g_offerGroupLeaderTimeMethod) return;
  JNIEnv *env = nullptr;
  bool attached = false;
  const jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    attached = true;
  } else if (result != JNI_OK) {
    return;
  }
  env->CallStaticVoidMethod(g_hsvjEngineClass, g_offerGroupLeaderTimeMethod,
                           static_cast<jlong>(utcMs), static_cast<jlong>(localMonoNs));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    LOG_ERROR("Group leader time JNI callback failed");
  }
  if (attached) g_jvm->DetachCurrentThread();
}

extern "C" bool callJavaApplyNetworkIpConfig(const std::string& mode, const std::string& staticIp,
                                             const std::string& gateway, const std::string& dns) {
  if (!g_jvm || !g_hsvjEngineClass || !g_applyNetworkIpConfigMethod) {
    LOG_WARN("Java applyNetworkIpConfig method not available");
    return false;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM for applyNetworkIpConfig");
      return false;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv for applyNetworkIpConfig");
    return false;
  }

  jstring jMode = env->NewStringUTF(mode.c_str());
  jstring jStaticIp = env->NewStringUTF(staticIp.c_str());
  jstring jGateway = env->NewStringUTF(gateway.c_str());
  jstring jDns = env->NewStringUTF(dns.c_str());
  bool ok = env->CallStaticBooleanMethod(g_hsvjEngineClass, g_applyNetworkIpConfigMethod, jMode, jStaticIp, jGateway, jDns) == JNI_TRUE;
  env->DeleteLocalRef(jMode);
  env->DeleteLocalRef(jStaticIp);
  env->DeleteLocalRef(jGateway);
  env->DeleteLocalRef(jDns);

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
  return ok;
}

extern "C" bool callJavaApplyPowerSchedule(bool scheduleEnabled, bool powerOnEnabled,
                                           const std::string& powerOnDate, const std::string& powerOnTime,
                                           bool powerOffEnabled, const std::string& powerOffDate,
                                           const std::string& powerOffTime) {
  if (!g_jvm || !g_hsvjEngineClass || !g_applyPowerScheduleMethod) {
    LOG_WARN("Java applyPowerSchedule method not available");
    return false;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  jint result = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    } else {
      LOG_ERROR("Failed to attach thread to JVM for applyPowerSchedule");
      return false;
    }
  } else if (result != JNI_OK) {
    LOG_ERROR("Failed to get JNIEnv for applyPowerSchedule");
    return false;
  }

  jstring jPowerOnDate = env->NewStringUTF(powerOnDate.c_str());
  jstring jPowerOnTime = env->NewStringUTF(powerOnTime.c_str());
  jstring jPowerOffDate = env->NewStringUTF(powerOffDate.c_str());
  jstring jPowerOffTime = env->NewStringUTF(powerOffTime.c_str());
  bool ok = env->CallStaticBooleanMethod(g_hsvjEngineClass, g_applyPowerScheduleMethod,
                            static_cast<jboolean>(scheduleEnabled),
                            static_cast<jboolean>(powerOnEnabled),
                            jPowerOnDate, jPowerOnTime,
                            static_cast<jboolean>(powerOffEnabled),
                            jPowerOffDate, jPowerOffTime) == JNI_TRUE;
  env->DeleteLocalRef(jPowerOnDate);
  env->DeleteLocalRef(jPowerOnTime);
  env->DeleteLocalRef(jPowerOffDate);
  env->DeleteLocalRef(jPowerOffTime);

  if (attached) {
    g_jvm->DetachCurrentThread();
  }
  return ok;
}

// Push PCM data to active capture-backed video layers.
extern "C" JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_pushAudioData(JNIEnv *env, jclass clazz,
                                               jshortArray data, jint numFrames,
                                               jint sampleRate) {
  (void)clazz;
  if (!isEngineReady() || !data || numFrames <= 0) {
    return;
  }
  jshort *pcm = env->GetShortArrayElements(data, nullptr);
  if (!pcm) {
    return;
  }
  for (int layerId : {10, 11}) {
    hsvj::Layer *layer = g_engine->getMubu().getLayer(layerId);
    if (layer && layer->getType() == hsvj::LayerType::VIDEO) {
      auto *videoLayer = static_cast<hsvj::LayerVideo *>(layer);
      if (videoLayer->isCaptureMode()) {
        videoLayer->pushAudioData(reinterpret_cast<const int16_t *>(pcm), numFrames, sampleRate);
      }
    }
  }
  env->ReleaseShortArrayElements(data, pcm, JNI_ABORT);
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
Java_com_hsvj_engine_HSVJEngine_startNativeAirPlayMirror(
        JNIEnv *env, jclass clazz, jint layerId) {
    (void)env;
    (void)clazz;
    if (!getMirrorLayer(layerId)) {
        LOG_ERROR("[AirPlayRkmppJNI] mirror layer %d unavailable", layerId);
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(g_nativeAirPlayMirrorMutex);
    if (g_nativeAirPlayMirrorSession) {
        g_nativeAirPlayMirrorSession->stop();
        g_nativeAirPlayMirrorSession.reset();
    }

    auto session = std::make_unique<hsvj::AirPlayRkmppSession>();
    if (!session->start(layerId, [](int callbackLayerId,
                                    hsvj::H264RkmppFrame& frame) {
          deliverMirrorDecodedFrame(callbackLayerId, frame);
        })) {
        LOG_ERROR("[AirPlayRkmppJNI] failed to start native session layer=%d",
                  layerId);
        return JNI_FALSE;
    }
    g_nativeAirPlayMirrorSession = std::move(session);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_pushNativeAirPlayVideo(
        JNIEnv *env, jclass clazz, jbyteArray data, jint nalType, jlong ptsUs,
        jint width, jint height, jint codec) {
    (void)clazz;
    if (!data) return JNI_FALSE;

    const jsize size = env->GetArrayLength(data);
    if (size <= 0) return JNI_FALSE;

    std::vector<uint8_t> payload(static_cast<size_t>(size));
    env->GetByteArrayRegion(data, 0, size,
                            reinterpret_cast<jbyte*>(payload.data()));
    if (env->ExceptionCheck()) {
        LOG_ERROR("[AirPlayRkmppJNI] failed to copy AirPlay NAL bytes=%d", size);
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(g_nativeAirPlayMirrorMutex);
    if (!g_nativeAirPlayMirrorSession ||
        !g_nativeAirPlayMirrorSession->isRunning()) {
        return JNI_FALSE;
    }
    return g_nativeAirPlayMirrorSession->enqueue(
               std::move(payload), nalType, static_cast<int64_t>(ptsUs),
               width, height, codec)
        ? JNI_TRUE
        : JNI_FALSE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_stopNativeAirPlayMirror(
        JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    stopNativeAirPlayMirrorSession();
}
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_startNativeUsbAdbMirror(
        JNIEnv *env, jclass clazz, jint layerId, jint usbFd,
        jint bulkInEndpoint, jint bulkOutEndpoint, jint preferredWidth,
        jint preferredHeight, jstring scrcpyServerPath, jstring keyDir,
        jboolean adbAlreadyConnected, jboolean foregroundMonitorEnabled) {
    (void)clazz;
    if (!getMirrorLayer(layerId)) {
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
                                   hsvj::H264RkmppFrame& frame) {
        deliverMirrorDecodedFrame(callbackLayerId, frame);
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
    stopNativeUsbMirrorClient();
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
Java_com_hsvj_engine_HSVJEngine_setExternalBluetoothAudioActive(
        JNIEnv *env, jclass clazz, jboolean active) {
    (void)env;
    (void)clazz;
    if (isEngineReady()) {
        g_engine->setBluetoothAudioMode(active == JNI_TRUE);
    } else {
        hsvj::AudioPlayerManager::getInstance().setExternalBluetoothAudioActive(active == JNI_TRUE);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setBluetoothAudioSpectrum(
        JNIEnv *env, jclass clazz, jfloatArray spectrum) {
    (void)clazz;
    if (!isEngineReady() || spectrum == nullptr) {
        return;
    }
    const jsize size = env->GetArrayLength(spectrum);
    if (size <= 0) {
        return;
    }
    std::vector<float> values(static_cast<size_t>(size));
    env->GetFloatArrayRegion(spectrum, 0, size, values.data());
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    g_engine->setBluetoothAudioSpectrum(values);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setBluetoothAudioMetadata(
        JNIEnv *env, jclass clazz, jstring title, jstring artist,
        jlong durationMs, jstring lyricFrame, jlong lyricPositionMs) {
    (void)clazz;
    if (!isEngineReady()) {
        return;
    }

    g_engine->setBluetoothAudioMetadata(jstringToUtf8(env, title),
                                        jstringToUtf8(env, artist),
                                        static_cast<int64_t>(durationMs),
                                        jstringToUtf8(env, lyricFrame),
                                        static_cast<int64_t>(lyricPositionMs));
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setBluetoothAudioPlayback(
        JNIEnv *env, jclass clazz, jint state, jlong positionMs, jfloat speed) {
    (void)env;
    (void)clazz;
    if (!isEngineReady()) {
        return;
    }
    g_engine->setBluetoothAudioPlayback(static_cast<int>(state),
                                        static_cast<int64_t>(positionMs),
                                        static_cast<float>(speed));
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
Java_com_hsvj_engine_HSVJEngine_setMirrorServiceState(
        JNIEnv *env, jclass clazz, jint layerId, jstring serviceName,
        jboolean started, jboolean connected, jstring deviceName, jint pinCode) {
    (void)clazz;
    if (!isEngineReady()) return;

    std::string serviceNameValue;
    if (serviceName) {
        const char* value = env->GetStringUTFChars(serviceName, nullptr);
        if (value) {
            serviceNameValue = value;
            env->ReleaseStringUTFChars(serviceName, value);
        }
    }

    std::string deviceNameValue;
    if (deviceName) {
        const char* value = env->GetStringUTFChars(deviceName, nullptr);
        if (value) {
            deviceNameValue = value;
            env->ReleaseStringUTFChars(deviceName, value);
        }
    }

    hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
    if (layer && layer->getType() == hsvj::LayerType::MIRROR) {
        auto* mirrorLayer = static_cast<hsvj::LayerMirror*>(layer);
        mirrorLayer->setServiceState(started == JNI_TRUE, serviceNameValue,
                                     connected == JNI_TRUE, deviceNameValue,
                                     pinCode);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hsvj_engine_HSVJEngine_setMirrorTvModeActive(
        JNIEnv *env, jclass clazz, jint layerId, jboolean active) {
    (void)env;
    (void)clazz;
    if (!isEngineReady()) return;

    hsvj::Layer* layer = g_engine->getMubu().getLayer(layerId);
    if (layer && layer->getType() == hsvj::LayerType::MIRROR) {
        auto* mirrorLayer = static_cast<hsvj::LayerMirror*>(layer);
        mirrorLayer->setTvModeActive(active == JNI_TRUE);
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

// [AdaptiveFPS] Render pacing JNI bridge.
extern "C"
JNIEXPORT jdouble JNICALL
Java_com_hsvj_engine_HSVJEngine_getActiveVideoFps(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0.0;
    return g_engine->getActiveVideoFps();
}

// [AdaptiveFPS] Render pacing JNI bridge.
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
Java_com_hsvj_engine_HSVJEngine_getSuccessfulPresentCount(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0;
    return static_cast<jlong>(g_engine->getSuccessfulPresentCount());
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_hsvj_engine_HSVJEngine_getSwapchainNoImageSkipCount(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return 0;
    return static_cast<jlong>(g_engine->getSwapchainNoImageSkipCount());
}

// [MirrorGate] Mirror layer lookup JNI bridge.
extern "C"
JNIEXPORT jint JNICALL
Java_com_hsvj_engine_HSVJEngine_getFirstMirrorLayerId(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (!isEngineReady()) return -1;
    return g_engine->getFirstMirrorLayerId();
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_hsvj_engine_HSVJEngine_getMirrorControlConfigJson(JNIEnv *env, jobject thiz) {
    (void)thiz;
    Json::Value result(Json::objectValue);
    if (!isEngineReady()) {
        result["configured"] = false;
        result["mode"] = "none";
        result["enabled"] = false;
        result["sceneName"] = "";
        return env->NewStringUTF(hsvj::JsonUtils::toString(result).c_str());
    }

    const hsvj::SystemConfig& config = g_engine->getSystemConfig();
    result["configured"] = config.hasMirrorControlConfig();
    result["mode"] = config.getMirrorAutoStartMode();
    result["enabled"] = config.isUsbAppSceneEnabled();
    result["sceneName"] = config.getUsbAppSceneName();
    return env->NewStringUTF(hsvj::JsonUtils::toString(result).c_str());
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hsvj_engine_HSVJEngine_saveMirrorControlConfig(
        JNIEnv *env, jobject thiz, jstring autoStartMode, jboolean appSceneEnabled,
        jstring sceneName) {
    (void)thiz;
    if (!isEngineReady() || autoStartMode == nullptr || sceneName == nullptr) {
        return JNI_FALSE;
    }

    const char *modeChars = env->GetStringUTFChars(autoStartMode, nullptr);
    const char *sceneChars = env->GetStringUTFChars(sceneName, nullptr);
    if (modeChars == nullptr || sceneChars == nullptr) {
        if (modeChars != nullptr) {
            env->ReleaseStringUTFChars(autoStartMode, modeChars);
        }
        if (sceneChars != nullptr) {
            env->ReleaseStringUTFChars(sceneName, sceneChars);
        }
        return JNI_FALSE;
    }
    const std::string mode(modeChars);
    const std::string scene(sceneChars);
    env->ReleaseStringUTFChars(autoStartMode, modeChars);
    env->ReleaseStringUTFChars(sceneName, sceneChars);

    if (mode != "none" && mode != "usb" && mode != "android" && mode != "apple") {
        return JNI_FALSE;
    }

    hsvj::SystemConfig& config = g_engine->getSystemConfig();
    const bool oldConfigured = config.hasMirrorControlConfig();
    const std::string oldMode = config.getMirrorAutoStartMode();
    const bool oldEnabled = config.isUsbAppSceneEnabled();
    const std::string oldSceneName = config.getUsbAppSceneName();
    config.setMirrorControlConfig(true, mode, appSceneEnabled == JNI_TRUE, scene);
    if (config.save(hsvj::CONFIG_PATH)) {
        return JNI_TRUE;
    }
    config.setMirrorControlConfig(oldConfigured, oldMode, oldEnabled, oldSceneName);
    return JNI_FALSE;
}
