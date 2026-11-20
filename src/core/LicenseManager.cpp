/**
 * @file LicenseManager.cpp（文件名）
 * @brief 授权管理器实现
 *
 * 本文件实现了授权管理器类，负责：
 * - 授权文件验证
 * - 授权状态管理
 * - 授权过期检测和警告
 * - 授权水印显示
 */

#include "core/LicenseManager.h"
#include "core/LayerDefinitions.h"
#include "core/LicenseVerify.h"
#include "foundation/PathConfig.h"
#include "network/CloudLicenseClient.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

namespace {
#ifndef HVIDEO_LICENSE_HMAC_KEY
#error "HVIDEO_LICENSE_HMAC_KEY must be configured by the build"
#endif
static_assert(sizeof(HVIDEO_LICENSE_HMAC_KEY) > 1, "License HMAC key must not be empty");
const char* const kLicenseHmacKey = HVIDEO_LICENSE_HMAC_KEY;

static std::string simpleStableHash(const std::string& content) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : content) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << std::hex << hash;
    return oss.str();
}

static int displayDays(double days) {
    if (days > 0.0) return static_cast<int>(std::floor(days));
    return static_cast<int>(std::ceil(days));
}
}

namespace hsvj {
static std::string formatExpiryDate(int64_t expiryTime);

// Constants are now defined as constexpr in License管理器.h

LicenseManager::LicenseManager()
    : licensed_(false)
    , licensePath_(LICENSE_PATH)
    , expiryTime_(0)
    , startTime_(0)
    , showWarning_(false)
    , warningTimer_(0.0f)
    , warningStage_(WarningStage::NONE)
    , inputChannelCount_(0)
    , arrearsAmount_(0.0)
    , remainingDays_(0.0)
    , totalLicenseDays_(0.0)
    , lastValidTime_(0)
    , lastStateExpiryTime_(0)
    , lastStateStartTime_(0)
    , cloudServerTime_(0)
    , cloudSyncLocalTime_(0)
    , daysSource_("unknown")
    , initializedFromLicense_(false)
    , deviceTimeValid_(false)
    , lastSaveTime_(0)
    , useCloudPriority_(true)
    , transientOnlineLicense_(false)
    , cloudClient_(nullptr)
{
    LOG_DEBUG("LicenseManager initialized");
    cloudClient_ = new CloudLicenseClient();
}

LicenseManager::~LicenseManager() {
    if (cloudClient_) {
        cloudClient_->stop();
        delete cloudClient_;
    }
    saveState();
}

void LicenseManager::initialize(const std::string& licenseDir) {
    // 更新 license file 路径 to: directory + "license.dat"
    if (!licenseDir.empty()) {
        // 确保目录路径以 '/' 结尾
        std::string dir = licenseDir;
        if (dir.back() != '/') {
            dir += '/';
        }
        licensePath_ = dir + "license.dat";
        statePath_ = dir + "license_state.json";
        LOG_DEBUG("LicenseManager: license path set to %s", licensePath_.c_str());
    }
    
    loadState();
}

void LicenseManager::clearLicenseState() {
    licensed_ = false;
    expiryTime_ = 0;
    startTime_ = 0;
    enabledLayerIds_.clear();
    modules_.clear();
    customerName_.clear();
    supplierName_.clear();
    usageMode_.clear();
    expiryDateFormatted_.clear();
    inputChannelCount_ = 0;
    arrearsAmount_ = 0.0;
    totalLicenseDays_ = 0.0;
    lastStateExpiryTime_ = 0;
    lastStateStartTime_ = 0;
    cloudServerTime_ = 0;
    cloudSyncLocalTime_ = 0;
    licenseHash_.clear();
    stateLicenseHash_.clear();
    daysSource_ = "unlicensed";
    initializedFromLicense_ = false;
    deviceTimeValid_ = false;
    paymentQrUrl_.clear();
    licenseInfo_ = "VJEngine";
    warningStage_ = WarningStage::UNLICENSED;
    showWarning_ = true;
    warningText_ = "VJEngine";
    transientOnlineLicense_ = false;
    cloudManagedLicense_ = false;
    serverLicensePermanent_ = false;
    remainingDays_ = 0.0;
}

bool LicenseManager::checkLicense(bool allowCloudSync) {
    // A network-mode fallback must not leave a previous cloud worker running.
    // Stop it before taking mutex_ because its callback also uses mutex_.
    if (cloudClient_) {
        cloudClient_->stop();
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    useCloudPriority_ = allowCloudSync;
    if (!allowCloudSync) {
        startupLocalSyncRequired_ = false;
        clearLicenseState();
        LOG_INFO("[License] 网络 VOD 模式，仅等待 VOD 服务器授权");
        return false;
    }

    migrateLegacyCloudLicense();
    // Both import and cloud renewal use license.dat. Only a manually imported
    // grant waits for startup reporting before allowing cloud replacement.
    const std::string localContent = getLocalLicenseContent();
    LicenseManager localCandidate;
    const int64_t now = std::max<int64_t>(std::time(nullptr), lastValidTime_);
    if (!localContent.empty() && localCandidate.validateLicenseFile(localContent) &&
        localCandidate.startTime_ > 0 && localCandidate.startTime_ <= now + 300 &&
        localCandidate.expiryTime_ > now) {
        const bool loadedLocal = validateLicenseFile(localContent);
        if (loadedLocal) {
            cloudManagedLicense_ = localCandidate.licenseHash_ == persistedCloudLicenseHash_;
            refreshLoadedLicense();
            if (!cloudManagedLicense_ && (startupLocalSyncRequired_ || localLicenseSyncPending_)) {
                LOG_INFO("[License] 已优先加载有效本地授权，等待启动上报确认后再启动云端同步");
                return licensed_;
            }
            LOG_INFO("[License] 已加载有效本地授权，启动云端同步");
            startCloudSync();
            return licensed_;
        }
    }

    applyLocalFallback("等待有效云端授权", 0);
    startCloudSync();
    return licensed_;
}

void LicenseManager::migrateLegacyCloudLicense() {
    if (licensePath_.empty()) return;
    const std::string legacyPath = FileUtils::getDirectory(licensePath_) + "/license-cloud.dat";
    if (!FileUtils::exists(legacyPath) || FileUtils::isDirectory(legacyPath)) return;

    const auto localContent = getLocalLicenseContent();
    const auto legacyContent = FileUtils::readTextFile(legacyPath);
    LicenseManager localCandidate;
    LicenseManager legacyCandidate;
    const int64_t now = std::max<int64_t>(std::time(nullptr), lastValidTime_);
    const bool localValid = localCandidate.validateLicenseFile(localContent);
    const bool legacyValid = legacyCandidate.validateLicenseFile(legacyContent) &&
        legacyCandidate.startTime_ > 0 && legacyCandidate.startTime_ <= now + 300 &&
        legacyCandidate.expiryTime_ > now;
    const bool localCurrent = localValid && localCandidate.startTime_ > 0 &&
        localCandidate.startTime_ <= now + 300 && localCandidate.expiryTime_ > now;
    // Preserve the existing startup priority: a current license.dat wins.
    if (legacyValid && !localLicenseSyncPending_ && !localCurrent) {
        if (!FileUtils::writeTextFile(licensePath_, legacyContent)) {
            LOG_WARN("[License] 旧云端授权迁移失败，保留原文件等待重试");
            return;
        }
        persistedCloudLicenseHash_ = legacyCandidate.licenseHash_;
        validateLicenseFile(legacyContent);
        cloudManagedLicense_ = true;
        refreshLoadedLicense();
        LOG_INFO("[License] 旧云端授权已迁移至 license.dat");
    } else if (!localValid) {
        // Never discard the only historical file when neither grant is usable.
        return;
    }
    if (!FileUtils::removeFile(legacyPath)) {
        LOG_WARN("[License] 旧云端授权文件清理失败，后续启动重试");
    }
}

void LicenseManager::startCloudSync() {
    if (!cloudClient_) return;
    cloudClient_->start(DEFAULT_CLOUD_HOST, DEFAULT_CLOUD_PORT,
        SystemUtils::generateDeviceFingerprint(), 8080,
        [this](const CloudLicenseClient::LicenseState& state) {
            onCloudLicenseReceived(state.success, state.server_time, state.content);
        }, CLOUD_SYNC_INTERVAL);
}

void LicenseManager::refreshLoadedLicense() {
    transientOnlineLicense_ = false;
    serverLicensePermanent_ = false;
    // 2. 根据可信时间源刷新剩余天数
    int64_t now = std::time(nullptr);
    double licenseDurationDays = (startTime_ > 0 && expiryTime_ > startTime_)
        ? static_cast<double>(expiryTime_ - startTime_) / 86400.0
        : 0.0;
    bool stateMatchesLicense = !licenseHash_.empty()
        && stateLicenseHash_ == licenseHash_
        && lastStateStartTime_ == startTime_
        && lastStateExpiryTime_ == expiryTime_;
    if (stateMatchesLicense && !initializedFromLicense_) {
        initializedFromLicense_ = true;
    }
    if (!stateMatchesLicense) {
        totalLicenseDays_ = licenseDurationDays;
        remainingDays_ = licenseDurationDays;
        cloudServerTime_ = 0;
        cloudSyncLocalTime_ = 0;
        lastStateStartTime_ = startTime_;
        lastStateExpiryTime_ = expiryTime_;
        stateLicenseHash_ = licenseHash_;
        daysSource_ = "license_duration";
        initializedFromLicense_ = true;
    }
    const bool clockRollbackDetected = (lastValidTime_ > 0 && now < lastValidTime_ - 3600);
    const bool hasMatchingSavedState = (lastStateStartTime_ == startTime_ && lastStateExpiryTime_ == expiryTime_ && remainingDays_ != 0.0);
    const bool localClockBeforeLicenseStart = (startTime_ > 0 && now < startTime_ - 3600);
    deviceTimeValid_ = !localClockBeforeLicenseStart && !clockRollbackDetected;
    bool savedDaysLooksSane = stateMatchesLicense && hasMatchingSavedState &&
        (totalLicenseDays_ <= 0.0 || (remainingDays_ >= -DAYS_EXPIRED_PERMANENT && remainingDays_ <= totalLicenseDays_ + 1.0));
    if (localClockBeforeLicenseStart) {
        if (savedDaysLooksSane) {
            LOG_WARN("[License] 本机时间早于授权开始时间，沿用云端/状态文件剩余天数 %.2f", remainingDays_);
            daysSource_ = "offline_state";
        } else {
            remainingDays_ = totalLicenseDays_ > 0.0 ? totalLicenseDays_ : remainingDays_;
            LOG_WARN("[License] 本机时间早于授权开始时间，使用授权周期天数 %.2f 等待云端校准", remainingDays_);
            cloudServerTime_ = 0;
            cloudSyncLocalTime_ = 0;
            daysSource_ = "license_duration";
        }
    } else if (clockRollbackDetected) {
        LOG_WARN("[License] 检测到系统时间回拨（now=%lld < lastValid=%lld），进入离线扣天保护",
                 static_cast<long long>(now), static_cast<long long>(lastValidTime_));
        if (savedDaysLooksSane) {
            LOG_WARN("[License] 时间不准，沿用离线剩余天数 %.2f，短时间重启不额外扣天", remainingDays_);
            daysSource_ = "offline_state";
        } else {
            remainingDays_ = totalLicenseDays_ > 0.0 ? totalLicenseDays_ : remainingDays_;
            daysSource_ = "license_duration";
        }
    } else {
        if (stateMatchesLicense && (daysSource_ == "cloud" || daysSource_ == "offline_state" || daysSource_ == "license_duration" || daysSource_ == "browser_time")) {
            daysSource_ = "offline_state";
        } else if (expiryTime_ > 0) {
            remainingDays_ = static_cast<double>(expiryTime_ - now) / 86400.0;
            daysSource_ = "local_clock";
        }
    }
    
    lastValidTime_ = now;
    saveState();

    // 3. 结果判定
    // 只要签名校验成功 (loadLicense() 内部已包含 verifyLicenseSignature)，即视为 [持有合法授权]
    // 具体的过期逻辑（15天宽限）由 updateWarningState -> warningStage_ 统一管理执行
    licensed_ = true;
    
    LOG_INFO("[License] 授权文件校验成功，状态机接管阶段流转。剩余天数=%.2f 客户=%s", 
             remainingDays_, customerName_.c_str());

    // 4. [BUG_FIX] 立即刷新一次状态，防止 reloadLicense 后仍然维持旧状态导致黑屏
    updateWarningState(0.0f);
    
}

bool LicenseManager::isServerLicenseMode() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !useCloudPriority_;
}

std::string LicenseManager::getLicenseSource() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !licensed_ ? "none" : transientOnlineLicense_ ? "server" : cloudManagedLicense_ ? "cloud" : "local";
}

std::string LicenseManager::getLocalLicenseContent() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (licensePath_.empty() || !FileUtils::exists(licensePath_) ||
        FileUtils::isDirectory(licensePath_)) return {};
    return FileUtils::readTextFile(licensePath_);
}

void LicenseManager::setCloudLicenseAppliedCallback(std::function<void()> callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    cloudLicenseAppliedCallback_ = std::move(callback);
}

void LicenseManager::setLocalLicenseSyncCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    localLicenseSyncCallback_ = std::move(callback);
}

void LicenseManager::stopCloudSync() {
    if (cloudClient_) cloudClient_->stop();
}

void LicenseManager::completeLocalLicenseSync(bool synced) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        lastImportCloudSyncStatus_ = synced ? 1 : 0;
        if (synced) {
            localLicenseSyncPending_ = false;
            startupLocalSyncRequired_ = false;
        }
    }
    if (synced) reloadLicense(true);
}

void LicenseManager::setLastImportCloudSyncStatus(int status) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    lastImportCloudSyncStatus_ = status;
}

int LicenseManager::getLastImportCloudSyncStatus() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lastImportCloudSyncStatus_;
}

bool LicenseManager::importLicense(const std::string& content, bool allowCloudSync) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        lastImportCloudSyncStatus_ = -1;
    }
    // Validate on a separate instance before touching the current local/server grant.
    LicenseManager candidate;
    if (!candidate.validateLicenseFile(content)) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        setLastError(candidate.getLastError());
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!FileUtils::writeTextFile(licensePath_, content)) {
            setLastError("写入本地授权文件失败");
            return false;
        }
        persistedCloudLicenseHash_.clear();
        // Block an already running cloud callback before releasing the file lock.
        localLicenseSyncPending_ = allowCloudSync;
        saveState();
        const std::string legacyPath = FileUtils::getDirectory(licensePath_) + "/license-cloud.dat";
        if (FileUtils::exists(legacyPath) && !FileUtils::isDirectory(legacyPath) &&
            !FileUtils::removeFile(legacyPath)) {
            LOG_WARN("[License] 本地授权已导入，旧云端文件清理失败");
        }
    }
    if (allowCloudSync) syncLocalLicenseToCloud(content);

    return true;
}

bool LicenseManager::hasValidLocalLicense() const {
    const std::string content = getLocalLicenseContent();
    LicenseManager candidate;
    if (!candidate.validateLicenseFile(content)) return false;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const int64_t now = std::max<int64_t>(std::time(nullptr), lastValidTime_);
    return candidate.licenseHash_ != persistedCloudLicenseHash_ &&
           candidate.startTime_ > 0 && candidate.startTime_ <= now + 300 &&
           candidate.expiryTime_ > now;
}

void LicenseManager::syncLocalLicenseToCloud(const std::string& content) {
    std::function<void(const std::string&)> syncCallback;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        localLicenseSyncPending_ = true;
        lastImportCloudSyncStatus_ = 2;
        applyLocalFallback("本地授权已导入，等待云端同步", 0);
        syncCallback = localLicenseSyncCallback_;
    }
    if (syncCallback) syncCallback(content);
    else {
        stopCloudSync();
        completeLocalLicenseSync(false);
    }
}

void LicenseManager::revokeServerLicense(const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (useCloudPriority_) return;
    clearLicenseState();
    setLastError(reason);
}

bool LicenseManager::authorizeServerLicense(const Json::Value& admission) {
    // Server grants may only affect an explicitly enabled network VOD session.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!admission.isObject() || !admission["contractVersion"].isInt() ||
        admission["contractVersion"].asInt() != 2 ||
        !admission["authorized"].isBool() || !admission["authorized"].asBool() ||
        !admission["issuedAt"].isInt64() || !admission["expiresAt"].isInt64() ||
        !admission["serverTime"].isInt64() || !admission["permanent"].isBool() ||
        !admission["customerName"].isString()) {
        setLastError("服务器授权响应格式无效，请升级服务器并重新签发授权");
        return false;
    }
    const int64_t serverTime = admission["serverTime"].asInt64();
    const int64_t issuedAt = admission["issuedAt"].asInt64();
    const int64_t expiry = admission["expiresAt"].asInt64();
    const bool permanent = admission["permanent"].asBool();
    if (serverTime <= 0 || issuedAt <= 0 || issuedAt > serverTime + 300 ||
        expiry <= issuedAt || (!permanent && expiry <= serverTime)) {
        setLastError("服务器授权未生效或已过期");
        return false;
    }
    // Validate all rights before replacing an existing running grant.
    LicenseManager candidate;
    if (!candidate.loadEntitlements(admission["license"])) {
        setLastError(candidate.getLastError());
        return false;
    }
    clearLicenseState();
    loadEntitlements(admission["license"]);
    licensed_ = true;
    transientOnlineLicense_ = true;
    serverLicensePermanent_ = permanent;
    customerName_ = admission["customerName"].asString();
    supplierName_ = "HVideo";
    licenseInfo_ = customerName_;
    expiryTime_ = expiry;
    startTime_ = issuedAt;
    expiryDateFormatted_ = permanent ? "永久" : formatExpiryDate(expiryTime_);
    totalLicenseDays_ = static_cast<double>(expiry - issuedAt) / 86400.0;
    remainingDays_ = static_cast<double>(expiry - serverTime) / 86400.0;
    daysSource_ = "server";
    initializedFromLicense_ = true;
    deviceTimeValid_ = true;
    cloudServerTime_ = serverTime;
    cloudSyncLocalTime_ = std::time(nullptr);
    serverLicenseReceivedAt_ = std::chrono::steady_clock::now();
    useCloudPriority_ = false;
    warningStage_ = WarningStage::NONE;
    warningTimer_ = 0.0f;
    setLastError("");
    updateWarningState(0.0f);
    return true;
}

bool LicenseManager::reloadLicense(bool allowCloudSync) {
    return checkLicense(allowCloudSync);
}

bool LicenseManager::isModuleEnabled(const std::string& module) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!licensed_) return false;
    for (const auto& m : modules_) {
        if (m == module) return true;
    }
    return false;
}

bool LicenseManager::isLayerEnabled(int layerId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!licensed_) return false;
    for (int id : enabledLayerIds_) {
        if (id == layerId) return true;
    }
    return false;
}

void LicenseManager::update(float deltaTime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (transientOnlineLicense_ && !serverLicensePermanent_) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - serverLicenseReceivedAt_).count();
        remainingDays_ = (static_cast<double>(expiryTime_ - cloudServerTime_) - elapsed) / 86400.0;
    } else if (licensed_ && !serverLicensePermanent_) {
        remainingDays_ -= (static_cast<double>(deltaTime) / 86400.0);
        if (daysSource_ == "cloud") {
            daysSource_ = "offline_state";
        }
        if (cloudManagedLicense_ && remainingDays_ <= 0) {
            applyLocalFallback("云端授权已过期", 0);
        }
    }

    // 更新最后已知正常时间
    int64_t now = std::time(nullptr);
    if (now > lastValidTime_) {
        lastValidTime_ = now;
    }

    // 定期保存判定频率：使用 WARNING_SAVE_PERIOD (300.0f = 5分钟)
    lastSaveTime_ += deltaTime;
    if (lastSaveTime_ > WARNING_SAVE_PERIOD) {
        saveState();
        lastSaveTime_ = 0.0f;
    }

    if (!licensed_ && expiryTime_ <= 0) {
        // [RESIDUE_CLEANUP] 无授权文件时强制 UNLICENSED
        warningStage_ = WarningStage::UNLICENSED;
        showWarning_ = true;
        warningText_ = "VJENGINE";
        return;
    }
    
    // 更新 warning 状态
    updateWarningState(deltaTime);
}

void LicenseManager::loadState() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (statePath_.empty() || !FileUtils::exists(statePath_)) return;

    std::string content = FileUtils::readTextFile(statePath_);
    Json::Value root;
    std::string err;
    if (JsonUtils::parseJson(content, root, err)) {
        remainingDays_ = root.get("remainingDays", 0.0).asDouble();
        totalLicenseDays_ = root.get("totalLicenseDays", 0.0).asDouble();
        lastValidTime_ = root.get("lastValidTime", 0).asInt64();
        lastStateStartTime_ = root.get("startTime", 0).asInt64();
        lastStateExpiryTime_ = root.get("expiryTime", 0).asInt64();
        cloudServerTime_ = root.get("cloudServerTime", 0).asInt64();
        cloudSyncLocalTime_ = root.get("cloudSyncLocalTime", 0).asInt64();
        stateLicenseHash_ = root.get("licenseHash", "").asString();
        persistedCloudLicenseHash_ = root.get("cloudLicenseHash", "").asString();
        daysSource_ = root.get("daysSource", "offline_state").asString();
        initializedFromLicense_ = root.get("initializedFromLicense", false).asBool();
        deviceTimeValid_ = root.get("deviceTimeValid", false).asBool();
        LOG_DEBUG("LicenseManager: loaded state, remaining=%.2f days", remainingDays_);
    }
}

void LicenseManager::saveState() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (statePath_.empty() || transientOnlineLicense_) return;

    Json::Value root;
    root["remainingDays"] = remainingDays_;
    root["totalLicenseDays"] = totalLicenseDays_;
    root["lastValidTime"] = lastValidTime_;
    root["startTime"] = static_cast<Json::Int64>(startTime_);
    root["expiryTime"] = static_cast<Json::Int64>(expiryTime_);
    root["cloudServerTime"] = static_cast<Json::Int64>(cloudServerTime_);
    root["cloudSyncLocalTime"] = static_cast<Json::Int64>(cloudSyncLocalTime_);
    root["licenseHash"] = licenseHash_;
    root["cloudLicenseHash"] = persistedCloudLicenseHash_;
    root["daysSource"] = daysSource_;
    root["initializedFromLicense"] = initializedFromLicense_;
    root["deviceTimeValid"] = deviceTimeValid_;
    root["updatedAt"] = static_cast<Json::Int64>(std::time(nullptr));

    std::string content = JsonUtils::toString(root);
    FileUtils::writeTextFile(statePath_, content);
}

void LicenseManager::applyLocalFallback(const std::string& reason, int64_t serverTime) {
    const auto previousHash = licenseHash_;
    const bool previousCloud = cloudManagedLicense_;
    const bool previousLicensed = licensed_;
    cloudManagedLicense_ = false;
    // A cloud rejection must not reload that same cloud grant as a local backup.
    if (loadLicense() && licenseHash_ != persistedCloudLicenseHash_) {
        refreshLoadedLicense();
        if (serverTime > 0) {
            remainingDays_ = static_cast<double>(expiryTime_ - serverTime) / 86400.0;
            cloudServerTime_ = serverTime;
            cloudSyncLocalTime_ = std::time(nullptr);
            daysSource_ = "cloud";
            deviceTimeValid_ = true;
            updateWarningState(0.0f);
            saveState();
        }
    } else {
        clearLicenseState();
        setLastError(reason + "；本地授权不可用");
    }
    if (previousCloud || previousLicensed != licensed_ || previousHash != licenseHash_) {
        LOG_INFO("[License] %s，已回退本地授权（%s）", reason.c_str(), licensed_ ? "有效" : "不可用");
        if (cloudLicenseAppliedCallback_) cloudLicenseAppliedCallback_();
    }
}

void LicenseManager::onCloudLicenseReceived(bool success, int64_t serverTime, const std::string& content) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!useCloudPriority_ || localLicenseSyncPending_) return;
    if (!success) {
        // A transport/response failure is not a revocation. Keep the already
        // verified cloud grant, including after loading it from disk at boot.
        const int64_t now = std::max<int64_t>(std::time(nullptr), lastValidTime_);
        if (licensed_ && cloudManagedLicense_ && remainingDays_ > 0.0 && expiryTime_ > now) {
            remainingDays_ = std::min(remainingDays_,
                static_cast<double>(expiryTime_ - now) / 86400.0);
            daysSource_ = "offline_state";
            updateWarningState(0.0f);
            saveState();
            LOG_WARN("[License] 云端连接或响应失败，继续使用已验签的本地云端授权缓存");
            return;
        }
        applyLocalFallback("云端连接或响应失败", 0);
        return;
    }
    if (serverTime <= 0 || content.empty()) {
        applyLocalFallback("云端未签发有效授权", serverTime);
        return;
    }
    LicenseManager candidate;
    if (!candidate.validateLicenseFile(content)) {
        LOG_WARN("[License] 云端授权校验失败: %s", candidate.getLastError().c_str());
        applyLocalFallback("云端授权校验失败", serverTime);
        return;
    }
    if (candidate.startTime_ <= 0 || candidate.startTime_ > serverTime + 300 || candidate.expiryTime_ <= serverTime) {
        applyLocalFallback("云端授权未生效或已过期", serverTime);
        return;
    }
    const bool changed = !licensed_ || !cloudManagedLicense_ || candidate.licenseHash_ != licenseHash_;
    if (changed) {
        // Signature, device and validity checks above precede the atomic replace.
        if (licensePath_.empty() || !FileUtils::writeTextFile(licensePath_, content)) {
            setLastError("云端授权写入 license.dat 失败，保留当前授权");
            return;
        }
        if (!validateLicenseFile(content)) {
            applyLocalFallback("云端授权加载失败", serverTime);
            return;
        }
        cloudManagedLicense_ = true;
        persistedCloudLicenseHash_ = licenseHash_;
        transientOnlineLicense_ = false;
        serverLicensePermanent_ = false;
        licensed_ = true;
        warningTimer_ = 0.0f;
        setLastError("");
    }
    remainingDays_ = static_cast<double>(expiryTime_ - serverTime) / 86400.0;
    totalLicenseDays_ = static_cast<double>(expiryTime_ - startTime_) / 86400.0;
    lastStateStartTime_ = startTime_;
    lastStateExpiryTime_ = expiryTime_;
    stateLicenseHash_ = licenseHash_;
    daysSource_ = "cloud";
    initializedFromLicense_ = true;
    deviceTimeValid_ = true;
    cloudServerTime_ = serverTime;
    cloudSyncLocalTime_ = std::time(nullptr);
    updateWarningState(0.0f);
    saveState();
    if (changed) {
        LOG_INFO("[License] 云端优先授权已下发并生效");
        if (cloudLicenseAppliedCallback_) cloudLicenseAppliedCallback_();
    }
}

void LicenseManager::triggerCloudSync() {
    if (cloudClient_) {
        cloudClient_->triggerSync();
    }
}

void LicenseManager::applyBrowserTimeHint(int64_t browserTime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!licensed_ || transientOnlineLicense_ || expiryTime_ <= 0 || browserTime <= 0) return;
    if (startTime_ > 0 && browserTime < startTime_ - 86400) return;

    double browserDays = static_cast<double>(expiryTime_ - browserTime) / 86400.0;
    if (totalLicenseDays_ > 0.0 && browserDays > totalLicenseDays_ + 1.0) return;

    const bool needsBrowserTime =
        daysSource_ == "license_duration" ||
        daysSource_ == "time_invalid" ||
        remainingDays_ <= 0.0 ||
        browserDays < remainingDays_;
    if (needsBrowserTime) {
        remainingDays_ = browserDays;
        daysSource_ = "browser_time";
        saveState();
        updateWarningState(0.0f);
        LOG_INFO("[License] 网页时间辅助校正剩余天数 %.2f", remainingDays_);
    }
}

bool LicenseManager::loadLicense() {
    if (!FileUtils::exists(licensePath_) || FileUtils::isDirectory(licensePath_)) {
        setLastError("授权文件不存在");
        LOG_ERROR("[License] 授权文件不存在");
        return false;
    }

    std::vector<char> rawBytes = FileUtils::readBinaryFile(licensePath_);
    if (rawBytes.empty()) {
        setLastError("授权文件为空或读取失败");
        LOG_WARN("[License] 文件为空或读取失败");
        return false;
    }
    return validateLicenseFile(std::string(rawBytes.begin(), rawBytes.end()));
}

bool LicenseManager::validateLicenseFile(std::string raw) {
    if (raw.empty()) {
        setLastError("授权文件为空");
        return false;
    }
    LOG_WARN("[License] 初始化授权 raw_len=%zu first=0x%02x printable=%c",
             raw.size(), static_cast<unsigned char>(raw[0]),
             (raw[0] >= 32 && raw[0] < 127) ? raw[0] : '.');

    // 去除首尾空白
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' ' || raw.back() == '\t'))
        raw.pop_back();
    size_t start = 0;
    while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t' || raw[start] == '\r' || raw[start] == '\n'))
        start++;
    // 去除 UTF-8 BOM
    bool hadBom = false;
    if (start + 3 <= raw.size() && static_cast<unsigned char>(raw[start]) == 0xEF &&
        static_cast<unsigned char>(raw[start + 1]) == 0xBB && static_cast<unsigned char>(raw[start + 2]) == 0xBF) {
        start += 3;
        hadBom = true;
    }
    if (start > 0 || hadBom)
        raw = raw.substr(start);
    if (hadBom)
        LOG_WARN("[License] 已去除 UTF-8 BOM，trim 后 size=%zu", raw.size());

    if (raw.empty()) {
        setLastError("授权文件去除空白/BOM 后为空");
        LOG_WARN("[License] 去除空白/BOM 后内容为空");
        return false;
    }

    std::string licenseData;
    if (raw[0] == '{') {
        licenseData = raw;
        LOG_WARN("[License] 识别为原始 JSON size=%zu", licenseData.size());
    } else {
        licenseData = licenseBase64Decode(raw);
        while (!licenseData.empty() && (licenseData.back() == '\r' || licenseData.back() == '\n'))
            licenseData.pop_back();
        if (licenseData.empty()) {
            setLastError("授权文件 Base64 解码结果为空");
            LOG_WARN("[License] Base64 解码结果为空 raw_len=%zu，请确认文件为 UTF-8/ASCII 且无非法字符", raw.size());
            return false;
        }
        if (licenseData[0] != '{') {
            setLastError("授权文件 Base64 解码后不是 JSON");
            LOG_WARN("[License] Base64 解码后非 JSON 首字节=0x%02x decoded_size=%zu",
                     static_cast<unsigned char>(licenseData[0]), licenseData.size());
            return false;
        }
    }

    try {
        return validateLicense(licenseData);
    } catch (const std::exception& error) {
        setLastError(std::string("授权字段格式无效: ") + error.what());
        return false;
    }
}

static std::string formatExpiryDate(int64_t expiryTime) {
    if (expiryTime <= 0) return "";
    std::time_t t = static_cast<std::time_t>(expiryTime);
    std::tm value{};
#ifdef _WIN32
    if (gmtime_s(&value, &t) != 0) return "";
#else
    if (!gmtime_r(&t, &value)) return "";
#endif
    const std::tm* utc = &value;
    std::ostringstream oss;
    oss << (utc->tm_year + 1900) << "-"
        << std::setfill('0') << std::setw(2) << (utc->tm_mon + 1) << "-"
        << std::setw(2) << utc->tm_mday;
    return oss.str();
}

static std::string safePrefix(const std::string& s, size_t maxLen) {
    std::string out;
    out.reserve(maxLen + 4);
    for (size_t i = 0; i < s.size() && out.size() < maxLen; i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 32 && c < 127 && c != '\\' && c != '"')
            out += static_cast<char>(c);
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += '.';
    }
    if (s.size() > maxLen) out += "...";
    return out;
}

bool LicenseManager::validateLicense(const std::string& licenseData) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    licenseHash_ = simpleStableHash(licenseData);
    enabledLayerIds_.clear();
    modules_.clear();
    customerName_.clear();
    supplierName_.clear();
    usageMode_ = "buyout";
    expiryDateFormatted_.clear();
    inputChannelCount_ = 0;
    arrearsAmount_ = 0.0;
    paymentQrUrl_.clear();

    Json::Value root;
    std::string err;
    if (!JsonUtils::parseJson(licenseData, root, err)) {
        setLastError("授权 JSON 解析失败: " + err);
        LOG_WARN("[License] JSON 解析失败: %s data_len=%zu prefix=%s",
                 err.c_str(), licenseData.size(), safePrefix(licenseData, 120).c_str());
        return false;
    }
    if (!root.isObject()) {
        setLastError("授权 JSON 根节点不是对象");
        LOG_WARN("[License] 根节点不是 JSON 对象 type=%d data_len=%zu",
                 root.type(), licenseData.size());
        return false;
    }
    int ver = root.get("version", 0).asInt();
    if (ver != 2) {
        setLastError("授权版本不符，请用新版授权工具生成");
        LOG_WARN("[License] 版本不符 version=%d 期望 2，请用 license-tool 生成", ver);
        return false;
    }
    if (!root.isMember("signature")) {
        setLastError("授权缺少 signature 字段");
        LOG_WARN("[License] 缺少 signature 字段，请用 license-tool 生成");
        return false;
    }
    if (!verifyLicenseSignature(root, kLicenseHmacKey)) {
        setLastError("授权签名校验失败，请确认授权工具和设备使用同一 LICENSE_HMAC_KEY");
        LOG_WARN("[License] 签名校验失败（文件可能被篡改或非 license-tool 签发）");
        return false;
    }
    // Device binding is mandatory. An omitted fingerprint would make a
    // signed license portable across every H66_POR device.
    if (!root.isMember("device") || !root["device"].isObject()
        || !root["device"].isMember("fingerprint")
        || !root["device"]["fingerprint"].isString()) {
        setLastError("授权缺少设备指纹，不能用于本机");
        LOG_WARN("[License] 授权缺少 device.fingerprint，拒绝通用授权文件");
        return false;
    }
    std::string boundFp = root["device"]["fingerprint"].asString();
    while (!boundFp.empty() && (boundFp.back() == ' ' || boundFp.back() == '\t' ||
                                boundFp.back() == '\r' || boundFp.back() == '\n')) {
        boundFp.pop_back();
    }
    const std::string currentFp = SystemUtils::generateDeviceFingerprint();
    if (boundFp.empty() || currentFp.empty() || currentFp != boundFp) {
        setLastError("设备指纹不匹配，授权文件不属于本机");
        LOG_WARN("[License] 设备指纹缺失或不匹配，授权与本机绑定不一致");
        return false;
    }
    const Json::Value& customer = root["customer"];
    const Json::Value& license = root["license"];
    const Json::Value& validity = root["validity"];
    if (!license.isObject() || !validity.isObject()) {
        setLastError("授权缺少 license 或 validity 字段");
        LOG_WARN("[License] 缺少必要字段 license.isObject=%d validity.isObject=%d",
                 license.isObject() ? 1 : 0, validity.isObject() ? 1 : 0);
        return false;
    }
    int64_t expiry = validity.get("expiry_time", 0).asInt64();
    if (expiry <= 0) {
        setLastError("授权到期时间缺失或无效");
        LOG_WARN("[License] validity.expiry_time 缺失或无效");
        return false;
    }
    expiryTime_ = static_cast<int64_t>(expiry);
    expiryDateFormatted_ = formatExpiryDate(expiryTime_);
    startTime_ = validity.get("issue_time", 0).asInt64();
    if (startTime_ > 0 && expiryTime_ > startTime_) {
        totalLicenseDays_ = static_cast<double>(expiryTime_ - startTime_) / 86400.0;
    }

    if (customer.isObject() && customer.isMember("name")) {
        customerName_ = customer["name"].asString();
    }
    const Json::Value& supplier = root["supplier"];
    if (supplier.isObject() && supplier.isMember("name") && supplier["name"].isString()) {
        supplierName_ = supplier["name"].asString();
    }
    if (!customerName_.empty()) {
        licenseInfo_ = customerName_;
    } else {
        licenseInfo_ = "HSVJEngine Licensed";
    }

    if (!loadEntitlements(license)) return false;

    int daysUntilExpiry = calculateDaysUntilExpiry();
    if (daysSource_ == "time_invalid" || daysSource_ == "license_duration") {
        warningStage_ = WarningStage::NONE;
        showWarning_ = false;
        warningTimer_ = 0.0f;
        warningText_.clear();
    } else if (daysUntilExpiry <= DAYS_BEFORE_WARNING) {
        warningStage_ = WarningStage::BEFORE_EXPIRY;
        warningTimer_ = WARNING_DURATION;
        showWarning_ = true;
        warningText_ = generateWarningText(daysUntilExpiry);
    } else {
        warningStage_ = WarningStage::NONE;
        showWarning_ = false;
        warningTimer_ = 0.0f;
    }
    setLastError("");
    return true;
}

bool LicenseManager::loadEntitlements(const Json::Value& license) {
    if (!license.isObject() || !license["usage_mode"].isString() ||
        !license["modules"].isArray() ||
        (license.isMember("input_channel_count") && !license["input_channel_count"].isInt())) {
        setLastError("授权模块、商业模式或输入通道数字段无效");
        return false;
    }
    static const std::vector<std::string> supportedModules = {"TV", "投屏", "霸屏", "fusion", "effects", "scene", "cave", "stage", "vod"};
    for (const auto& module : license["modules"]) {
        if (!module.isString() || std::find(supportedModules.begin(), supportedModules.end(), module.asString()) == supportedModules.end()) {
            setLastError("授权包含不支持的功能模块");
            return false;
        }
    }
    modules_.clear();
    enabledLayerIds_.clear();
    inputChannelCount_ = 0;
    if (license.isMember("usage_mode")) {
        usageMode_ = license["usage_mode"].asString();
    }
    if (usageMode_ != "buyout" && usageMode_ != "rent" && usageMode_ != "installment") {
        setLastError("授权商业模式无效: " + usageMode_);
        LOG_WARN("[License] license.usage_mode 无效: %s", usageMode_.c_str());
        return false;
    }
    if (license.isMember("input_channel_count") && license["input_channel_count"].isInt()) {
        inputChannelCount_ = license["input_channel_count"].asInt();
        if (inputChannelCount_ < 0) {
            setLastError("授权输入通道数不能为负数");
            LOG_WARN("[License] license.input_channel_count 无效: %d", inputChannelCount_);
            return false;
        }
    }
    if (license.isMember("arrears_amount") && license["arrears_amount"].isNumeric()) {
        arrearsAmount_ = license["arrears_amount"].asDouble();
    }
    if (license.isMember("payment_qr_url") && license["payment_qr_url"].isString()) {
        paymentQrUrl_ = license["payment_qr_url"].asString();
    }

    const Json::Value& mods = license["modules"];
    if (mods.isArray()) {
        for (Json::ArrayIndex i = 0; i < mods.size(); ++i) {
            if (mods[i].isString()) {
                modules_.push_back(mods[i].asString());
            }
        }
    }
    const Json::Value& layers = license["enabled_layers"];
    if (!layers.isArray() || layers.empty()) {
        setLastError("授权启用图层缺失或为空");
        LOG_WARN("[License] license.enabled_layers 缺失或为空");
        return false;
    }
    for (Json::ArrayIndex i = 0; i < layers.size(); ++i) {
        if (!layers[i].isInt()) {
            setLastError("授权启用图层包含非整数项");
            LOG_WARN("[License] license.enabled_layers[%u] 不是整数", i);
            return false;
        }
        int layerId = layers[i].asInt();
        if (!getLayerDefinition(layerId)) {
            setLastError("授权启用图层包含当前引擎不支持的图层 ID: " + std::to_string(layerId));
            LOG_WARN("[License] license.enabled_layers 包含未知图层 ID: %d", layerId);
            return false;
        }
        if (std::find(enabledLayerIds_.begin(), enabledLayerIds_.end(), layerId) == enabledLayerIds_.end()) {
            enabledLayerIds_.push_back(layerId);
        }
    }

    return true;
}

int LicenseManager::getDaysUntilExpiry() const {
  return calculateDaysUntilExpiry();
}

int LicenseManager::calculateDaysUntilExpiry() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (serverLicensePermanent_) return 36500;
    if (daysSource_ == "offline_state" || daysSource_ == "cloud" ||
        daysSource_ == "browser_time" || daysSource_ == "server") {
        return displayDays(remainingDays_);
    }
    if (daysSource_ == "license_duration") {
        return 0;
    }
    int64_t now = std::time(nullptr);
    if (startTime_ > 0 && now < startTime_ - 3600) {
        bool savedDaysLooksSane = lastStateStartTime_ == startTime_ && lastStateExpiryTime_ == expiryTime_ && remainingDays_ != 0.0 &&
            (totalLicenseDays_ <= 0.0 || remainingDays_ <= totalLicenseDays_ + 1.0);
        if (savedDaysLooksSane) return displayDays(remainingDays_);
        return displayDays(totalLicenseDays_ > 0.0 ? totalLicenseDays_ : remainingDays_);
    }
    if (expiryTime_ <= 0) return static_cast<int>(remainingDays_);
    double days = static_cast<double>(expiryTime_ - now) / 86400.0;
    return displayDays(days);
}

void LicenseManager::updateWarningState(float deltaTime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (transientOnlineLicense_) {
        warningStage_ = (!serverLicensePermanent_ && remainingDays_ <= 0.0)
            ? WarningStage::EXPIRED_15_PLUS : WarningStage::NONE;
        showWarning_ = warningStage_ != WarningStage::NONE;
        warningText_ = showWarning_ ? "服务器授权已过期" : "";
        return;
    }

    if (daysSource_ == "time_invalid" || daysSource_ == "license_duration") {
        warningStage_ = WarningStage::NONE;
        showWarning_ = false;
        warningTimer_ = 0.0f;
        warningText_.clear();
        return;
    }

    // --- [BUG_FIX] 全精度判定核心逻辑 ---
    if (!licensed_) {
        warningStage_ = WarningStage::UNLICENSED;
    } else if (remainingDays_ > (double)DAYS_BEFORE_WARNING) {
        // 正常：超过 15 天
        warningStage_ = WarningStage::NONE;
        showWarning_ = false;
        warningTimer_ = 0.0f;
    } else if (remainingDays_ > 0.0) {
        // 到期提醒：0 - 15 天 (仅启动提示 5 分钟)
        if (warningStage_ != WarningStage::BEFORE_EXPIRY) {
            warningStage_ = WarningStage::BEFORE_EXPIRY;
            warningTimer_ = WARNING_DURATION; // 初始化 300 秒计时
        }
    } else if (remainingDays_ >= (double)-DAYS_EXPIRED_PERMANENT) {
        // 宽限期：过期 0 - 15 天 (永久居中水印)
        warningStage_ = WarningStage::EXPIRED_1_15;
    } else {
        // 强制期：过期 > 15 天 (黑屏 + 永久居中水印)
        warningStage_ = WarningStage::EXPIRED_15_PLUS;
    }
    
    // 更新 warning 状态 based on stage
    switch (warningStage_) {
        case WarningStage::BEFORE_EXPIRY: {
            // 到期前 (DAYS_BEFORE_WARNING=15)，启动后提示显示 WARNING_DURATION (300秒/5分钟)
            if (warningTimer_ > 0.0f) {
                warningTimer_ -= deltaTime;
                showWarning_ = true;
                warningText_ = generateWarningText(static_cast<int>(remainingDays_));
            } else {
                showWarning_ = false;
            }
            break;
        }
        
        case WarningStage::EXPIRED_1_15: {
            // 过期 0-15 天 (宽限期)，永久显示居中水印 (Stage 2)
            showWarning_ = true;
            warningText_ = generateWarningText(static_cast<int>(remainingDays_));
            break;
        }
        
        case WarningStage::EXPIRED_15_PLUS: {
            // 过期超过 15 天 (强制期)，永久显示居中水印 + 黑屏拦截 (Stage 3)
            showWarning_ = true;
            warningText_ = generateWarningText(static_cast<int>(remainingDays_));
            break;
        }
        
        default:
            showWarning_ = false;
            break;
    }
}

std::string LicenseManager::generateWarningText(int daysUntilExpiry) const {
    std::ostringstream oss;
    
    if (daysUntilExpiry > 0) {
        // 说明：过期前
        oss << "License expires in " << daysUntilExpiry << " days, please renew";
    } else if (daysUntilExpiry >= -DAYS_EXPIRED_PERMANENT) {
        // 说明：过期 1-15 天，显示带过期信息的水印
        int daysExpired = -daysUntilExpiry;
        oss << "VJEngine (Expired " << daysExpired << " days)";
    } else {
        // 说明：过期超过 15 天，显示永久水印
        oss << "VJEngine";
    }
    
    return oss.str();
}

bool LicenseManager::shouldBlockVideoPlayback() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return warningStage_ == WarningStage::EXPIRED_15_PLUS || warningStage_ == WarningStage::UNLICENSED;
}

} // 命名空间 hsvj
