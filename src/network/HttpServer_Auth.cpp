#include "HttpServer_Internal.h"
#include "utils/Logger.h"
#include "utils/JsonUtils.h"
#include "utils/HttpClient.h"
#include "utils/SystemUtils.h"
#include "core/LicenseManager.h"
#include "network/CloudAgentAuth.h"
#include <json/json.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdlib>
#include <unordered_set>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "platform/NetworkHttpBridge.h"
#endif

namespace {

const std::string DEFAULT_USERNAME = "admin";

std::string configuredAdminPassword() {
#ifdef __ANDROID__
    char propertyValue[PROP_VALUE_MAX] = {};
    if (__system_property_get("persist.hsvj.web_password", propertyValue) > 0) {
        return propertyValue;
    }
#endif
    const char *environmentValue = std::getenv("HSVJ_WEB_PASSWORD");
    return environmentValue ? environmentValue : "";
}

// 活跃会话超时（秒）：超过此时间未收到任何 validate/请求，视为掉线
constexpr int SESSION_TIMEOUT_SEC = 120;

// 存储有效的认证令牌
std::unordered_set<std::string> validTokens;
std::mutex tokensMutex;

void setApiSuccess(HttpResponse& response, const Json::Value& data = Json::Value()) {
    Json::Value root(Json::objectValue);
    root["ok"] = true;
    root["data"] = data;
    root["error"] = Json::Value(Json::nullValue);
    response.setStatusCode(200);
    response.setJson(hsvj::JsonUtils::toString(root));
}

void setApiError(HttpResponse& response, int statusCode,
                 const std::string& code, const std::string& message) {
    Json::Value root(Json::objectValue);
    root["ok"] = false;
    root["data"] = Json::Value(Json::nullValue);
    root["error"] = Json::Value(Json::objectValue);
    root["error"]["code"] = code;
    root["error"]["message"] = message;
    response.setStatusCode(statusCode);
    response.setJson(hsvj::JsonUtils::toString(root));
}

/**
 * @brief 生成随机令牌
 */
std::string generateToken() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    for (int i = 0; i < 32; ++i) {
        ss << std::setw(1) << dis(gen);
    }

    return ss.str();
}

/**
 * @brief 验证令牌是否有效
 */
bool validateToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(tokensMutex);
    return validTokens.find(token) != validTokens.end();
}

/**
 * @brief 添加令牌到有效令牌集合
 */
void addToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(tokensMutex);
    validTokens.insert(token);
}

/**
 * @brief 移除令牌
 */
void removeToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(tokensMutex);
    validTokens.erase(token);
}

/**
 * @brief 从请求中提取认证令牌
 */
std::string extractAuthTokenFromReq(const HttpRequest& req) {
    const auto& headers = req.getHeaders();
    // HttpRequest normalizes header names to lowercase while parsing.
    auto authIt = headers.find("authorization");
    if (authIt == headers.end()) {
        // Keep compatibility with programmatically constructed requests.
        authIt = headers.find("Authorization");
    }
    if (authIt != headers.end()) {
        const std::string& authHeader = authIt->second;
        if (authHeader.size() >= 7 &&
            (authHeader.compare(0, 7, "Bearer ") == 0 ||
             authHeader.compare(0, 7, "bearer ") == 0)) {
            return authHeader.substr(7);
        }
    }

    const auto& queryParams = req.getQueryParams();
    auto tokenIt = queryParams.find("token");
    if (tokenIt != queryParams.end()) {
        return tokenIt->second;
    }

    return "";
}

} // 匿名命名空间

/**
 * @brief 注册认证相关路由
 */
void HttpServer::Impl::registerAuthRoutes(std::function<std::string()> cloudServerUrl) {
    get("/api/v1/auth/config", [](const HttpRequest&, HttpResponse& resp) {
        Json::Value data(Json::objectValue);
        data["localPasswordConfigured"] = !configuredAdminPassword().empty();
        data["cloudAgentLoginEnabled"] = true;
        resp.setHeader("Cache-Control", "no-store");
        setApiSuccess(resp, data);
    });

    // POST /api/v1/auth/login - 登录
    post("/api/v1/auth/login", [this, cloudServerUrl](const HttpRequest& req, HttpResponse& resp) {
        try {
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(req.getBody(), root)) {
                setApiError(resp, 400, "BAD_REQUEST", "Invalid JSON");
                return;
            }

            if (!root.isObject() || !root["username"].isString() || !root["password"].isString() ||
                (root.isMember("loginType") && !root["loginType"].isString())) {
                setApiError(resp, 400, "BAD_REQUEST", "账号、密码和登录方式格式不正确");
                return;
            }
            const std::string username = root["username"].asString();
            const std::string password = root["password"].asString();
            const std::string loginType = root.get("loginType", "local").asString();
            if (username.empty() || username.size() > 256 || password.empty() || password.size() > 1024 ||
                (loginType != "local" && loginType != "cloud")) {
                setApiError(resp, 400, "BAD_REQUEST", "请输入有效账号、密码和登录方式");
                return;
            }

            std::string expectedPassword = configuredAdminPassword();
            if (expectedPassword.empty()) {
                expectedPassword = "admin*";
            }
            if (loginType == "local" && (username != DEFAULT_USERNAME || password != expectedPassword)) {
                setApiError(resp, 401, "UNAUTHORIZED",
                            "本地账号或密码错误");
                return;
            }
            if (loginType == "cloud") {
                std::string base = cloudServerUrl();
                if (base.empty()) {
                    base = "http://" + std::string(hsvj::LicenseManager::DEFAULT_CLOUD_HOST) + ":" +
                           std::to_string(hsvj::LicenseManager::DEFAULT_CLOUD_PORT);
                }
                const auto result = hsvj::authenticateCloudAgent(
                    base, username, password, hsvj::SystemUtils::generateDeviceFingerprint(),
                    [](const std::string& method, const std::string& url,
                       const std::string& body, const std::string& token) {
                        hsvj::CloudAuthReply reply;
#ifdef __ANDROID__
                        Json::Value envelope;
                        std::string errors;
                        if (hsvj::JsonUtils::parseJson(
                                callJavaHttpRequestAcrossNetworks(method, url, body, token, 5000), envelope, errors) &&
                            envelope["status"].isInt() && envelope["body"].isString()) {
                            reply.status = envelope["status"].asInt();
                            reply.body = envelope["body"].asString();
                        }
#else
                        reply.body = method == "POST"
                            ? hsvj::httpPostJson(url, body, 5, token, &reply.status)
                            : hsvj::httpGet(url, 5, token, &reply.status);
#endif
                        return reply;
                    });
                if (!result.authenticated()) {
                    setApiError(resp, result.status, result.code, result.message);
                    return;
                }
            }

            // 获取请求方 IP
            std::string peerIp = req.getPeerIp();

            // 检查是否已有活跃会话
            {
                std::lock_guard<std::mutex> lock(activeSessionMutex_);
                if (!activeSessionToken_.empty()) {
                    // 检查会话是否超时
                    auto now = std::chrono::steady_clock::now();
                    bool expired = activeSessionLastSeen_.time_since_epoch().count() != 0 &&
                        now - activeSessionLastSeen_ >= std::chrono::seconds(SESSION_TIMEOUT_SEC);
                    bool sameIp = !activeSessionIp_.empty() && activeSessionIp_ == peerIp;

                    if (!expired && !sameIp) {
                        // 不同 IP 已有人在线，拒绝登录并提示
                        setApiError(resp, 409, "SESSION_ACTIVE",
                                    "已有用户在线 (" + activeSessionIp_ +
                                        ")，请等待对方退出后再试");
                        LOG_INFO("Login rejected: session active from %s, new attempt from %s",
                                 activeSessionIp_.c_str(), peerIp.c_str());
                        return;
                    }

                    // 同一 IP 重新登录视为替换自己的旧会话；超时会话也直接清理。
                    if (sameIp && !expired) {
                        LOG_INFO("Replacing active session from same IP %s", peerIp.c_str());
                    } else {
                        LOG_INFO("Previous session from %s expired, allowing new login from %s",
                                 activeSessionIp_.c_str(), peerIp.c_str());
                    }
                    removeToken(activeSessionToken_);
                    activeSessionToken_.clear();
                    activeSessionIp_.clear();
                    activeSessionLastSeen_ = {};
                }

                // 创建新会话
                std::string token = generateToken();
                addToken(token);
                activeSessionToken_ = token;
                activeSessionIp_ = peerIp;
                activeSessionLastSeen_ = std::chrono::steady_clock::now();

                Json::Value data(Json::objectValue);
                data["token"] = token;
                data["expiresIn"] = 2 * 60 * 60 * 1000; // 2小时
                setApiSuccess(resp, data);
                LOG_INFO("User logged in: %s from %s", username.c_str(), peerIp.c_str());
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Login error: %s", e.what());
            setApiError(resp, 500, "INTERNAL_ERROR", "Internal server error");
        }
    });

    // POST /api/v1/auth/logout - 登出
    post("/api/v1/auth/logout", [this](const HttpRequest& req, HttpResponse& resp) {
        try {
            std::string token = extractAuthTokenFromReq(req);

            if (!token.empty()) {
                // Keep the same active-session -> token lock order as login.
                std::lock_guard<std::mutex> lock(activeSessionMutex_);
                removeToken(token);
                if (activeSessionToken_ == token) {
                    LOG_INFO("Active session logged out from %s", activeSessionIp_.c_str());
                    activeSessionToken_.clear();
                    activeSessionIp_.clear();
                    activeSessionLastSeen_ = {};
                }
            }

            setApiSuccess(resp, Json::Value(Json::nullValue));
        } catch (const std::exception& e) {
            LOG_ERROR("Logout error: %s", e.what());
            setApiError(resp, 500, "INTERNAL_ERROR", "Internal server error");
        }
    });

    // GET /api/v1/auth/validate - 验证令牌
    get("/api/v1/auth/validate", [this](const HttpRequest& req, HttpResponse& resp) {
        try {
            std::string token = extractAuthTokenFromReq(req);

            bool valid = false;
            {
                std::lock_guard<std::mutex> lock(activeSessionMutex_);
                const auto now = std::chrono::steady_clock::now();
                if (activeSessionToken_ == token &&
                    activeSessionLastSeen_.time_since_epoch().count() != 0 &&
                    now - activeSessionLastSeen_ < std::chrono::seconds(SESSION_TIMEOUT_SEC)) {
                    valid = validateToken(token);
                    if (valid) activeSessionLastSeen_ = now;
                }
            }

            if (valid) {

                Json::Value data(Json::objectValue);
                data["valid"] = true;
                setApiSuccess(resp, data);
            } else {
                setApiError(resp, 401, "UNAUTHORIZED",
                            "Invalid or expired token");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Token validation error: %s", e.what());
            setApiError(resp, 500, "INTERNAL_ERROR", "Internal server error");
        }
    });

    LOG_DEBUG("Auth routes registered");
}

/**
 * @brief 检查请求是否已认证
 */
bool HttpServer::Impl::isAuthenticated(const HttpRequest& req) {
    // 8081 是局域网免登录手机控制端；普通 API 与流式请求共用此入口。
    if (port_ == 8081) return true;

    const std::string token = extractAuthTokenFromReq(req);
    if (token.empty()) {
        return false;
    }

    // Check the server's active session before touching the global token set.
    // Login takes the same lock first, preventing a lock-order inversion.
    {
        std::lock_guard<std::mutex> lock(activeSessionMutex_);
        if (activeSessionToken_ != token) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (activeSessionLastSeen_.time_since_epoch().count() == 0 ||
            now - activeSessionLastSeen_ >= std::chrono::seconds(SESSION_TIMEOUT_SEC)) {
            return false;
        }
        activeSessionLastSeen_ = now;
    }
    return validateToken(token);
}

bool HttpServer::Impl::isVodClientAuthenticated(const HttpRequest& req) const {
    // The VOD/KTV client protocol has no session-cookie exchange.  These
    // compatibility headers are the existing one-to-one contract used by the
    // pad client and are now enforced for every 9898 stream handoff too.
    return req.getHeader("_uname") == "5" && req.getHeader("clientid") == "8";
}

/**
 * @brief 从请求中提取认证令牌（供外部使用）
 */
std::string HttpServer::Impl::extractAuthToken(const HttpRequest& req) {
    return extractAuthTokenFromReq(req);
}
