/**
 * @file SceneManager.h（文件名）
 * @brief 场景管理器类定义
 * 
 * 本文件定义了场景管理器类，负责：
 * - 场景的加载、保存和管理
 * - 场景切换和应用
 * - 场景模板管理
 */

#ifndef HSVJ_SCENE_MANAGER_H
#define HSVJ_SCENE_MANAGER_H

#include <json/json.h>
#include <deque>
#include <string>
#include <memory>
#include <vector>
#include "core/PathConfig.h"

namespace hsvj {

class Mubu;
class SystemConfig;
class VulkanRenderer;

/**
 * @brief 场景管理器类
 * 
 * 负责场景的加载、保存、切换和应用
 */
class SceneManager {
public:
    SceneManager();
    ~SceneManager();
    
    /**
     * @brief 设置图层管理器
     * @param mubu 图层管理器指针
     */
    void setMubu(Mubu* mubu) { mubu_ = mubu; }
    
    /**
     * @brief 设置系统配置
     * @param config 系统配置指针
     */
    void setSystemConfig(SystemConfig* config) { systemConfig_ = config; }

    /**
     * @brief 设置渲染器
     * @param 渲染器 渲染器指针
     */
    void setRenderer(VulkanRenderer* renderer) { renderer_ = renderer; }

    /**
     * @brief 设置授权图层 ID，用于场景加载时补创建缺失的运行时图层
     * @param layerIds 授权图层 ID 列表
     */
    void setAuthorizedLayerIds(const std::vector<int>& layerIds) { authorizedLayerIds_ = layerIds; }
    
    /**
     * @brief 切换场景（按名称）
     * @param sceneName 场景名称
     * @return 是否切换成功
     */
    bool switchScene(const std::string& sceneName);
    
    /**
     * @brief 从文件加载场景
     * @param scenePath 场景文件路径
     * @return 是否加载成功
     */
    bool loadScene(const std::string& scenePath);
    
    /**
     * @brief 应用场景配置（从JSON字符串）
     * @param sceneConfigJson 场景配置JSON字符串
     * @return 是否应用成功
     */
    bool applyScene(const std::string& sceneConfigJson);
    
    /**
     * @brief 列出所有场景
     * @return 场景名称列表
     */
    std::vector<std::string> listScenes();
    
    /**
     * @brief 删除场景
     * @param sceneName 场景名称
     * @return 是否删除成功
     */
    bool deleteScene(const std::string& sceneName);
    
    /**
     * @brief 获取当前场景名称
     * @return 当前场景名称
     */
    std::string getCurrentSceneName() const { return currentSceneName_; }
    
    /**
     * @brief 获取当前场景ID
     * @return 当前场景ID
     */
    std::string getCurrentSceneId() const { return currentSceneId_; }
    
    /**
     * @brief 获取当前配置文件路径
     * @return 当前配置文件路径（config.json 或场景文件路径）
     */
    std::string getCurrentConfigPath() const { return currentConfigPath_; }
    
    /**
     * @brief 设置当前配置文件路径
     * @param path 配置文件路径
     */
    void setCurrentConfigPath(const std::string& path);
    
    /**
     * @brief 场景应用统计信息结构
     */
    struct SceneApplyStats {
        int layersApplied = 0;                                    // 已应用的图层数量
    std::vector<std::pair<int, std::string>> layersUpdated;  // 更新的图层列表（layerId, status）
    };
    
    /**
     * @brief 应用场景配置（带统计信息，供CommandRouter使用）
     * @param configJson 场景配置JSON字符串
     * @param stats 统计信息（输出）
     * @return 是否应用成功
     */
    bool applySceneConfigWithStats(const std::string& configJson, SceneApplyStats* stats);
    
    /**
     * @brief 从文件加载场景（带统计信息）
     * @param scenePath 场景文件路径
     * @param stats 统计信息（输出）
     * @return 是否加载成功
     */
    bool loadSceneFromFileWithStats(const std::string& scenePath, SceneApplyStats* stats);

    /**
     * @brief 逐帧处理延后的非媒体图层重内容刷新
     * @param deltaTime 帧间隔（当前未使用）
     */
    void update(float deltaTime);

private:
    struct DeferredSceneLayer {
        int layerId = 0;
        std::string layerKey;
        Json::Value layerConfig;
    };

    /**
     * @brief 从文件加载场景（内部方法）
     * @param scenePath 场景文件路径
     * @return 是否加载成功
     */
    bool loadSceneFromFile(const std::string& scenePath);
    
    /**
     * @brief 应用场景配置（内部方法）
     * @param configJson 场景配置JSON字符串
     * @return 是否应用成功
     */
    bool applySceneConfig(const std::string& configJson);
    void applyDeferredNonMediaLayer(const DeferredSceneLayer& deferred);
    void syncLayerConfigToSystemConfig(int layerId,
                                       const std::string& layerKey,
                                       const Json::Value& layerConfig);
    void refreshLyricTimeBinding();
    
    Mubu* mubu_ = nullptr;           // 图层管理器指针
    SystemConfig* systemConfig_ = nullptr; // 系统配置指针
    VulkanRenderer* renderer_ = nullptr; // 渲染器指针
    std::vector<int> authorizedLayerIds_; // 授权图层 ID 列表
    std::string currentSceneName_;   // 当前场景名称
    std::string currentSceneId_;     // 当前场景ID
    std::string currentConfigPath_;  // 当前配置文件路径（config.json 或场景文件）
    std::string layoutPath_;         // 场景布局目录路径（LAYOUT_DIR）
    std::deque<DeferredSceneLayer> deferredNonMediaLayers_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_SCENE_MANAGER_H

