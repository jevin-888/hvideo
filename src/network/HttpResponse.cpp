/**
 * @file HttpResponse.cpp（文件名）
 * @brief HTTP响应构建实现
 *
 * 本文件实现了HTTP响应构建类，负责：
 * - HTTP响应状态码和消息设置
 * - HTTP响应头管理
 * - HTTP响应体构建
 * - 文件响应支持
 */

#include "HttpResponse.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

HttpResponse::HttpResponse()
    : statusCode_(200), statusMessage_("OK"), isFileResponse_(false) {
  // 设置默认响应头
  setHeader("Server", "HSVJ HTTP Server");
  setHeader("Content-Type", "text/plain");
  // 轻量服务器每个 accepted socket 只处理一个请求并关闭
  // 响应发送后关闭 socket。声明 keep-alive 会让浏览器
  // 复用即将关闭的 socket，表现为调试 Web UI 偶发
  // fetch 失败。
  setHeader("Connection", "close");
}

HttpResponse::~HttpResponse() {}

void HttpResponse::setStatusCode(int code) {
  statusCode_ = code;
  statusMessage_ = getDefaultStatusMessage(code);
}

int HttpResponse::getStatusCode() const { return statusCode_; }

void HttpResponse::setStatusMessage(const std::string &message) {
  statusMessage_ = message;
}

const std::string &HttpResponse::getStatusMessage() const {
  return statusMessage_;
}

void HttpResponse::setHeader(const std::string &key, const std::string &value) {
  headers_[key] = value;
}

void HttpResponse::setContentType(const std::string &contentType) {
  setHeader("Content-Type", contentType);
}

const std::string &HttpResponse::getHeader(const std::string &key) const {
  static const std::string empty;
  auto it = headers_.find(key);
  return it != headers_.end() ? it->second : empty;
}

const std::unordered_map<std::string, std::string> &
HttpResponse::getHeaders() const {
  return headers_;
}

void HttpResponse::setBody(const std::string &body) {
  body_ = body;
  isFileResponse_ = false;
  setHeader("Content-Length", std::to_string(body_.size()));
}

const std::string &HttpResponse::getBody() const { return body_; }

void HttpResponse::setJson(const std::string &json) {
  setBody(json);
  setHeader("Content-Type", "application/json");
}

void HttpResponse::setFile(const std::string &filePath) {
  filePath_ = filePath;
  isFileResponse_ = true;

  if (fileExists(filePath)) {
    size_t fileSize = fs::file_size(filePath);
    std::string contentType = getMimeType(filePath);
    setHeader("Content-Length", std::to_string(fileSize));
    setHeader("Content-Type", contentType);
    if (contentType.find("application/javascript") != std::string::npos ||
        contentType.find("text/css") != std::string::npos ||
        contentType.find("text/html") != std::string::npos) {
      setHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      setHeader("Pragma", "no-cache");
      setHeader("Expires", "0");
    }
    setStatusCode(200);
    setStatusMessage("OK");
  } else {
    setNotFound();
  }
}

void HttpResponse::setText(const std::string &text) {
  setBody(text);
  setHeader("Content-Type", "text/plain; charset=utf-8");
}

void HttpResponse::setHtml(const std::string &html) {
  setBody(html);
  setHeader("Content-Type", "text/html; charset=utf-8");
}

void HttpResponse::setData(const std::vector<uint8_t> &data) {
  // 将二进制数据转换为 std::string（保留所有字节）
  body_.assign(reinterpret_cast<const char *>(data.data()), data.size());
  isFileResponse_ = false;
  setHeader("Content-Length", std::to_string(data.size()));
}

void HttpResponse::setNotFound() {
  setStatusCode(404);
  setStatusMessage("未找到");
  setBody("404 未找到");
  setHeader("Content-Type", "text/plain");
  isFileResponse_ = false;
}

void HttpResponse::setInternalError() {
  setStatusCode(500);
  setStatusMessage("Internal Server Error");
  setBody("500 Internal Server Error");
  setHeader("Content-Type", "text/plain");
  isFileResponse_ = false;
}

void HttpResponse::setRedirect(const std::string &url) {
  setStatusCode(302);
  setStatusMessage("Found");
  setHeader("Location", url);
  setBody("Redirecting to " + url);
  isFileResponse_ = false;
}

std::vector<char> HttpResponse::build() const {
  std::ostringstream response;

  // 构建响应行
  response << "HTTP/1.1 " << statusCode_ << " " << statusMessage_ << "\r\n";

  // 构建响应头
  for (const auto &header : headers_) {
    response << header.first << ": " << header.second << "\r\n";
  }

  // 空行分隔响应头和响应体
  response << "\r\n";

  std::vector<char> result;

  // 添加响应头和空行到结果
  std::string headersPart = response.str();
  result.insert(result.end(), headersPart.begin(), headersPart.end());

  // 添加响应体到结果
  if (isFileResponse_) {
    if (fileExists(filePath_)) {
      std::vector<char> fileContent = readFile(filePath_);
      result.insert(result.end(), fileContent.begin(), fileContent.end());
    } else {
      // 如果文件不存在，添加404响应体
      result.insert(result.end(), body_.begin(), body_.end());
    }
  } else {
    result.insert(result.end(), body_.begin(), body_.end());
  }

  return result;
}

std::string HttpResponse::getDefaultStatusMessage(int code) const {
  switch (code) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 304:
    return "Not Modified";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "未找到";
  case 405:
    return "Method Not Allowed";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 502:
    return "Bad Gateway";
  case 503:
    return "服务不可用";
  default:
    return "Unknown Status";
  }
}

bool HttpResponse::fileExists(const std::string &path) const {
  return fs::exists(path) && fs::is_regular_file(path);
}

std::vector<char> HttpResponse::readFile(const std::string &path) const {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }

  // 获取文件大小
  file.seekg(0, std::ios::end);
  size_t fileSize = file.tellg();
  file.seekg(0, std::ios::beg);

  // 安全检查：限制读取的文件大小（最大500MB）
  const size_t MAX_READ_SIZE = 500 * 1024 * 1024; // 技术标识：500MB
  if (fileSize > MAX_READ_SIZE) {
    // 文件太大，返回空内容（调用者应该已经检查过文件大小）
    return {};
  }

  // 读取文件内容
  std::vector<char> content(fileSize);
  file.read(content.data(), fileSize);

  return content;
}

std::string HttpResponse::getMimeType(const std::string &path) const {
  std::string extension = path.substr(path.find_last_of(".") + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 ::tolower);

  if (extension == "html" || extension == "htm")
    return "text/html; charset=utf-8";
  if (extension == "css")
    return "text/css";
  if (extension == "js")
    return "application/javascript";
  if (extension == "json")
    return "application/json";
  if (extension == "png")
    return "image/png";
  if (extension == "jpg" || extension == "jpeg")
    return "image/jpeg";
  if (extension == "gif")
    return "image/gif";
  if (extension == "ico")
    return "image/x-icon";
  if (extension == "svg")
    return "image/svg+xml";
  if (extension == "pdf")
    return "application/pdf";
  if (extension == "txt")
    return "text/plain; charset=utf-8";
  if (extension == "mp4")
    return "video/mp4";
  if (extension == "mp3")
    return "audio/mpeg";
  if (extension == "wav")
    return "audio/wav";
  if (extension == "zip")
    return "application/zip";
  if (extension == "rar")
    return "application/x-rar-compressed";
  if (extension == "tar")
    return "application/x-tar";
  if (extension == "gz")
    return "application/gzip";

  return "application/octet-stream";
}
