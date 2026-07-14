/**
 * @file Layer.h（文件名）
 * @brief 图层基类定义
 *
 * 本文件定义了图层系统的基类和相关数据结构，包括：
 * - Layer基类：所有图层类型的基类
 * - LayerType枚举：图层类型定义
 * - Position结构：位置信息
 * - Size结构：尺寸信息
 */

#ifndef HSVJ_LAYER_H
#define HSVJ_LAYER_H

#include "core/Resolution.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <json/json.h>
#include <map>
#include <mutex>
#include <string>

namespace hsvj {

/**
 * @brief 图层类型枚举
 */
enum class LayerType {
  VIDEO = 1,  ///< 视频图层
  IMAGE = 2,  ///< 图片图层
  TEXT = 3,   ///< 文本图层
  QRCODE = 4, ///< 二维码图层
  EFFECT = 5, ///< 特效图层
  MIRROR = 6  ///< 投屏图层
};

/**
 * @brief 位置结构体
 */
struct Position {
  int x; ///< X坐标
    int y; ///< Y坐标

  Position() : x(0), y(0) {}
  Position(int x, int y) : x(x), y(y) {}
};

/**
 * @brief 尺寸结构体
 */
struct Size {
  int width;  ///< 宽度
    int height; ///< 高度

  Size() : width(1920), height(1080) {}
  Size(int w, int h) : width(w), height(h) {}
};

/**
 * @brief 图层基类
 *
 * 所有图层类型的基类，提供通用的图层属性和方法
 */
class Layer {
public:
  /**
   * @brief 构造函数
   * @param layerId 图层ID
   */
  Layer(int layerId);

  /**
   * @brief 析构函数
   */
  virtual ~Layer();

  /**
   * @brief 初始化图层
   * @return 是否初始化成功
   */
  virtual bool initialize() = 0;

  /**
   * @brief 关闭图层，释放资源
   */
  virtual void shutdown() = 0;

  /**
   * @brief 更新图层（每帧调用）
   * @param deltaTime 帧间隔时间（秒）
   */
  virtual void update(float deltaTime) = 0;

  /**
   * @brief 渲染图层
   */
  virtual void render() = 0;

  /**
   * @brief 检查是否需要更新纹理（在 render pass 前调用）
   */
  virtual bool needsTextureUpdate() const { return false; }

  /**
   * @brief 更新图层纹理（在 render pass 前调用）
   */
  virtual void updateTexture() {}

  /**
   * @brief 获取图层ID
   * @return 图层ID
   */
  int getLayerId() const { return layerId_; }

  /**
   * @brief 获取图层类型
   * @return 图层类型
   */
  LayerType getType() const { return type_; }

  /**
   * @brief 检查图层是否可见
   * @return 是否可见
   */
  bool isVisible() const { return visible_; }

  /**
   * @brief 设置图层可见性
   * @param visible 是否可见
   */
  void setVisible(bool visible) { visible_ = visible; }

  /**
   * @brief 获取图层位置
   * @return 位置
   */
  Position getPosition() const { return position_; }

  /**
   * @brief 设置图层位置
   * @param pos 位置
   */
  void setPosition(const Position &pos);

  /**
   * @brief 获取图层尺寸
   * @return 尺寸
   */
  Size getSize() const { return size_; }

  /**
   * @brief 设置图层尺寸
   * @param size 尺寸
   */
  virtual void setSize(const Size &size) { size_ = size; }

  /**
   * @brief 获取旋转角度
   * @return 旋转角度（度）
   */
  float getRotation() const { return rotation_; }

  /**
   * @brief 设置旋转角度
   * @param rotation 旋转角度（度）
   */
  void setRotation(float rotation) { rotation_ = rotation; }

  /**
   * @brief 获取缩放比例
   * @return 缩放比例
   */
  float getScale() const { return scale_; }

  /**
   * @brief 设置缩放比例
   * @param scale 缩放比例
   */
  void setScale(float scale) { scale_ = scale; }

  /**
   * @brief 获取透明度
   * @return 透明度（0.0-1.0）
   */
  float getAlpha() const { return alpha_; }

  /**
   * @brief 设置透明度
   * @param alpha 透明度（0.0-1.0）
   */
  void setAlpha(float alpha) { alpha_ = alpha; }

  /**
   * @brief 获取优先级
   * @return 优先级
   */
  int getPriority() const { return priority_; }

  /**
   * @brief 设置优先级
   * @param priority 优先级
   */
  void setPriority(int priority) { priority_ = priority; }

  void setShapeType(int type) { shapeType_ = type; }
  int getShapeType() const { return shapeType_; }
  void setShapeParam(float param) { shapeParam_ = param; }
  float getShapeParam() const { return shapeParam_; }

  /**
   * @brief 设置是否将黑色变为透明
   * @param enable 是否启用黑色变透明
   */
  void setBlackToTransparent(bool enable) { blackToTransparent_ = enable; }

  /**
   * @brief 获取是否启用黑色变透明
   * @return 是否启用
   */
  bool getBlackToTransparent() const { return blackToTransparent_; }

  /**
   * @brief 设置图像反转模式
   * @param mode 反转模式 (0=无, 1=水平, 2=垂直, 3=水平+垂直)
   */
  void setInvert(int mode);

  /**
   * @brief 获取图像反转模式
   * @return 反转模式 (0=无, 1=水平, 2=垂直, 3=水平+垂直)
   */
  int getInvert() const { return invert_; }

  /**
   * @brief 设置效果关联切片模式
   * @param enable 是否启用
   */
  void setEffectLinkedSlices(bool enable) { effectLinkedSlices_ = enable; }

  /**
   * @brief 获取效果关联切片模式
   * @return 是否启用
   */
  bool getEffectLinkedSlices() const { return effectLinkedSlices_; }

  /**
   * @brief 设置高斯模糊强度
   * @param blur 模糊强度 [0.0, 10.0]
   */
  void setGaussianBlur(float blur) { gaussianBlur_ = blur; }

  /**
   * @brief 获取高斯模糊强度
   * @return 模糊强度
   */
  float getGaussianBlur() const { return gaussianBlur_; }

  void setFitMode(int mode) { fitMode_ = mode > 0 ? 1 : 0; }
  int getFitMode() const { return fitMode_; }

  /**
   * @brief 设置渲染器
   * @param 渲染器 渲染器指针
   */
  virtual void setRenderer(class VulkanRenderer *renderer) {
    renderer_ = renderer;
  }

  /**
   * @brief 获取渲染器
   * @return 渲染器指针
   */
  class VulkanRenderer *getRenderer() const { return renderer_; }

  /**
   * @brief 设置静默模式（不输出初始化日志）
   * @param silent 是否静默
   */
  void setSilent(bool silent) { silent_ = silent; }

  /**
   * @brief 获取静默模式
   * @return 是否静默
   */
  bool isSilent() const { return silent_; }

  /**
   * @brief 设置特效
   * @param effectNo 特效编号
   * @param effectParams 特效参数（可选）
   */
  void setEffect(int effectNo,
                 const Json::Value &effectParams = Json::nullValue);

  /**
   * @brief 获取特效编号
   * @return 特效编号
   */
  int getEffect() const { return effectNo_.load(std::memory_order_acquire); }

  /**
   * @brief 设置混合模式
   * @param blendMode 混合模式
   */
  void setBlendMode(int blendMode) { blendMode_ = blendMode; }

  /**
   * @brief 获取混合模式
   * @return 混合模式
   */
  int getBlendMode() const { return blendMode_; }

  /**
   * @brief 设置切片配置
   * @param sliceKey 切片键（如"slice1"、"slice2"等）
   * @param sliceConfig 切片配置JSON
   */
  void setSlice(const std::string &sliceKey, const Json::Value &sliceConfig);

  /**
   * @brief 获取切片配置
   * @param sliceKey 切片键
   * @return 切片配置JSON，如果不存在则返回nullValue
   */
  Json::Value getSlice(const std::string &sliceKey) const;

  /**
   * @brief 删除切片配置
   * @param sliceKey 切片键
   * @return 是否删除成功
   */
  bool removeSlice(const std::string &sliceKey);

  /**
   * @brief 获取所有切片配置
   * @return 包含所有切片的JSON对象
   */
  Json::Value getAllSlices() const;

  /**
   * @brief 获取所有切片配置快照
   * @param out 输出切片JSON对象；无切片时写入空对象
   * @return 存在至少一个切片配置时返回true
   */
  bool getAllSlices(Json::Value &out) const;

  /**
   * @brief 遍历切片配置；回调在内部锁保护下执行，不应反调 Layer 切片修改接口。
   * @return 至少存在一个切片配置时返回 true
   */
  bool forEachSlice(const std::function<void(const std::string &, const Json::Value &)> &visitor) const;

  /**
   * @brief 是否存在切片配置
   * @return 至少存在一个切片配置时返回 true
   */
  bool hasSlices() const;

  /**
   * @brief 获取切片配置版本号
   * @return 每次切片增删改都会递增，用于渲染缓存失效判定
   */
  uint64_t getSliceRevision() const {
    return sliceRevision_.load(std::memory_order_acquire);
  }

  /**
   * @brief 清除所有切片配置
   * 用于场景切换时重置图层切片状态
   */
  void clearAllSlices();

  /**
   * @brief 更新漫游配置（从JSON配置加载）
   * @param roamConfigJson 漫游配置JSON对象
   * 
   * 此方法从JSON配置中解析漫游参数并应用到图层
   */
  void updateRoamConfig(const Json::Value &roamConfigJson);

  /**
   * @brief 获取漫游配置
   * @return 漫游配置JSON对象
   * 
   * 此方法返回当前图层的漫游配置
   */
  Json::Value getRoamConfig() const;

  /**
   * @brief 当前图层是否有逐帧漫游动画需求
   */
  bool hasActiveRoam() const;

  /**
   * @brief 更新漫游动画（在update方法中调用）
   * @param deltaTime 帧间隔时间（秒）
   * 
   * 此方法根据漫游配置更新图层位置
   */
  void updateRoam(float deltaTime);

protected:
  int layerId_;
  LayerType type_;
  bool visible_;
  Position position_;
  Size size_;
  float rotation_;
  float scale_;
  float alpha_;
  int priority_;
  int shapeType_;
  float shapeParam_;
  bool blackToTransparent_ = false; // 是否将黑色变为透明（默认关闭）
    int invert_ = 0; // 图像反转模式 (0=无, 1=水平, 2=垂直, 3=水平+垂直)
    bool effectLinkedSlices_ = false; // 效果关联切片模式
    float gaussianBlur_ = 0.0f;       // 高斯模糊强度
    int fitMode_ = 0;                 // 视频适配模式 (0=铺满显示, 1=保持视频比例显示)
    bool silent_ = false;             // 静默模式（不输出初始化日志）
  class VulkanRenderer *renderer_;

  // 特效和混合模式（effectNo_ 原子化，供 DMX 线程写入、渲染线程读取）
    std::atomic<int> effectNo_{0};
  Json::Value effectParams_;
  int blendMode_;

  // 切片数据（key: "slice1", "slice2"等，value: 切片配置JSON）
    std::map<std::string, Json::Value> slices_;
  mutable std::mutex slicesMutex_; // 保护 slices_ 的并发访问
  std::atomic<uint64_t> sliceRevision_{0};

  // 漫游相关成员变量
    bool roamEnabled_ = false;        // 是否启用漫游
    int roamMode_ = 0;                // 漫游模式 (0=关闭, 1=左右, 2=上下, 3=圆形, 4=自定义)
    float roamSpeed_ = 100.0f;        // 移动速度(像素/秒)
    bool roamLoop_ = true;            // 是否循环
    int roamRangeX_ = 500;            // 左右移动范围(像素)
    int roamRangeY_ = 500;            // 上下移动范围(像素)
    float roamRadius_ = 200.0f;       // 圆形半径(像素)
  Position roamBasePosition_;       // 漫游基准位置（初始位置）
    float roamTime_ = 0.0f;          // 漫游时间累计
    bool roamBasePositionSet_ = false; // 是否已设置基准位置
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_H
