/**
 * @file HttpRequest.cpp（文件名）
 * @brief HTTP请求解析实现
 *
 * 本文件实现了HTTP请求解析类，负责：
 * - HTTP请求行解析
 * - HTTP请求头解析
 * - HTTP请求体解析
 * - URL参数解析
 */

#include "HttpRequest.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>

HttpRequest::HttpRequest() {
}

HttpRequest::~HttpRequest() {
}

bool HttpRequest::parse(const std::vector<char>& data) {
    std::string request(data.begin(), data.end());
    std::istringstream iss(request);
    std::string line;
    bool parsedRequestLine = false;
    bool parsingHeaders = true;
    std::string headers;
    std::string body;

    // 解析请求行和请求头
    while (std::getline(iss, line)) {
        // 移除可能的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // 空行表示请求头结束，开始处理请求体
        if (line.empty()) {
            parsingHeaders = false;
            break;
        }

        if (!parsedRequestLine) {
            // 解析请求行
            if (!parseRequestLine(line)) {
                return false;
            }
            parsedRequestLine = true;
        } else if (parsingHeaders) {
            // 解析请求头
            if (!parseHeader(line)) {
                return false;
            }
        }
    }

    // 解析请求体
    if (!parsingHeaders) {
        std::ostringstream bodyStream;
        bodyStream << iss.rdbuf();
        body_ = bodyStream.str();
    }

    return parsedRequestLine;
}

const std::string& HttpRequest::getMethod() const {
    return method_;
}

const std::string& HttpRequest::getPath() const {
    return path_;
}

const std::string& HttpRequest::getVersion() const {
    return version_;
}

const std::string& HttpRequest::getHeader(const std::string& key) const {
    static const std::string empty;
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
    auto it = headers_.find(lowerKey);
    return it != headers_.end() ? it->second : empty;
}

const std::unordered_map<std::string, std::string>& HttpRequest::getHeaders() const {
    return headers_;
}

const std::string& HttpRequest::getBody() const {
    return body_;
}

const std::string& HttpRequest::getQueryParam(const std::string& key) const {
    static const std::string empty;
    auto it = query_params_.find(key);
    return it != query_params_.end() ? it->second : empty;
}

const std::unordered_map<std::string, std::string>& HttpRequest::getQueryParams() const {
    return query_params_;
}

const std::string& HttpRequest::getUrlParam(const std::string& key) const {
    static const std::string empty;
    auto it = url_params_.find(key);
    return it != url_params_.end() ? it->second : empty;
}

void HttpRequest::setUrlParam(const std::string& key, const std::string& value) {
    url_params_[key] = value;
}

const std::unordered_map<std::string, std::string>& HttpRequest::getUrlParams() const {
    return url_params_;
}

bool HttpRequest::parseRequestLine(const std::string& line) {
    std::istringstream iss(line);
    iss >> method_ >> path_ >> version_;

    // 转换方法为大写
    std::transform(method_.begin(), method_.end(), method_.begin(), ::toupper);

    // 处理查询参数
    size_t queryPos = path_.find('?');
    if (queryPos != std::string::npos) {
        std::string query = path_.substr(queryPos + 1);
        path_ = path_.substr(0, queryPos);
        parseQueryParams(query);
    }

    return !method_.empty() && !path_.empty() && !version_.empty();
}

bool HttpRequest::parseHeader(const std::string& line) {
    size_t colonPos = line.find(':');
    if (colonPos == std::string::npos) {
        return false;
    }

    std::string key = line.substr(0, colonPos);
    std::string value = line.substr(colonPos + 1);

    // 移除key和value的前后空格
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t") + 1);

    // 转换key为小写
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    headers_[key] = value;
    return true;
}

// URL解码函数
static std::string urlDecode(const std::string& str) {
    std::string decoded;
    decoded.reserve(str.size());
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            // 处理 %XX 格式的编码
            std::string hex = str.substr(i + 1, 2);
            try {
                char decodedChar = static_cast<char>(std::stoi(hex, nullptr, 16));
                decoded += decodedChar;
                i += 2; // 跳过已处理的%XX
            } catch (...) {
                // 如果解析失败，保留原字符
                decoded += str[i];
            }
        } else if (str[i] == '+') {
            // + 号在URL编码中表示空格
            decoded += ' ';
        } else {
            decoded += str[i];
        }
    }
    
    return decoded;
}

bool HttpRequest::parseQueryParams(const std::string& query) {
    std::istringstream iss(query);
    std::string param;

    while (std::getline(iss, param, '&')) {
        size_t equalsPos = param.find('=');
        if (equalsPos != std::string::npos) {
            std::string key = param.substr(0, equalsPos);
            std::string value = param.substr(equalsPos + 1);
            // 对key和value进行URL解码
            query_params_[urlDecode(key)] = urlDecode(value);
        } else if (!param.empty()) {
            // 对key进行URL解码
            query_params_[urlDecode(param)] = "";
        }
    }

    return true;
}