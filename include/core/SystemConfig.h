/**
 * @file SystemConfig.h（文件名）
 * @brief 系统配置类定义
 *
 * 本文件定义了系统配置类，负责：
 * - 系统配置的加载和保存
 * - 图层配置管理
 * - 切片配置管理
 * - 配置参数访问接口
 */

#ifndef HSVJ_SYSTEM_CONFIG_H
#define HSVJ_SYSTEM_CONFIG_H

#include "layer/Layer.h" // 用于位置与尺寸
#include "fusion/FusionTypes.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hsvj {

/** 音量/透明度等 0～1 参数：统一为 0.00～1.00，最多 2 位小数。 */
inline float roundVolume01(float v) {
  v = std::max(0.f, std::min(1.f, v));
  return std::round(v * 100.f) / 100.f;
}
/** 通用 2 位小数舍入，避免配置/API 出现多余小数位。 */
inline float roundFloat2(float v) {
  return std::round(v * 100.f) / 100.f;
}
/** 通用 1 位小数舍入（如字体大小）。 */
inline float roundFloat1(float v) {
  return std::round(v * 10.f) / 10.f;
}
/** 高斯模糊：限制在 0～10，保留整数步进。 */
inline float clampGaussianBlur(float v) {
  v = std::max(0.f, std::min(10.f, v));
  return std::round(v);
}

/**
 * @brief 切片配置结构
 */
struct SliceConfig {
  std::string coordinate; // "x y 宽度 高度"
  std::string range;      // "x y 宽度 高度"
  int transparency;       // 0-255
  bool enable;            // 是否启用
  bool mirror;            // 是否镜像
  std::string mask;       // 遮罩图像路径
  int priority;           // 优先级
  float rotate;           // 旋转角度
  float scale;            // 缩放比例
  int shapeType;         // 几何形状
  float shapeParam;      // 形状参数
  bool blackToTransparent; // 黑色变透明
  int invert;             // 图像反转 (0=无, 1=水平, 2=垂直, 3=水平+垂直)
  float gaussianBlur;     // 高斯模糊强度 (0-10)
  int fitMode;            // 视频/采集适配模式 (0=铺满显示, 1=保持视频比例显示)
  std::string roamConfig; // 切片漫游配置 JSON 字符串
  std::string captureType; // 采集切片独立输入源；空值表示跟随主图层
  int captureIndex;        // 采集切片独立设备索引
  Json::Value extraFields;  // 前端/未来扩展字段，归一化时透传，避免配置被清掉

  SliceConfig()
      : coordinate("0 0 1920 1080"), range("0 0 1920 1080"), transparency(255),
        enable(true), mirror(false), priority(0), rotate(0.0f), scale(1.0f),
        shapeType(0), shapeParam(0.0f), blackToTransparent(false), invert(0),
        gaussianBlur(0.0f), fitMode(0), captureType(""),
        captureIndex(0), extraFields(Json::objectValue) {}
};

/**
 * @brief 图层配置结构（存储从JSON解析的图层配置）
 */
struct LayerConfigData {
  std::string layerKey; // "layer1", "layer2"等
  int layerId;          // 图层ID（从layerKey解析）

  // 通用属性
  bool visible;      // 是否可见
  Position position; // 位置
  Size size;         // 尺寸
  float rotation;    // 旋转角度
  float scale;       // 缩放比例
  float alpha;       // 透明度
  int priority;      // 优先级

  // 视频图层属性
  float playbackRate;           // 播放速率
  float volume;                  // 音量
  int audioTrack;               // 音轨
  std::string audioChannel;     // 音频声道
  bool subtitleVisible;         // 字幕是否可见
  std::string boundPlaylistId; // 绑定的播放列表ID（切换到该图层时自动播放）

  // 图像图层运行时路径（不保存到 config.json，不从 config.json 读取）
  // 仅用于非 Logo/QRCode 图层，由 ROOT_PATH + "Image/" + 文件名 拼出
  // Layer70 (Logo): 始终为空，Logo 路径由 logo/ 目录硬编码加载
  // Layer71 (QRCode): 始终为空，QRCode 路径由 QRCode/ 目录生成
  std::string imagePath;
  int filterMode;        // 滤镜模式
  float fadeInTime;     // 淡入时间
  float fadeOutTime;    // 淡出时间
  float displayDuration; // 显示时长
  bool animated;          // 是否动画
  bool photoWallMode;   // 照片墙模式
  int scaleMode;         // 显示比例模式 (0:Stretch, 1:Fit, 2:Fill, 3:Original)
  int shapeType;    // 几何形状 (0:Rect, 1:Circle, 2:Triangle, 3:RoundedRect)
  float shapeParam; // 形状参数 (如圆角半径)
  bool blackToTransparent; // 黑色变透明
  int invert;                // 图像反转 (0:无, 1:水平, 2:垂直, 3:两者)
  float gaussianBlur;       // 高斯模糊强度 (0-10)
  bool effectLinkedSlices; // 效果关联切片开关
  int audioEffectType;     // 音频联动特效类型 (0=无, 1=flash_white, 2=flash_black, 3=red, 4=green, 5=blue)
  std::vector<int> audioEffectIds; // 特效管理页完整效果列表，保留顺序
  uint32_t audioEffectStackPacked; // 多效果并行栈（bit31=1,count+最多3个ID）；0=单效果
  std::string audioEffectBlendMode; // sequential / parallel，供特效管理页持久化
  uint32_t audioEffectColor; // 描边/追逐光颜色 packed (mode<<24|B<<16|G<<8|R)；0=彩虹默认。未来 DMX512 RGBW 通道直写此字段
  float audioEffectWidth;   // 描边/追逐光宽度，占短边百分比（默认 2.5）
  int fitMode;              // 视频适配模式 (0=铺满显示, 1=保持视频比例显示)
  bool mirrorReadyHintVisible; // MIRROR 图层未连接时是否显示“投屏就绪”提示
  int tvVerticalCropPx;     // TV 模式上下各裁剪像素，裁剪后拉伸铺满

  // 文本图层运行时字体路径（不保存到 config.json，不从 config.json 读取）
  // 由 ROOT_PATH + "ttf/" + 文件名 拼出
  std::string text;
  std::string fontPath;
  float fontSize;        // 字体大小
  std::string textColor; // 文本颜色
  std::string bgColor;   // 背景颜色
  int alignment;          // 对齐方式
  int bindLayerId;      // 绑定的图层ID（时间源，主要用于图层21）
  int lyricDisplayMode; // 歌词显示模式（0=karaoke, 1=listening_classic, 2=listening_dream, 3=listening_neon）

  // 高级文本属性 (Layer 40)
  float scrollSpeed;
  float outlineWidth;
  std::string outlineColor;
  float shadow;

  // 播放列表提示属性 (Layer 41)
  std::string playlistId;    // 关联播放列表ID
  int showCount;             // 显示数量
  int displayAlign;          // 显示对齐: 0=靠左, 1=居中, 2=靠右
  float l41DisplayDuration; // Layer 41 显示时长 (s)，与图像层
                              // displayDuration 区分
  float startHintTime;      // 起始提示时间 (s)
  float endHintTime;        // 结束提示时间 (s)
  bool l41ShowList;        // Layer 41 是否显示播放列表（显示列表/隐藏列表）

  // 二维码图层属性
  std::string qrContent;    // 二维码内容
  int qrSize;               // 二维码尺寸
  std::string qrLogoPath;  // 运行时路径（不保存到 config.json）
  int qrLogoSize;          // Logo 尺寸（百分比，0-30）
  std::string qrText;       // 文字内容
  std::string qrTextColor; // 文字颜色（RGBA格式："R G B A"）
  std::string qrBgColor;   // 背景颜色（RGBA格式："R G B A"）
  std::string qrFgColor;   // 前景颜色（RGBA格式："R G B A"）
  int qrErrorCorrection;   // 容错级别（0=L, 1=M, 2=Q, 3=H）

  // 切片属性（key: "slice1", "slice2"等，value: 切片配置）
  std::map<std::string, SliceConfig> slices;

  // 漫游配置（JSON 字符串格式存储）
  std::string roamConfig; // 漫游配置 JSON 字符串

  // 采集图层属性（Layer 10/11）
  std::string captureType;   // "AUTO"、"HDMI"、"USB" 或 "MIPI"；空值按 AUTO 处理
  int captureRotation;       // 输入采集画面旋转角度（-1=自动，0/90/180/270），默认 0
  int captureWidth;          // 运行时采集请求宽度（不保存到 config.json）
  int captureHeight;         // 运行时采集请求高度（不保存到 config.json）
  int captureIndex;          // 采集设备索引（MIPI 索引或 HDMI 索引）
    LayerConfigData()
      : layerId(0), visible(true), position(0, 0), size(1920, 1080),
        rotation(0.0f), scale(1.0f), alpha(1.0f), priority(0),
        playbackRate(1.0f), volume(1.0f), audioTrack(0),
        audioChannel("stereo"), subtitleVisible(true), filterMode(0),
        fadeInTime(0.5f), fadeOutTime(0.5f), displayDuration(3.0f),
        animated(false), photoWallMode(false), scaleMode(0), shapeType(0),
        shapeParam(0.0f), blackToTransparent(false), invert(0),
        gaussianBlur(0.0f), effectLinkedSlices(false), audioEffectType(0),
        audioEffectIds(), audioEffectStackPacked(0), audioEffectBlendMode("sequential"),
        audioEffectColor(0), audioEffectWidth(2.5f), fitMode(0),
        mirrorReadyHintVisible(true), tvVerticalCropPx(0),
        fontSize(48.0f),
        textColor("1.0 1.0 1.0 1.0"), bgColor("0.0 0.0 0.0 0.0"),
        alignment(1), // 居中
        bindLayerId(1), lyricDisplayMode(0), scrollSpeed(0.0f), outlineWidth(2.0f),
        outlineColor("0 0 0 1.0"), shadow(0.0f), showCount(3),
        displayAlign(1), l41DisplayDuration(5.0f), startHintTime(10.0f),
        endHintTime(10.0f), l41ShowList(true), qrSize(256), qrLogoSize(0),
        qrTextColor("1.0 1.0 1.0 1.0"), qrBgColor("1.0 1.0 1.0 1.0"),
        qrFgColor("0.0 0.0 0.0 1.0"),         qrErrorCorrection(1), captureType(""),
        captureRotation(0), captureWidth(0), captureHeight(0), captureIndex(0) {
  } // 默认中等容错级别
};

/**
 * @brief 系统配置类
 *
 * 负责系统配置的加载、保存和管理
 */
class SystemConfig {
public:
  // 每个区域的颜色配置
  struct RegionParams {
    float luminance = 1.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
  };

  // 运行时融合带参数：由 fusionState_ 映射给旧渲染器使用，不写入 config.json。
  struct BlendParams {
    float blendLeft = 0.0f;
    float blendRight = 0.0f;
    float blendTop = 0.0f;
    float blendBottom = 0.0f;
    int blendGridRows = 2;
    int blendGridCols = 2;
    bool blendLeftEnabled = false;
    bool blendRightEnabled = false;
    bool blendTopEnabled = false;
    bool blendBottomEnabled = false;
    float edgeLeftGamma = 1.8f;
    float edgeLeftSlope = 1.0f;
    float edgeRightGamma = 1.8f;
    float edgeRightSlope = 1.0f;
    float edgeTopGamma = 1.8f;
    float edgeTopSlope = 1.0f;
    float edgeBottomGamma = 1.8f;
    float edgeBottomSlope = 1.0f;
    float stripStartL = 0.0f;
    float stripEndL = 255.0f;
    float stripStartR = 0.0f;
    float stripEndR = 255.0f;
    float stripStartT = 0.0f;
    float stripEndT = 255.0f;
    float stripStartB = 0.0f;
    float stripEndB = 255.0f;
    float edgeLeftAnchor = 0.5f;
    float edgeRightAnchor = 0.5f;
    float edgeTopAnchor = 0.5f;
    float edgeBottomAnchor = 0.5f;
    bool solidLeft = false;
    bool solidRight = false;
    bool solidTop = false;
    bool solidBottom = false;
    uint8_t brightL[3] = {128, 128, 128};
    uint8_t brightR[3] = {128, 128, 128};
    uint8_t brightT[3] = {128, 128, 128};
    uint8_t brightB[3] = {128, 128, 128};
  };
  SystemConfig();
  ~SystemConfig();

  /**
   * @brief 从文件加载配置
   * @param configPath 配置文件路径
   * @return 是否加载成功
   */
  bool load(const std::string &configPath);

  /**
   * @brief 保存配置到文件（含系统参数、矩阵、全部图层）
   * @param configPath 配置文件路径
   * @return 是否保存成功
   */
  bool save(const std::string &configPath);

  /**
   * @brief 仅保存矩阵与系统参数，不触碰图层
   * 用于 set_flexible_mapping 的矩阵布局一致性校验。
   * 读入 config.json，仅更新系统/矩阵相关字段后写回，图层键保持原样。
   * @param configPath 配置文件路径
   * @return 是否保存成功
   */
  bool saveMatrixOnly(const std::string &configPath);

  /**
   * @brief 获取分辨率
   * @return 分辨率
   */
  Resolution getResolution() const { return resolution_; }

  /**
   * @brief 设置分辨率
   * @param res 分辨率
   */
  void setResolution(const Resolution &res) { resolution_ = res; }

  /**
   * @brief 获取设备类型
   * @return 设备类型
   */
  int getDeviceType() const { return deviceType_; }

  /**
   * @brief 设置设备类型
   * @param type 设备类型
   */
  void setDeviceType(int type) { deviceType_ = type; }

  /**
   * @brief 获取屏幕旋转角度
   * @return 旋转角度（度）
   */
  int getScreenRotate() const { return screenRotate_; }

  /**
   * @brief 设置屏幕旋转角度
   * @param angle 旋转角度（度）
   */
  void setScreenRotate(int angle) { screenRotate_ = angle; }

  /**
   * @brief 获取系统音量
   * @return 系统音量（0.0-1.0，最多 2 位小数，如 0.73）
   */
  float getSystemVolume() const {
    float v = std::max(0.f, std::min(1.f, systemVolume_));
    return std::round(v * 100.f) / 100.f;
  }

  /**
   * @brief 设置系统音量
   * @param volume 系统音量（0.0-1.0），写入时自动限制并舍入为最多 2 位小数
   */
  void setSystemVolume(float volume) {
    float v = std::max(0.f, std::min(1.f, volume));
    systemVolume_ = std::round(v * 100.f) / 100.f;
  }

  /** 多图层同时播放时指定输出音频的图层 ID；0 表示自动（按优先级最高的播放图层） */
  int getAudioOutputLayerId() const { return audioOutputLayerId_; }
  void setAudioOutputLayerId(int id) { audioOutputLayerId_ = id; }

  // 区域配置参数 getter/setter
  int getRegionCount() const { return regionCount_; }
  void setRegionCount(int count) { regionCount_ = count; }
  int getRegionLayoutCols() const { return regionLayoutCols_; }
  void setRegionLayoutCols(int cols) { regionLayoutCols_ = cols; }
  int getRegionLayoutRows() const { return regionLayoutRows_; }
  void setRegionLayoutRows(int rows) { regionLayoutRows_ = rows; }
  int getRegionWidth() const { return regionWidth_; }
  void setRegionWidth(int width) { regionWidth_ = width; }
  int getRegionHeight() const { return regionHeight_; }
  void setRegionHeight(int height) { regionHeight_ = height; }
  int getInputWidth() const { return inputWidth_; }
  void setInputWidth(int width) { inputWidth_ = width; }
  int getInputHeight() const { return inputHeight_; }
  void setInputHeight(int height) { inputHeight_ = height; }
  int getInputLayoutRows() const { return inputLayoutRows_; }
  void setInputLayoutRows(int rows) { inputLayoutRows_ = rows; }
  int getInputLayoutCols() const { return inputLayoutCols_; }
  void setInputLayoutCols(int cols) { inputLayoutCols_ = cols; }
  int getInputGridRows() const { return inputLayoutRows_; }
  int getInputGridCols() const { return inputLayoutCols_; }
  int getSplitDirection() const { return splitDirection_; }
  void setSplitDirection(int direction) { splitDirection_ = direction; }
  int getOutputWidth() const { return outputWidth_; }
  void setOutputWidth(int width) { outputWidth_ = width; }
  int getOutputHeight() const { return outputHeight_; }
  void setOutputHeight(int height) { outputHeight_ = height; }
  int getOutputLayoutCols() const { return outputLayoutCols_; }
  void setOutputLayoutCols(int cols) { outputLayoutCols_ = cols; }
  int getOutputLayoutRows() const { return outputLayoutRows_; }
  void setOutputLayoutRows(int rows) { outputLayoutRows_ = rows; }
  int getOutputGridRows() const { return outputLayoutRows_; }
  int getOutputGridCols() const { return outputLayoutCols_; }
  float getRotationAngle() const { return rotationAngle_; }
  void setRotationAngle(float angle) { rotationAngle_ = angle; }

  // 区域颜色参数接口
  void setRegionParams(int id, float luminance, float contrast,
                       float saturation);
  void setRegionParams(int id, const RegionParams& params);
  bool getRegionParams(int id, float &luminance, float &contrast,
                       float &saturation) const;
  const RegionParams* getRegionParamsPtr(int id) const;
  bool getRegionParams(int id, float &luminance, float &contrast,
                       float &saturation, int &rows, int &cols,
                       std::vector<float> &points,
                       int &interpolationMode, bool &showGrid) const;
  bool getGlobalMaskParams(bool &enabled, int &rows, int &cols,
                           std::vector<float> &vertices,
                           int &interpolationMode) const;
  const BlendParams* getProjectionBlendParamsFull(int regionId) const;
  bool getRegionGeometryCorrection(int regionId, bool &enabled,
                                   float &offsetX, float &offsetY,
                                   float &scaleX, float &scaleY,
                                   float &rotateRad, float &keystoneX,
                                   float &keystoneY) const;
  bool getCaveWallConfig(int regionId, bool &enabled, int &wallType,
                         float &llx, float &lly, float &llz,
                         float &ulx, float &uly, float &ulz,
                         float &lrx, float &lry, float &lrz,
                         float &nearPlane, float &farPlane,
                         float &eyeDistance) const;
  bool getFusionMasterEnabled() const { return fusionState_.masterEnabled; }
  bool getManagerMode() const { return fusionState_.managerMode; }
  bool getFusionMerge360Enabled() const { return fusionState_.merge360; }
  int getActiveRegionId() const { return fusionState_.activeRegionId; }

  /**
   * @brief 检查歌词功能是否启用（全局）
   * @return 是否启用
   */
  bool isLyricEnabled() const { return lyricEnabled_; }

  /**
   * @brief 设置歌词功能开关（全局）
   * @param enabled 是否启用
   */
  void setLyricEnabled(bool enabled) { lyricEnabled_ = enabled; }

  /**
   * @brief 获取 DMX 波特率
   */
  int getDmxBaudRate() const { return dmxBaudRate_; }

  /**
   * @brief 设置 DMX 波特率
   */
  void setDmxBaudRate(int baudrate) { dmxBaudRate_ = baudrate; }

  int getDmxStartAddress() const { return dmxStartAddress_; }
  void setDmxStartAddress(int address) { dmxStartAddress_ = address; }

  /** 音频联动总开关：勾选后才会按各图层 audioEffectType 跟随鼓点触发 */
  bool isAudioReactiveEnabled() const { return audioReactiveEnabled_; }
  void setAudioReactiveEnabled(bool enabled) { audioReactiveEnabled_ = enabled; }

  /** 点歌(VOD)模式开关：启用后目标 VOD 播放图层由点歌占用，手动播放列表不得占用该图层 */
  bool isVodEnabled() const { return enableVod_; }
  void setVodEnabled(bool enabled) { enableVod_ = enabled; }

  /** VOD模式配置：0=关闭, 1=单机(LocalVod), 2=网络(OnlineVod) */
  int getVodMode() const { return vodMode_; }
  void setVodMode(int mode) {
    vodMode_ = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    enableVod_ = vodMode_ > 0;
  }

  /** 便捷方法：检查是否启用了任何VOD功能 */
  bool isAnyVodEnabled() const { return vodMode_ > 0; }

  /** 便捷方法：检查是否启用了本地VOD */
  bool isLocalVodEnabled() const { return vodMode_ == 1; }

  /** 便捷方法：检查是否启用了网络VOD */
  bool isNetworkVodEnabled() const { return vodMode_ == 2; }

  /** 授权服务器URL配置 */
  std::string getLicenseServerUrl() const { return licenseServerUrl_; }
  void setLicenseServerUrl(const std::string& url) { licenseServerUrl_ = url; }

  /** OnlineVod 点播服务器配置（enableVod=1 时用于自动同步 state/queue） */
  std::string getOnlineVodHost() const { return onlineVodHost_; }
  int getOnlineVodPort() const { return 9898; }
  std::string getOnlineVodRoomId() const { return onlineVodRoomId_; }
  void setOnlineVodHost(const std::string& host) { onlineVodHost_ = host; }
  void setOnlineVodPort(int port) { (void)port; }
  void setOnlineVodRoomId(const std::string& roomId) { onlineVodRoomId_ = roomId; }

  int getVodLayerId() const { return 1; }

  bool isLocalSongFileScanEnabled() const { return localSongFileScanEnabled_; }
  void setLocalSongFileScanEnabled(bool enabled) { localSongFileScanEnabled_ = enabled; }

  /** App 自动更新开关：关闭后启动后不再触发 checkForUpdate */
  bool isAppUpdateEnabled() const { return appUpdateEnabled_; }
  void setAppUpdateEnabled(bool enabled) { appUpdateEnabled_ = enabled; }

  /** 最终合成帧率策略：auto=按内容自适应，fixed30=固定 30fps */
  std::string getRenderFrameRateMode() const { return renderFrameRateMode_; }
  void setRenderFrameRateMode(const std::string& mode) {
    renderFrameRateMode_ = (mode == "fixed30") ? "fixed30" : "auto";
  }
  bool isRenderFrameRateFixed30() const { return renderFrameRateMode_ == "fixed30"; }

  /** Engine Surface 渲染质量：smooth=1920x1080, normal=2560x1440, high=2880x1620, ultra=3840x2160 */
  std::string getRenderQuality() const { return renderQuality_; }
  void setRenderQuality(const std::string& quality) {
    if (quality == "smooth" || quality == "high" || quality == "ultra") {
      renderQuality_ = quality;
    } else {
      renderQuality_ = "normal";
    }
  }

  std::string getNetworkIpMode() const { return networkIpMode_; }
  void setNetworkIpMode(const std::string& mode) { networkIpMode_ = mode == "static" ? "static" : "dynamic"; }
  std::string getNetworkStaticIp() const { return networkStaticIp_; }
  void setNetworkStaticIp(const std::string& ip) { networkStaticIp_ = ip; }
  std::string getNetworkGateway() const { return networkGateway_; }
  void setNetworkGateway(const std::string& gateway) { networkGateway_ = gateway; }
  std::string getNetworkDns() const { return networkDns_; }
  void setNetworkDns(const std::string& dns) { networkDns_ = dns; }
  bool isDebugHotspotEnabled() const { return debugHotspotEnabled_; }
  void setDebugHotspotEnabled(bool enabled) { debugHotspotEnabled_ = enabled; }

  bool isPowerScheduleEnabled() const { return powerScheduleEnabled_; }
  void setPowerScheduleEnabled(bool enabled) { powerScheduleEnabled_ = enabled; }
  bool isPowerOnScheduleEnabled() const { return powerOnScheduleEnabled_; }
  void setPowerOnScheduleEnabled(bool enabled) { powerOnScheduleEnabled_ = enabled; }
  std::string getPowerOnDate() const { return powerOnDate_; }
  void setPowerOnDate(const std::string& date) { powerOnDate_ = date; }
  std::string getPowerOnTime() const { return powerOnTime_; }
  void setPowerOnTime(const std::string& time) { powerOnTime_ = time; }
  bool isPowerOffScheduleEnabled() const { return powerOffScheduleEnabled_; }
  void setPowerOffScheduleEnabled(bool enabled) { powerOffScheduleEnabled_ = enabled; }
  std::string getPowerOffDate() const { return powerOffDate_; }
  void setPowerOffDate(const std::string& date) { powerOffDate_ = date; }
  std::string getPowerOffTime() const { return powerOffTime_; }
  void setPowerOffTime(const std::string& time) { powerOffTime_ = time; }

  // ===== MPG (MPEG-PS) 解码模式 =====
  // 兼容旧配置字段：播放解码策略已固定为 RKMPP-only。
  bool isMpegPsHardwareDecode() const { return mpegPsHardwareDecode_; }
  void setMpegPsHardwareDecode(bool enabled) { mpegPsHardwareDecode_ = enabled; }

  // ===== 音频唇同步偏移 (毫秒) =====
  // 用于补偿 AAudio HAL 输出延迟。RK3588 SHARED 模式 getTimestamp 失败时
  // 通过 SCR-PTS gap + 此偏移估计 HAL 延迟。
  // 正值 = "嘴动了声音才出来" 的修正方向（视频提前显示与音频对齐）
  // 负值 = "声音先嘴后" 的修正方向
  // 默认 0；典型范围 -200 ~ +200 ms
  int getAudioLipSyncOffsetMs() const { return audioLipSyncOffsetMs_; }
  void setAudioLipSyncOffsetMs(int ms) { audioLipSyncOffsetMs_ = ms; }

  /**
   * @brief 输入输出映射配置结构
   */
  struct FlexibleMapping {
    bool enabled = true;
    int inputRegionId = 0;  // 输入区域ID (1-based)
    int outputIndex = 0;      // 输出索引 (0-based)
  };

  /**
   * @brief 获取输入输出映射配置
   * @return 映射配置列表
   */
  const std::vector<FlexibleMapping> &getFlexibleMappings() const {
    return flexibleMappings_;
  }

  /**
   * @brief 设置输入输出映射配置
   * @param mappings 映射配置列表
   */
  void setFlexibleMappings(const std::vector<FlexibleMapping> &mappings) {
    flexibleMappings_ = mappings;
  }

  const fusion::FusionProjectState &getFusionState() const {
    return fusionState_;
  }
  fusion::FusionProjectState &getMutableFusionState() {
    return fusionState_;
  }
  void setFusionState(const fusion::FusionProjectState &state) {
    fusionState_ = state;
  }

  /**
   * @brief 获取图层配置
   * @param layerId 图层ID
   * @return 图层配置指针，不存在返回nullptr
   */
  const LayerConfigData *getLayerConfig(int layerId) const;

  /**
   * @brief 获取可修改的图层配置（用于直接修改配置）
   * @param layerId 图层ID
   * @return 图层配置指针，不存在返回nullptr
   */
  LayerConfigData *getMutableLayerConfig(int layerId);

  /**
   * @brief 设置图层配置
   * @param layerId 图层ID
   * @param config 图层配置
   */
  void setLayerConfig(int layerId, const LayerConfigData &config);

  /**
   * @brief 获取所有图层配置
   * @return 图层配置映射表
   */
  const std::unordered_map<int, LayerConfigData> &getAllLayerConfigs() const {
    return layerConfigs_;
  }

  /**
   * @brief 检查图层配置是否存在
   * @param layerId 图层ID
   * @return 是否存在
   */
  bool hasLayerConfig(int layerId) const;

  /**
   * @brief 删除图层配置
   * @param layerId 图层ID
   */
  void removeLayerConfig(int layerId);

  /**
   * @brief 清除所有图层配置
   * 用于切换场景时重置图层配置
   */
  void clearAllLayerConfigs() { layerConfigs_.clear(); }

  /**
   * @brief 解析字符串到Position的辅助方法（公共静态方法）
   * @param str 字符串（格式："x y"）
   * @return Position对象
   */
  static Position parsePosition(const std::string &str);

  /**
   * @brief 解析字符串到Size的辅助方法（公共静态方法）
   * @param str 字符串（格式："宽度 高度"）
   * @return Size对象
   */
  static Size parseSize(const std::string &str);

  /**
   * @brief Position转字符串的辅助方法
   * @param pos Position对象
   * @return 字符串（格式："x y"）
   */
  static std::string positionToString(const Position &pos);

  /**
   * @brief Size转字符串的辅助方法
   * @param size Size对象
   * @return 字符串（格式："宽度 高度"）
   */
  static std::string sizeToString(const Size &size);

  /**
   * @brief 仅保留 config.json 合法字段（矩阵/系统/图层），存储时按白名单检查字段合法性。
   */
  static void retainOnlyConfigJsonKeys(Json::Value &root);

private:
  Resolution resolution_;        // 分辨率
  int deviceType_;               // 设备类型
  int screenRotate_;             // 屏幕旋转角度
  float systemVolume_;           // 系统音量（0.0-1.0）
  int audioOutputLayerId_;       // 多图层同时播放时输出音频的图层 ID，0=自动按优先级
  bool lyricEnabled_;            // 歌词功能全局开关（路径已移至 PathConfig.h）
  int dmxBaudRate_;              // DMX 波特率
  int dmxStartAddress_ = 1;      // DMX 起始地址
  bool audioReactiveEnabled_ = false; // 音频联动总开关（前端"启用联动"复选框）
  bool enableVod_ = false;         // enableVod：兼容旧配置/API；实际模式由 vodMode_ 决定
  int vodMode_ = 0;                // vodMode：VOD模式 (0=关闭, 1=单机LocalVod, 2=在线VOD)
  std::string licenseServerUrl_; // licenseServerUrl：授权服务器URL
  std::string onlineVodHost_;          // OnlineVodHost：OnlineVod 点播服务器 host/IP（不含协议）
  std::string onlineVodRoomId_ = "current"; // OnlineVodRoomId：房间ID（默认 current）
  bool localSongFileScanEnabled_ = false; // localSongFileScanEnabled：启动时扫描歌曲目录并对照 songNo 更新路径
  bool appUpdateEnabled_ = true;          // appUpdateEnabled：App 自动更新检查总开关，默认开启
  std::string renderFrameRateMode_ = "auto"; // renderFrameRateMode：最终合成帧率策略 auto/fixed30
  std::string renderQuality_ = "normal";      // renderQuality：Engine Surface 渲染质量 smooth/normal/high/ultra
  std::string networkIpMode_ = "dynamic";
  std::string networkStaticIp_;
  std::string networkGateway_;
  std::string networkDns_;
  bool debugHotspotEnabled_ = false;
  bool powerScheduleEnabled_ = false;
  bool powerOnScheduleEnabled_ = false;
  std::string powerOnDate_;
  std::string powerOnTime_;
  bool powerOffScheduleEnabled_ = false;
  std::string powerOffDate_;
  std::string powerOffTime_;
  bool mpegPsHardwareDecode_ = false;     // mpegPsHardwareDecode：兼容旧配置；播放解码固定 RKMPP-only
  int audioLipSyncOffsetMs_ = 0;          // audioLipSyncOffsetMs：HAL 延迟微调 (ms)，正值修正"嘴动声音晚"
  std::vector<FlexibleMapping> flexibleMappings_; // 输入输出映射配置
  fusion::FusionProjectState fusionState_;

  // 区域配置参数
  int regionCount_;     // 区域数量
  int regionLayoutCols_;  // 区域布局列数
  int regionLayoutRows_;  // 区域布局行数
  int regionWidth_;     // 每个区域的宽度
  int regionHeight_;    // 每个区域的高度
  int inputWidth_;      // 输入总宽度（像素）
  int inputHeight_;     // 输入总高度（像素）
  int inputLayoutRows_;   // 输入行数
  int inputLayoutCols_;   // 输入列数
  int splitDirection_;  // 分割方向（0=水平，1=垂直）
  // 系统硬件最终输出范围（像素）。渲染按此范围 1:1 输出，不铺满窗口。
  int outputWidth_;
  int outputHeight_;
  int outputLayoutCols_;  // 输出布局列数（显示约定为 rows×cols；12×1 表示 12 行 1 列）
  int outputLayoutRows_;  // 输出布局行数（显示约定为 rows×cols；rows 永远在前）
  float rotationAngle_; // 旋转角度（度）
  std::map<int, RegionParams> regionParams_;
  mutable std::map<int, BlendParams> runtimeBlendParams_;

  // 图层配置存储（key: layerId）
  std::unordered_map<int, LayerConfigData> layerConfigs_;

  /**
   * @brief 将系统参数与矩阵配置写入 JSON 根（不修改图层键）
   * @param jsonRoot 指向 Json::Value 的指针
   */
  void applySystemAndMatrixToRoot(void *jsonRoot) const;

  /**
   * @brief 保存图层配置到JSON的辅助方法
   * @param jsonRoot JSON根对象指针
   * @param layerId 图层ID
   * @param config 图层配置
   */
  void saveLayerConfig(void *jsonRoot, int layerId,
                       const LayerConfigData &config,
                       const Json::Value *originalConfig = nullptr,
                       bool forceSaveAll = false) const;

public:
  /**
   * @brief 从JSON解析图层配置的辅助方法（供外部批量解析使用）
   * @param layerKey 图层键名
   * @param jsonValue JSON值指针
   * @return 是否解析成功
   */
  bool parseLayerConfig(const std::string &layerKey, const void *jsonValue);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_SYSTEM_CONFIG_H
