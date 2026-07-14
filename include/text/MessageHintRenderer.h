/**
 * @file MessageHintRenderer.h（文件名）
 * @brief 消息提示渲染器 - Layer 41 专用
 * 
 * 负责渲染各种消息提示：
 * - 播放列表提示（即将播放的歌曲列表）
 * - 操作图标提示（播放/暂停/静音等）
 * - 通用消息提示（可扩展）
 */

#ifndef HSVJ_MESSAGE_HINT_RENDERER_H
#define HSVJ_MESSAGE_HINT_RENDERER_H

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <functional>
#include <cstdint>

namespace hsvj {

class VulkanRenderer;
class VulkanTextOverlayBridge;

/**
 * @brief 消息提示类型
 */
enum class HintType {
    NONE = 0,           // 无提示

    PLAYLIST,           // 播放列表提示
    PLAY,               // 播放
    PAUSE,              // 暂停
    NEXT,               // 下一曲
    PREV,               // 重播（从头播放当前歌曲）
    VOLUME_UP,          // 音量增加
    VOLUME_DOWN,        // 音量减少
    MUTE,               // 静音
    UNMUTE,             // 取消静音
    AUDIO_TRACK,        // 原唱音轨
    BACKING_TRACK,      // 伴唱音轨
    CUSTOM              // 自定义消息
};

/**
 * @brief 消息提示项
 */
struct HintItem {
    HintType type;                  // 提示类型

    std::string text;               // 提示文本

    std::string icon;               // 图标字符（Unicode）

    float displayDuration;          // 显示时长（秒），0表示永久显示

    bool isPermanent;               // 是否永久显示

    std::chrono::steady_clock::time_point startTime;  // 开始时间
    
    HintItem() : type(HintType::NONE), displayDuration(1.0f), isPermanent(false) {}
};

/**
 * @brief 播放列表项信息
 */
struct PlaylistItemInfo {
    std::string name;       // 歌曲名称

    std::string artist;     // 艺术家（可选）

    int index;              // 在列表中的索引

    bool operator==(const PlaylistItemInfo& other) const {
        return name == other.name && artist == other.artist && index == other.index;
    }
    bool operator!=(const PlaylistItemInfo& other) const {
        return !(*this == other);
    }
};

/**
 * @brief 消息提示渲染器
 */
class MessageHintRenderer {
public:
    MessageHintRenderer();
    ~MessageHintRenderer();

    /**
     * @brief 初始化渲染器（使用共享的字体叠加桥接器）
     * @param sharedBridge 共享的 VulkanTextOverlayBridge 实例（由外部管理生命周期）
     * @param 宽度 渲染宽度
     * @param 高度 渲染高度
     * @return 是否初始化成功
     */
    bool initialize(VulkanTextOverlayBridge* sharedBridge, int width, int height);

    /**
     * @brief 检查是否已初始化
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief 关闭渲染器
     */
    void shutdown();

    void releaseGpuResources(VulkanRenderer* renderer);

    /** VkDevice 已销毁：清空缓存与图标状态，不调用 Vulkan */
    void dropGpuCachesWithoutDestroy();

    /**
     * @brief 更新渲染器状态
     * @param deltaTime 距上一帧的时间（秒）
     */
    void update(float deltaTime);

    /**
     * @brief 渲染消息提示
     * @param 渲染器 Vulkan渲染器
     * @param x 渲染位置X
     * @param y 渲染位置Y
     * @param 宽度 渲染宽度
     * @param 高度 渲染高度
     * @param alpha 透明度
     * @param displayAlign 显示对齐（0=左, 1=中, 2=右），由调用方传入，保证按配置绘制
     */
    void render(VulkanRenderer* renderer, int x, int y, int width, int height, float alpha, int displayAlign = -1);

    /**
     * @brief 在 render pass 外更新纹理
     * 
     * 重要：此方法必须在 Vulkan render pass 开始前调用！
     * Vulkan 不允许在 render pass 内进行纹理更新操作。
     * 
     * @param 渲染器 Vulkan渲染器
     * @param 宽度 渲染宽度
     * @param 高度 渲染高度
     */
    void updateTextures(VulkanRenderer* renderer, int width, int height);

    // ========== 播放列表提示 ==========
    
    /**
     * @brief 设置播放列表提示
     * @param items 播放列表项
     * @param showCount 显示数量
     * @param totalRemaining 剩余总数
     */
    void setPlaylistHint(const std::vector<PlaylistItemInfo>& items, int showCount, int totalRemaining);

    /**
     * @brief 清除播放列表提示
     */
    void clearPlaylistHint();

    /**
     * @brief 设置播放列表提示可见性
     * @param visible 是否可见
     */
    void setPlaylistHintVisible(bool visible);

    /**
     * @brief 设置播放列表提示标题类型：0=即将播放（结束时间提示），1=已选列表（起始时间提示）
     */
    void setPlaylistHintListType(int type) { playlistHintListType_ = type; }

    // ========== 操作图标提示 ==========

    /**
     * @brief 显示操作提示
     * @param type 提示类型
     * @param customText 自定义文本（可选）
     */
    void showOperationHint(HintType type, const std::string& customText  = "");

    /**
     * @brief 显示受保护的永久操作提示，普通提示不会覆盖它
     * @param type 提示类型
     * @param customText 自定义文本（可选）
     */
    void showProtectedOperationHint(HintType type, const std::string& customText = "");

    /**
     * @brief 清除操作提示
     */
    void clearOperationHint();

    /**
     * @brief 检查操作提示是否正在激活
     * @return 操作提示是否正在显示
     */
    bool isOperationHintActive() const;

    /**
     * @brief 检查是否有活动的提示
     * @return 是否有提示正在显示
     */
    bool hasActiveHint() const;

    /**
     * @brief 检查是否需要更新纹理（有活动提示且内容/样式已变更）
     * @return 是否需要调用 updateTextures
     */
    bool needsTextureUpdate() const;

    /**
     * @brief 当前用于绘制的纹理 ID（供切片渲染等使用，无纹理时返回 0）
     */
    uint32_t getTextureId() const;

    // ========== 样式设置 ==========

    /**
     * @brief 设置字体大小
     * @param size 字体大小
     */
    void setFontSize(float size);

    /**
     * @brief 设置显示对齐方式
     * @param align 对齐方式 (0=左, 1=中, 2=右)
     */
    void setDisplayAlign(int align) { if (displayAlign_ != align) { displayAlign_ = align; dirty_ = true; } }

    /**
     * @brief 设置背景透明度
     * @param alpha 透明度 (0.0-1.0)
     */
    void setBackgroundAlpha(float alpha) { if (bgAlpha_ != alpha) { bgAlpha_ = alpha; dirty_ = true; } }

    /**
     * @brief 设置字体路径
     * @param path 字体文件路径
     */
    void setFontPath(const std::string& path) { fontPath_ = path; }

    /**
     * @brief 设置操作提示显示时长
     * @param duration 时长（秒）
     */
    void setOperationHintDuration(float duration) { operationHintDuration_ = duration; }

    /**
     * @brief 设置文本颜色
     * @param r 红色分量 (0.0-1.0)
     * @param g 绿色分量 (0.0-1.0)
     * @param b 蓝色分量 (0.0-1.0)
     * @param a 透明度 (0.0-1.0)
     */
    void setTextColor(float r, float g, float b, float a = 1.0f) {
        textColorR_ = r; textColorG_ = g; textColorB_ = b; textColorA_ = a;
        dirty_ = true;
    }

    /**
     * @brief 设置背景颜色
     * @param r 红色分量 (0.0-1.0)
     * @param g 绿色分量 (0.0-1.0)
     * @param b 蓝色分量 (0.0-1.0)
     * @param a 透明度 (0.0-1.0)
     */
    void setBgColor(float r, float g, float b, float a) {
        bgColorR_ = r; bgColorG_ = g; bgColorB_ = b; bgAlpha_ = a;
        dirty_ = true;
    }

    /**
     * @brief 设置显示数量
     * @param count 显示的播放列表项数量
     */
    void setShowCount(int count) { if (playlistShowCount_ != count) { playlistShowCount_ = count; dirty_ = true; } }

    /**
     * @brief 检查播放列表提示是否可见
     * @return 播放列表提示是否设置为可见
     */
    bool isPlaylistHintVisible() const;

    /**
     * @brief 检查是否有播放列表项
     * @return 是否有播放列表项
     */
    bool hasPlaylistItems() const;

private:
    /** 仅加载 PNG 图标（首帧）；文字预热见 textWarmupNextIndex_ 分帧逻辑 */
    void preloadHintIcons(VulkanRenderer* renderer);

    struct IconTextureInfo {
        uint32_t textureId;
        int width;
        int height;
        bool loaded;
        bool tried;
        IconTextureInfo()
            : textureId(0), width(0), height(0), loaded(false), tried(false) {}
    };

    // 操作提示文字纹理缓存（每种文字独立纹理，避免切换时重新光栅化）
    struct CachedTextEntry {
        uint32_t textureId = 0;
        int width = 0;
        int height = 0;
    };
    // key = displayText + fontSize；超过 kTextCacheMax 条时淘汰最旧的（LRU-insert-order）
    static constexpr int kTextCacheMax = 32;
    std::unordered_map<std::string, CachedTextEntry> textCache_;
    std::vector<std::string> textCacheOrder_; // 插入顺序，用于淘汰

    // 向 textCache_ 插入一条，超限时销毁最旧纹理
    void insertTextCache(VulkanRenderer* renderer, const std::string& key, CachedTextEntry entry);

    // 后台光栅化：RGBA buffer 准备好后放入此队列，渲染线程在 updateTextures 里上传
    struct PendingUpload {
        std::string cacheKey;
        std::string text;
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
    };
    mutable std::mutex uploadMutex_;
    std::vector<PendingUpload> pendingUploads_; // 后台线程写，渲染线程读

    // 在后台线程光栅化文字，结果放入 pendingUploads_
    void rasterizeAsync(const std::string& text, const std::string& cacheKey, float fontSize);

    // 内部渲染方法
    // render播放列表Hint：displayAlign 0=左 1=中 2=右，-1 使用成员 displayAlign_
    // renderOperationHint：操作提示始终水平居中，不受配置影响
    void renderPlaylistHint(VulkanRenderer* renderer, int x, int y, int width, int height, float alpha, int displayAlign = -1);
    void renderOperationHint(VulkanRenderer* renderer, int x, int y, int width, int height, float alpha);

    bool shouldHidePermanentHint(HintType newType) const;

    std::unique_ptr<VulkanTextOverlayBridge> ownedVtoBridge_; // 拥有的桥接器实例（用于独立纹理）
    VulkanTextOverlayBridge* vtoBridgePtr_ = nullptr;        // 当前使用的桥接器引用（共享或私有）

    bool initialized_;
    bool dirty_;                   // 样式或内容是否已更改

    bool iconsPreloaded_ = false;   // PNG 图标是否已预加载

    int textWarmupNextIndex_ = -1;  // 文字预热：>=0 为 kWarmupTexts 下标；-1 未开始或已完成

    int renderWidth_;
    int renderHeight_;

    // 播放列表提示

    std::vector<PlaylistItemInfo> playlistItems_;
    int playlistShowCount_;
    int playlistTotalRemaining_;
    bool playlistHintVisible_;
    int playlistHintListType_ = 0;  // 0=即将播放 1=已选列表

    float playlistScrollTimeAccum_ = 0.0f;   // 滚动计时，每 0.5s 步进

    int playlistScrollCharOffset_ = 0;       // 当前滚动字符偏移（用于 6 字窗口）

    bool playlistScrollRestAtZero_ = false; // 回绕到 0 时多停一拍，避免列表抖动

    // 操作提示（hintMutex_ 保护：HTTP/命令线程写入，渲染线程读取）
    mutable std::mutex hintMutex_;
    HintItem currentOperationHint_;
    bool operationHintActive_;
    bool protectedOperationHint_;

    // 纹理状态跟踪（强制更新机制）

    int currentTextureType_;       // 当前纹理类型: 0=无, 1=操作提示, 2=播放列表

    int currentTextureWidth_;      // 当前纹理宽度

    int currentTextureHeight_;     // 当前纹理高度

    std::string lastOperationText_; // 上次渲染的操作提示文本
    std::string lastOperationCacheKey_; // 上次操作提示文字纹理缓存 key
    HintType lastOperationHintType_; // 上次渲染的操作提示类型（供 render 线程无锁读取）

    std::string lastPlaylistText_;  // 上次渲染的播放列表文本

    int lastAppliedAlign_;         // 上次应用的对齐方式 (assAlign)

    // 样式

    float fontSize_;           // 操作提示字体（下一首、音量等）

    float playlistFontSize_;   // 播放列表提示字体（缩小以配合每行 12 字）

    int displayAlign_;  // 0=左, 1=中, 2=右

    float textColorR_, textColorG_, textColorB_, textColorA_;  // 文本颜色

    float bgColorR_, bgColorG_, bgColorB_, bgAlpha_;           // 背景颜色

    std::string fontPath_;
    float operationHintDuration_;  // 操作提示默认显示时长

    // 操作提示图标纹理（从 RES_DIR 加载）
    IconTextureInfo playIcon_;
    IconTextureInfo pauseIcon_;
    IconTextureInfo nextIcon_;
    IconTextureInfo prevIcon_;
    IconTextureInfo volumeUpIcon_;
    IconTextureInfo volumeDownIcon_;
    IconTextureInfo muteIcon_;
    IconTextureInfo unmuteIcon_;
    IconTextureInfo audioTrackIcon_;
    IconTextureInfo backingTrackIcon_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_MESSAGE_HINT_RENDERER_H

