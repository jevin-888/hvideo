#include "HttpServer_Internal.h"
#include "utils/Logger.h"
#include "utils/JsonUtils.h"
#include <json/json.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <unordered_set>

namespace {

// 默认用户名和密码
const std::string DEFAULT_USERNAME = "admin";
const std::string DEFAULT_PASSWORD = "admin";

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
    auto authIt = headers.find("Authorization");
    if (authIt != headers.end()) {
        const std::string& authHeader = authIt->second;
        if (authHeader.substr(0, 7) == "Bearer ") {
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
void HttpServer::Impl::registerAuthRoutes() {
    // POST /api/v1/auth/login - 登录
    post("/api/v1/auth/login", [this](const HttpRequest& req, HttpResponse& resp) {
        try {
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(req.getBody(), root)) {
                setApiError(resp, 400, "BAD_REQUEST", "Invalid JSON");
                return;
            }

            std::string username = root.get("username", "").asString();
            std::string password = root.get("password", "").asString();

            if (username != DEFAULT_USERNAME || password != DEFAULT_PASSWORD) {
                setApiError(resp, 401, "UNAUTHORIZED",
                            "Invalid username or password");
                LOG_WARN("Failed login attempt for user: %s", username.c_str());
                return;
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
                removeToken(token);

                // 清除活跃会话
                std::lock_guard<std::mutex> lock(activeSessionMutex_);
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

            if (validateToken(token)) {
                // 刷新活跃时间
                {
                    std::lock_guard<std::mutex> lock(activeSessionMutex_);
                    if (activeSessionToken_ == token) {
                        activeSessionLastSeen_ = std::chrono::steady_clock::now();
                    }
                }

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
    return true;
}

/**
 * @brief 从请求中提取认证令牌（供外部使用）
 */
std::string HttpServer::Impl::extractAuthToken(const HttpRequest& req) {
    return extractAuthTokenFromReq(req);
}
