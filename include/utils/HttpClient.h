/**
 * @file HttpClient.h（文件名）
 * @brief 简易 HTTP 客户端（仅用于进程内 POST JSON，不依赖临时文件或 curl）
 */

#ifndef HSVJ_HTTP_CLIENT_H
#define HSVJ_HTTP_CLIENT_H

#include <string>
#include <functional>

namespace hsvj {

/**
 * 向指定 URL 发送 HTTP POST，Body 为 JSON 字符串。
 * 仅支持 HTTP（不支持 HTTPS），适用于内网授权服务上报。
 * @param url 完整 URL，如 "http://60.205.127.117:8080/api/device/register"
 * @param jsonBody 请求体（JSON 字符串）
 * @param timeoutSeconds 连接与读取超时（秒），默认 15
 * @param bearerToken 可选 Bearer 令牌，不记录或持久化
 * @param statusCode 可选 HTTP 状态码输出，传输失败为 0
 * @return 响应体；失败或超时返回空字符串
 */
std::string httpPostJson(const std::string& url, const std::string& jsonBody, int timeoutSeconds = 15,
                         const std::string& bearerToken = "", int* statusCode = nullptr);

/** Build the JSON after connecting, using the TCP socket's real local IP.
 * An empty body aborts before any HTTP bytes are sent. */
std::string httpPostJson(const std::string& url,
                         const std::function<std::string(const std::string&)>& bodyForLocalIp,
                         int timeoutSeconds = 15, const std::string& bearerToken = "",
                         int* statusCode = nullptr);

/**
 * 向指定 URL 发送 HTTP GET。
 * @param url 完整 URL，如 "http://192.168.1.2:8080/api/system/device-info"
 * @param timeoutSeconds 连接与读取超时（秒）
 * @param bearerToken 可选 Bearer 令牌，不记录或持久化
 * @param statusCode 可选 HTTP 状态码输出，传输失败为 0
 * @return 响应体；失败或超时返回空字符串
 */
std::string httpGet(const std::string& url, int timeoutSeconds = 2,
                    const std::string& bearerToken = "", int* statusCode = nullptr);

/**
 * 向指定 URL 发送 HTTP HEAD，只读响应头，不读 body。
 * 适合检查文件是否存在（200/206=存在，404=不存在）。
 * @param url 完整 URL
 * @param 时间outSeconds 超时（秒）
 * @return HTTP 状态码（如 200、404）；连接失败返回 -1
 */
int httpHead(const std::string& url, int timeoutSeconds = 3);

/**
 * 将指定 URL 下载到本地文件（流式写入，不加载到内存）。
 * @param url 完整 URL
 * @param filePath 本地文件路径
 * @param 时间outSeconds 超时（秒）
 * @return 是否下载成功
 */
bool httpDownloadStreaming(const std::string& url, const std::string& filePath, int timeoutSeconds = 60);

} // 命名空间 hsvj

#endif
