/**
 * @file Mubu.h（文件名）
 * @brief 图层管理器类定义
 * 
 * 本文件定义了图层管理器类Mubu，负责：
 * - 图层的创建、删除和管理
 * - 图层的渲染和更新
 * - 图层优先级排序
 * - 可见图层筛选
 */

#ifndef HSVJ_MUBU_H
#define HSVJ_MUBU_H

#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "layer/Layer.h"
#include "core/Resolution.h"

namespace hsvj {

class SharedLibassHolder;
class SharedTextOverlayHolder;

/**
 * @brief 图层管理器类
 * 
 * 负责图层的创建、管理和渲染，是图层系统的核心管理组件
 */
class Mubu {
public:
    Mubu();
    ~Mubu();
    
    /**
     * @brief 初始化图层管理器
     * @param resolution 分辨率
     * @return 是否初始化成功
     */
    bool initialize(const Resolution& resolution);
    
    /**
     * @brief 关闭图层管理器，释放所有资源
     */
    void shutdown();
    
    /**
     * @brief 创建图层
     * @param layerId 图层ID
     * @param type 图层类型
     * @param silent 是否静默创建（不输出日志），默认false
     * @return 是否创建成功
     */
    bool createLayer(int layerId, LayerType type, bool silent = false);
    
    /**
     * @brief 删除图层
     * @param layerId 图层ID
     * @return 是否删除成功
     */
    bool removeLayer(int layerId);
    
    /**
     * @brief 获取图层（非const版本）
     * @param layerId 图层ID
     * @return 图层指针，不存在返回nullptr
     */
    Layer* getLayer(int layerId);
    
    /**
     * @brief 获取图层（const版本）
     * @param layerId 图层ID
     * @return 图层指针，不存在返回nullptr
     */
    const Layer* getLayer(int layerId) const;
    
    /**
     * @brief 获取所有图层ID列表
     * @return 图层ID列表
     */
    std::vector<int> getAllLayerIds() const;

    /**
     * @brief 获取所有图层列表
     * @return 图层智能指针列表（保证生命周期跨线程安全）
     */
    std::vector<std::shared_ptr<Layer>> getAllLayers() const;
    
    /**
     * @brief 获取所有可见图层列表
     * @return 可见图层智能指针列表（保证生命周期跨线程安全）
     */
    std::vector<std::shared_ptr<Layer>> getVisibleLayers() const;
    
    /**
     * @brief 渲染所有图层
     */
    void renderLayers();
    
    /**
     * @brief 更新所有图层（每帧调用）
     * @param deltaTime 帧间隔时间（秒）
     */
    void updateLayers(float deltaTime);
    
    /**
     * @brief 获取分辨率
     * @return 分辨率
     */
    Resolution getResolution() const { return resolution_; }
    
    /**
     * @brief 设置分辨率
     * @param res 分辨率
     */
    void setResolution(const Resolution& res) { resolution_ = res; }
    
    /**
     * @brief 图层排序（按优先级）
     * @param verbose 是否输出详细的图层列表日志，默认false（减少日志冗余）
     */
    void sortLayersByPriority(bool verbose = false);
    
    /**
     * @brief 获取当前正在播放的视频图层数量
     * @return 正在播放的视频图层数量
     */
    int getActiveVideoLayerCount() const;
    
    /**
     * @brief 获取同步启动时间（用于多视频图层同步启动）
     * @return 同步启动时间（秒），如果未设置返回0
     */
    double getSyncStartTime() const;

    /**
     * @brief 设置同步启动时间（用于多视频图层同步启动）
     * @param startTime 启动时间（秒）
     */
    void setSyncStartTime(double startTime);

    /**
     * @brief 清除同步启动时间
     */
    void clearSyncStartTime();

    /**
     * @brief 获取全局播放时钟基准（用于多视频图层帧同步）
     * @return 全局播放时钟基准（秒），如果未设置返回0
     * 
     * 全局播放时钟基准用于确保所有视频的第一帧都对齐到相同的时间点
     * 注意：返回的是引用，允许外部直接修改（用于第一帧对齐）
     */
    double& getGlobalPlayClockBase() { return globalPlayClockBase_; }
    const double& getGlobalPlayClockBase() const { return globalPlayClockBase_; }

    /**
     * @brief 设置全局播放时钟基准（用于多视频图层帧同步）
     * @param baseTime 基准时间（秒）
     * 
     * 当第一个视频的第一帧解码时，设置全局播放时钟基准
     * 后续视频的第一帧会调整自己的 startTime_ 以对齐到这个基准
     */
    void setGlobalPlayClockBase(double baseTime);

    /**
     * @brief 清除全局播放时钟基准
     */
    void clearGlobalPlayClockBase();

    /**
     * @brief 设置共享 libass 实例（Layer 21 歌词使用，由 Engine 注入）
     * @param holder SharedLibassHolder 智能指针
     */
    void setSharedLibassHolder(std::shared_ptr<SharedLibassHolder> holder);

    /**
     * @brief 获取共享 libass 实例（创建 Layer 21 时传入）
     */
    SharedLibassHolder* getSharedLibassHolder() const;

    /**
     * @brief 设置共享文本叠加持有者（Layer 40, 41 共享字体使用，由 Engine 注入）
     * @param holder SharedTextOverlayHolder 共享指针
     */
    void setSharedTextOverlayHolder(std::shared_ptr<SharedTextOverlayHolder> holder);

    /**
     * @brief 获取共享文本叠加持有者
     */
    SharedTextOverlayHolder* getSharedTextOverlayHolder() const;

private:
    Resolution resolution_;                                     // 分辨率
    std::unordered_map<int, std::shared_ptr<Layer>> layers_;   // 图层映射表（使用 shared_ptr 跨线程安全持有）
    std::vector<int> layerOrder_;                              // 按优先级排序的图层ID列表
    mutable std::recursive_mutex layersMutex_;                 // 保护 layers_ 的并发访问（使用 recursive_mutex 支持嵌套调用）
    
    // 同步启动时间管理（用于多视频图层同步启动）
    mutable std::mutex syncStartTimeMutex_;                   // 保护 syncStartTime_ 的并发访问
    double syncStartTime_;                                    // 同步启动时间（秒），0表示未设置
    
    // 全局播放时钟基准（用于多视频图层帧同步）
    mutable std::mutex globalPlayClockMutex_;                 // 保护 globalPlayClockBase_ 的并发访问
    double globalPlayClockBase_;                              // 全局播放时钟基准（秒），0表示未设置
    std::shared_ptr<SharedLibassHolder> sharedLibassHolder_;   // Layer 21 歌词共享 libass 实例
    std::shared_ptr<SharedTextOverlayHolder> sharedTextOverlayHolder_; // Layer 40/41 共享文本/字体实例
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_MUBU_H
