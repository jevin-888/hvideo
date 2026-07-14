#ifndef HSVJ_LICENSE_MANAGER_H
#define HSVJ_LICENSE_MANAGER_H

#include <string>
#include <vector>
#include <ctime>
#include <cstdint>
#include <mutex>

namespace hsvj {

/**
 * @brief 授权提示阶段枚举
 */
enum class WarningStage {
    NONE,              // 无提示
    BEFORE_EXPIRY,     // 到期前提示
    EXPIRED_1_15,      // 到期后1-15天提示
    EXPIRED_15_PLUS,   // 到期后15天以上永久提示
    UNLICENSED         // 未授权或授权文件无效（文件不存在/解析/签名失败）
};

/**
 * @brief 授权管理器类
 *
 * 负责授权检查、到期提示和视频播放限制
 */
class LicenseManager {
public:
    LicenseManager();
    ~LicenseManager();

    /**
     * @brief 初始化授权管理器（设置授权目录）
     * @param licenseDir 授权文件所在目录
     */
    void initialize(const std::string& licenseDir);

    /**
     * @brief 检查授权
     * @return 是否已授权
     */
    bool checkLicense();

    /**
     * @brief 更新授权状态（每帧调用）
     * @param deltaTime 帧间隔时间（秒）
     */
    void update(float deltaTime);

    /**
     * @brief 检查是否已授权
     * @return 是否已授权
     */
    bool isLicensed() const { return licensed_; }

    /**
     * @brief 检查是否应该显示提示
     * @return 是否应该显示提示
     */
    bool shouldShowWarning() const { return showWarning_; }

    /**
     * @brief 检查是否应该阻止视频播放（15天后）
     * @return 是否应该阻止视频播放
     */
    bool shouldBlockVideoPlayback() const;

    /**
     * @brief 获取授权信息
     * @return 授权信息字符串
     */
    std::string getLicenseInfo() const { return licenseInfo_; }

    /**
     * @brief 获取提示文本
     * @return 提示文本
     */
    std::string getWarningText() const { return warningText_; }

    /**
     * @brief 获取当前提示阶段
     * @return 当前提示阶段
     */
    WarningStage getWarningStage() const { return warningStage_; }

    /**
     * @brief 获取授权启用的图层 ID 列表（从 license.enabled_layers 解析）
     * @return 已授权图层 ID 列表；未授权或解析失败时返回空列表
     */
    std::vector<int> getEnabledLayerIds() const { return enabledLayerIds_; }

    /**
     * @brief 获取距离到期的天数（正数=未到期，负数=已到期）
     * @return 天数；未授权时返回 -999
     */
    int getDaysUntilExpiry() const;

    /** 客户名称（来自 customer.name） */
    std::string getCustomerName() const { return customerName_; }
    /** 供应商名称（来自 supplier.name） */
    std::string getSupplierName() const { return supplierName_; }
    /** 商业模式：buyout(正常销售) / rent / installment */
    std::string getUsageMode() const { return usageMode_; }
    /** 到期日期字符串 YYYY-MM-DD */
    std::string getExpiryDate() const { return expiryDateFormatted_; }
    /** 到期时间 Unix 时间戳（用于设备上报）；未授权/无到期时为 0 */
    int64_t getExpiryTime() const { return expiryTime_; }
    /** 授权开始时间 Unix 时间戳（来自 validity.issue_时间）；无时为 0 */
    int64_t getStartTime() const { return startTime_; }
    /** 授权模块列表：ktv, vod, effects */
    const std::vector<std::string>& getModules() const { return modules_; }
    /** 授权输入通道数/最大输入区域数；<=0 表示未限制（兼容旧授权） */
    int getInputChannelCount() const { return inputChannelCount_; }
    /** 最近一次授权加载/验证失败原因，用于调试页展示 */
    std::string getLastError() const { return lastError_; }
    /** 欠费金额 */
    double getArrearsAmount() const { return arrearsAmount_; }
    /** 付款二维码 URL */
    std::string getPaymentQrUrl() const { return paymentQrUrl_; }
    /** 剩余天数来源：cloud / offline_state / license_duration / local_clock / 时间_invalid */
    std::string getDaysSource() const { return daysSource_; }
    /** 设备本机时间是否可信 */
    bool isDeviceTimeValid() const { return deviceTimeValid_; }
    /** 检查指定模块是否已授权 */
    bool isModuleEnabled(const std::string& module) const;
    /** 检查指定图层是否已授权 */
    bool isLayerEnabled(int layerId) const;
    /** 重新从文件加载授权（用于导入新 license 后刷新） */
    bool reloadLicense();

    /** 触发云端即时同步 */
    void triggerCloudSync();
    /** 使用网页端时间辅助校正（仅允许减少剩余天数，不允许增加） */
    void applyBrowserTimeHint(int64_t browserTime);

    // --- 授判定核心常量 (全链路唯一基准) ---
    static constexpr const char* DEFAULT_CLOUD_HOST = "60.205.127.117";
    static constexpr int DEFAULT_CLOUD_PORT = 8080;

private:
    /**
     * @brief 加载授权文件
     * @return 是否加载成功
     */
    bool loadLicense();

    /**
     * @brief 验证授权数据
     * @param licenseData 授权数据
     * @return 是否验证通过
     */
    bool validateLicense(const std::string& licenseData);

    /**
     * @brief 计算到期天数
     * @return 到期天数（正数表示未到期，负数表示已到期）
     */
    int calculateDaysUntilExpiry() const;

    /**
     * @brief 更新提示状态
     * @param deltaTime 帧间隔时间（秒）
     */
    void updateWarningState(float deltaTime);

    /**
     * @brief 加载/保存授权运行状态 (license_state.json)
     */
    void loadState();
    void saveState();

    /**
     * @brief 当云端授权同步时的回调
     */
    void onCloudLicenseReceived(bool success, int64_t expiry, int64_t serverTime);

    /**
     * @brief 生成提示文本
     * @param daysUntilExpiry 到期天数
     * @return 提示文本
     */
    std::string generateWarningText(int daysUntilExpiry) const;

    void clearLicenseState();
    void setLastError(const std::string& error) { lastError_ = error; }

    bool licensed_;                 // 是否已授权
    std::string licenseInfo_;       // 授权信息
    std::string licensePath_;      // 授权文件路径
    std::string lastError_;         // 最近一次授权加载/验证失败原因

    // 授权到期时间（Unix时间戳）
    int64_t expiryTime_;
    // 授权开始时间（validity.issue_时间，用于设备上报）
    int64_t startTime_;

    // 提示相关
    bool showWarning_;              // 是否显示提示
    std::string warningText_;      // 提示文本

    // 定时器
    float warningTimer_;            // 当前提示显示计时器（秒）

    // 提示策略阶段
    WarningStage warningStage_;

    // 授权启用的图层 ID（从 license.enabled_layers 解析）
    std::vector<int> enabledLayerIds_;
    // 文档 B2 格式扩展字段
    std::string customerName_;
    std::string supplierName_;
    std::string usageMode_;           // "buyout"(正常销售) | "rent" | "installment"
    std::string expiryDateFormatted_; // 说明："YYYY-MM-DD"
    std::vector<std::string> modules_; // 说明："ktv", "vod", "effects"
    int inputChannelCount_;
    double arrearsAmount_;
    std::string paymentQrUrl_;

    // 混合授权扩展字段
    double remainingDays_;          // 剩余可用天数（云端计算或本地计时）
    double totalLicenseDays_;       // 授权总天数（由开始时间和结束时间计算）
    int64_t lastValidTime_;         // 最后已知合法系统时间
    int64_t lastStateExpiryTime_;   // 上次保存到状态文件的到期时间（用于识别授权变更）
    int64_t lastStateStartTime_;    // 上次保存到状态文件的开始时间（用于识别授权变更）
    int64_t cloudServerTime_;       // 最近一次云端同步返回的服务器时间
    int64_t cloudSyncLocalTime_;    // 最近一次云端同步时的本机时间
    std::string licenseHash_;       // 当前 license.dat 内容指纹，用于绑定状态文件
    std::string stateLicenseHash_;  // 状态文件记录的授权指纹
    std::string daysSource_;        // 剩余天数来源
    bool initializedFromLicense_;   // 当前授权是否已经用授权起止时间初始化过
    bool deviceTimeValid_;          // 本机系统时间是否可信
    int64_t lastSaveTime_;          // 上次保存状态的时间
    std::string statePath_;         // 状态文件路径 (license_state.json)
    bool useCloudPriority_;         // 是否优先使用云端授权
    class CloudLicenseClient* cloudClient_;
    mutable std::recursive_mutex mutex_;      // 保护 remainingDays_ 等成员

    // --- 授权判定核心常量 (全链路唯一基准) ---
    static constexpr int DAYS_BEFORE_WARNING = 15;    // 剩余 15 天开始启动提示
    static constexpr int DAYS_EXPIRED_PERMANENT = 15; // 过期超过 15 天进入强制拦截 (黑屏)
    static constexpr float WARNING_DURATION = 300.0f; // 启动后提示显示时长：300 秒 (5 分钟)
    static constexpr float WARNING_SAVE_PERIOD = 300.0f; // 本地状态保存周期：300 秒 (5 分钟)
    static constexpr int CLOUD_SYNC_INTERVAL = 1800;   // 云端校准周期：30 分钟
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LICENSE_MANAGER_H
