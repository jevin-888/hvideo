#ifndef SYSTEMUTILS_H
#define SYSTEMUTILS_H

#include <string>
#include <json/json.h>

// DSP 音频控制函数声明
// Android 平台：定义在 jni_interface.cpp 中
extern "C" {
    void setDSPAudioType(int dspType);
    void setDSPVolume(float volume, int isHdmin);
}

namespace hsvj {

enum class AudioOutputPath {
    System,
    CaptureHdmi,
    CaptureMipi,
    CaptureUsb,
    ExternalHdmi
};

inline bool usesHdmiInputVolumePolicy(AudioOutputPath path) {
    return path == AudioOutputPath::CaptureHdmi ||
           path == AudioOutputPath::CaptureMipi ||
           path == AudioOutputPath::ExternalHdmi;
}

inline void setManagedOutputVolume(float volume, AudioOutputPath path) {
    setDSPVolume(volume, usesHdmiInputVolumePolicy(path) ? 1 : 0);
}

inline AudioOutputPath audioOutputPathFromCaptureType(const std::string& captureType) {
    if (captureType == "HDMI") return AudioOutputPath::CaptureHdmi;
    if (captureType == "MIPI") return AudioOutputPath::CaptureMipi;
    if (captureType == "USB") return AudioOutputPath::CaptureUsb;
    return AudioOutputPath::System;
}

/**
 * @brief 系统相关的实用工具函数
 */
class SystemUtils {
public:
    /**
     * @brief 获取网络接口信息
     * @return 包含网络接口信息的 Json::Value 数组
     */
    static Json::Value getNetworkInterfaces();

    /**
     * @brief 获取设备名称 (Hostname)
     * @return 设备名称
     */
    static std::string getDeviceName();

    /**
     * @brief 获取 CPU 逻辑核数（用于将多核汇总使用率归一化到 0–100%）
     * @return 核数，无法获取时返回 0
     */
    static int getCpuCoreCount();

    /**
     * @brief 获取当前 CPU 使用率 (0.0 - 100.0)
     * @return CPU 使用率百分比
     */
    static double getCpuUsage();

    /**
     * @brief 获取当前进程内存使用信息
     * @return 包含内存信息的 Json::Value
     */
    static Json::Value getMemoryInfo();

    /**
     * @brief 获取本机主要 IP 地址
     * @return IP 地址字符串，如果获取失败返回空字符串
     */
    static std::string getLocalIp();

    // 设备信息（文档 A1，用于授权/设备信息导出）
    static std::string getHardwareModel();
    static std::string getHardwareSerial();
    static std::string getCpuSerial();
    static std::string getStorageSerial();
    static std::string getMacAddress();
    /**
     * @brief 设备指纹：基于型号/CPU/存储等稳定硬件信息的哈希
     *
     * 注意：不再包含网络 MAC，确保同一设备在有线/无线等不同网络环境下指纹保持一致。
     */
    static std::string generateDeviceFingerprint();

    /** 由 Java 经 JNI 注入（绕过 SELinux 对 sysfs 的限制），设备上报时优先使用 */
    static void setDeviceInfoFromJava(const std::string& serial, const std::string& model, const std::string& mac);
};

} // 命名空间 hsvj

#endif // 结束 SYSTEMUTILS_H
