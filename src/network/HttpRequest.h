#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H

#include <string>
#include <unordered_map>
#include <vector>

class HttpRequest {
public:
    HttpRequest();
    ~HttpRequest();

    // 解析HTTP请求
    bool parse(const std::vector<char>& data);
    
    // 获取请求方法
    const std::string& getMethod() const;
    // 获取请求路径
    const std::string& getPath() const;
    // 获取请求版本
    const std::string& getVersion() const;
    // 获取请求头
    const std::string& getHeader(const std::string& key) const;
    // 获取所有请求头
    const std::unordered_map<std::string, std::string>& getHeaders() const;
    // 获取请求体
    const std::string& getBody() const;
    // 获取查询参数
    const std::string& getQueryParam(const std::string& key) const;
    // 获取所有查询参数
    const std::unordered_map<std::string, std::string>& getQueryParams() const;
    // 获取URL参数（如/api/scenes/{name}中的name）
    const std::string& getUrlParam(const std::string& key) const;
    // 设置URL参数
    void setUrlParam(const std::string& key, const std::string& value);
    // 获取所有URL参数
    const std::unordered_map<std::string, std::string>& getUrlParams() const;

    // 获取/设置客户端IP
    const std::string& getPeerIp() const { return peerIp_; }
    void setPeerIp(const std::string& ip) { peerIp_ = ip; }

private:
    // 解析请求行
    bool parseRequestLine(const std::string& line);
    // 解析请求头
    bool parseHeader(const std::string& line);
    // 解析查询参数
    bool parseQueryParams(const std::string& query);

    std::string method_;          // 请求方法（GET, POST, PUT, DELETE等）
    std::string path_;            // 请求路径
    std::string version_;         // HTTP版本
    std::string body_;            // 请求体
    std::unordered_map<std::string, std::string> headers_;       // 请求头
    std::unordered_map<std::string, std::string> query_params_;  // 查询参数
    std::unordered_map<std::string, std::string> url_params_;    // URL参数
    std::string peerIp_;             // 客户端IP
};

#endif // 结束 HTTPREQUEST_H