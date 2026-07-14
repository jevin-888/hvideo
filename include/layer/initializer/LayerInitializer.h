/**
 * @file LayerInitializer.h（文件名）
 * @brief 图层初始化器基础接口
 *
 * 本模块采用策略模式，将不同类型图层的初始化逻辑解耦到独立的初始化器中。
 * 每个初始化器负责一种图层类型或特殊图层的配置加载和设置。
 *
 * 设计原则：
 * - 单一职责：每个初始化器只负责一种图层的初始化
 * - 开闭原则：添加新图层类型只需新增初始化器，无需修改现有代码
 * - 可测试性：每个初始化器可以独立单元测试
 */

#ifndef HSVJ_LAYER_INITIALIZER_H
#define HSVJ_LAYER_INITIALIZER_H

#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "renderer/VulkanRenderer.h"
#include <memory>
#include <string>
#include <set>
#include <utility>

namespace hsvj {

// 前向声明
class Mubu;
class SystemConfig;

/**
 * @brief 图层初始化上下文
 *
 * 包含图层初始化所需的所有依赖和环境信息。
 * 通过上下文对象传递依赖，避免初始化器与Engine强耦合。
 */
struct LayerInitContext {
    Mubu* mubu = nullptr;                           // 图层管理器
    SystemConfig* systemConfig = nullptr;           // 系统配置
    VulkanRenderer* renderer = nullptr;             // 渲染器
    std::string rootPath;                           // 根路径（/huoshan/）
    std::string fontDir;                            // 字体目录
    std::string lyricsDir;                          // 歌词目录

    LayerInitContext() = default;

    LayerInitContext(Mubu* m, SystemConfig* sc, VulkanRenderer* r,
                     const std::string& root, const std::string& font,
                     const std::string& lyrics)
        : mubu(m), systemConfig(sc), renderer(r),
          rootPath(root), fontDir(font), lyricsDir(lyrics) {}
};

/**
 * @brief 图层初始化结果
 */
struct LayerInitResult {
    bool success = false;                  // 是否初始化成功
    std::string errorMessage;              // 错误信息（失败时）
    std::string warningMessage;            // 警告信息（成功但有问题时）

    static LayerInitResult Success() {
        return LayerInitResult{true, "", ""};
    }

    static LayerInitResult Warning(const std::string& warning) {
        return LayerInitResult{true, "", warning};
    }

    static LayerInitResult Failure(const std::string& error) {
        return LayerInitResult{false, error, ""};
    }
};

/**
 * @brief 图层初始化器抽象基类
 *
 * 定义了图层初始化的标准流程：
 * 1. canHandle()  - 判断是否能处理该图层
 * 2. initialize() - 执行初始化
 * 3. validate()   - 验证初始化结果
 */
class LayerInitializer {
public:
    virtual ~LayerInitializer() = default;

    /**
     * @brief 判断是否能处理指定图层
     * @param layerId 图层ID
     * @param layerType 图层类型
     * @return 能处理返回true
     */
    virtual bool canHandle(int layerId, LayerType layerType) const = 0;

    /**
     * @brief 执行图层初始化
     * @param layer 要初始化的图层
     * @param config 图层配置数据
     * @param context 初始化上下文
     * @return 初始化结果
     */
    virtual LayerInitResult initialize(Layer* layer,
                                       const LayerConfigData& config,
                                       const LayerInitContext& context) = 0;

    /**
     * @brief 获取初始化器名称（用于日志）
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * @brief 应用通用图层属性
     *
     * 所有图层共有的属性：位置、尺寸、旋转、缩放、透明度等
     */
    LayerInitResult applyCommonProperties(Layer* layer,
                                          const LayerConfigData& config);

    /**
     * @brief 规范化文件路径
     */
    std::string normalizeFilePath(const std::string& path,
                                   const std::string& rootPath) const;

    /**
     * @brief 检查文件是否存在，不存在时记录警告（避免重复日志）
     */
    bool checkFileExists(const std::string& path, int layerId,
                        const std::string& fileType);

private:
    // 用于去重的日志记录
    static std::set<std::pair<int, std::string>> s_fileNotFoundLogged_;
    static std::set<std::pair<int, std::string>> s_fileLoadFailLogged_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_INITIALIZER_H
