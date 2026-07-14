#ifndef HSVJ_HARDWARE_MANAGER_H
#define HSVJ_HARDWARE_MANAGER_H

#include "decoder/frame/FrameTypes.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef __ANDROID__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#else
typedef int AVCodecID;
typedef void AVCodec;
typedef void AVCodecContext;
typedef void AVBufferRef;
enum AVHWDeviceType { AV_HWDEVICE_TYPE_NONE = 0 };
#endif

namespace hsvj {

/**
 * @brief 硬件解码管理器
 * 
 * 核心功能：
 * - 自动检测可用硬件解码器
 * - 智能选择最佳解码器（根据设备类型）
 * - 管理硬件设备上下文
 * - 配置零拷贝模式
 * - 自动降级机制
 */
class HardwareManager {
public:
  /**
   * @brief 解码器优先级策略
   */
  enum class DecoderPriority {
    RKMPP_FIRST       // 仅使用 RKMPP
  };

  /**
   * @brief 解码器信息
   */
  struct DecoderInfo {
    std::string name;           // 解码器名称（如 "h264_rkmpp"）
    AVCodecID codecId;          // 编码ID
    bool isHardware;            // 是否为硬件解码
    bool isZeroCopyCapable;     // 是否支持零拷贝
    int priority;               // 优先级（数字越小越优先）
    bool isAvailable;           // 是否可用
  };

  HardwareManager();
  ~HardwareManager();

  /**
   * @brief 初始化硬件管理器
   * @return 是否初始化成功
   */
  bool initialize();

  /**
   * @brief 根据编码格式选择最佳解码器
   * @param codecId 编码ID（如 AV_CODEC_ID_H264）
   * @return 解码器指针（失败返回 nullptr）
   */
  const AVCodec* selectBestDecoder(AVCodecID codecId);

  /**
   * @brief 配置硬件解码
   * @param codec 解码器
   * @param codecCtx 解码器上下文
   * @return 是否配置成功
   */
  bool configureHardwareDecoding(const AVCodec* codec, AVCodecContext* codecCtx);

  /**
   * @brief 配置零拷贝模式
   * @param codec 解码器
   * @param codecCtx 解码器上下文
   * @return 零拷贝模式
   */
  ZeroCopyMode configureZeroCopy(const AVCodec* codec, AVCodecContext* codecCtx);

  /**
   * @brief 标记解码器失败（用于自动降级）
   * @param 解码器Name 解码器名称
   */
  void markDecoderFailed(const std::string& decoderName);

  /**
   * @brief 检查是否为硬件解码器
   */
  bool isHardwareDecoder(const AVCodec* codec) const;

  /**
   * @brief 获取硬件设备上下文
   */
  AVBufferRef* getHardwareDeviceContext() const { return hwDeviceCtx_; }

  /**
   * @brief 获取零拷贝模式
   */
  ZeroCopyMode getZeroCopyMode() const { return zeroCopyMode_; }

  /**
   * @brief 是否为 RK 设备
   */
  bool isRKDevice() const { return isRKDevice_; }

  /**
   * @brief 是否为 RK356x 平台
   */
  bool isRK356xDevice() const { return isRK356xDevice_; }

  /**
   * @brief 是否为 V03P 机型
   */
  bool isV03PDevice() const { return isV03PDevice_; }

  /**
   * @brief 获取解码器统计信息
   */
  std::vector<DecoderInfo> getAllDecoders() const;

private:
  // 检测可用解码器
  void detectAvailableDecoders();
  void detectH264Decoders();
  void detectHEVCDecoders();
  void detectMPEG1Decoders();
  void detectMPEG2Decoders();
  void detectMJPEGDecoders();
  void detectVP8Decoders();
  void detectVP9Decoders();
  
  // 设备检测
  static bool detectRKChip();
  // 硬件配置
    bool createHardwareDeviceContext(const AVCodec* codec, AVCodecContext* codecCtx);

  // 解码器映射表：codecId -> 解码器列表（按优先级排序）
    std::map<AVCodecID, std::vector<DecoderInfo>> decoderMap_;

  // 硬件设备上下文
  AVBufferRef* hwDeviceCtx_;
  AVHWDeviceType hwDeviceType_;

  // 零拷贝支持
  ZeroCopyMode zeroCopyMode_;

  // 设备信息
    bool isRKDevice_;
  DecoderPriority priority_;
  bool isRK356xDevice_;
  bool isV03PDevice_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_HARDWARE_MANAGER_H
