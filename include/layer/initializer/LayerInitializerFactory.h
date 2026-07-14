/**
 * @file LayerInitializerFactory.h（文件名）
 * @brief 图层初始化器工厂
 *
 * 负责创建和管理所有图层初始化器实例。
 * 采用责任链模式，遍历所有初始化器直到找到能处理该图层的初始化器。
 */

#ifndef HSVJ_LAYER_INITIALIZER_FACTORY_H
#define HSVJ_LAYER_INITIALIZER_FACTORY_H

#include "layer/initializer/LayerInitializer.h"
#include <memory>
#include <vector>

namespace hsvj {

/**
 * @brief 图层初始化器工厂
 *
 * 设计模式：
 * - 工厂模式：创建初始化器实例
 * - 责任链模式：遍历初始化器链找到合适的处理器
 * - 单例模式：全局唯一的工厂实例
 */
class LayerInitializerFactory {
public:
    /**
     * @brief 获取工厂单例
     */
    static LayerInitializerFactory& getInstance();

    /**
     * @brief 初始化所有图层
     *
     * 这是Engine::createLayersFromConfig()的替代方法。
     * 遍历配置中的所有图层，为每个图层找到合适的初始化器并执行。
     *
     * @param context 初始化上下文
     * @return 成功初始化的图层数量
     */
    int initializeAllLayers(const LayerInitContext& context);

    /**
     * @brief 初始化单个图层
     *
     * @param layer 要初始化的图层
     * @param config 图层配置
     * @param context 初始化上下文
     * @return 初始化结果
     */
    LayerInitResult initializeLayer(Layer* layer,
                                    const LayerConfigData& config,
                                    const LayerInitContext& context);

    /**
     * @brief 注册初始化器
     *
     * 允许外部注册自定义初始化器，支持扩展。
     *
     * @param initializer 初始化器实例
     * @param priority 优先级（数字越小优先级越高，默认100）
     */
    void registerInitializer(std::unique_ptr<LayerInitializer> initializer,
                           int priority = 100);

    /**
     * @brief 清空所有初始化器（用于测试）
     */
    void clearInitializers();

private:
    LayerInitializerFactory();
    ~LayerInitializerFactory() = default;

    // 禁止拷贝和赋值
    LayerInitializerFactory(const LayerInitializerFactory&) = delete;
    LayerInitializerFactory& operator=(const LayerInitializerFactory&) = delete;

    /**
     * @brief 注册所有内置初始化器
     */
    void registerBuiltinInitializers();

    /**
     * @brief 按优先级排序初始化器
     */
    void sortInitializers();

    struct InitializerEntry {
        std::unique_ptr<LayerInitializer> initializer;
        int priority;

        InitializerEntry(std::unique_ptr<LayerInitializer> init, int prio)
            : initializer(std::move(init)), priority(prio) {}
    };

    std::vector<InitializerEntry> initializers_;
    bool sorted_ = false;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_INITIALIZER_FACTORY_H
