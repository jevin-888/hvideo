#ifndef HTTPRESPONSE_H
#define HTTPRESPONSE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class HttpResponse {
public:
  HttpResponse();
  ~HttpResponse();

  // 设置响应状态码
  void setStatusCode(int code);
  // 获取响应状态码
  int getStatusCode() const;
  // 设置响应状态描述
  void setStatusMessage(const std::string &message);
  // 获取响应状态描述
  const std::string &getStatusMessage() const;
  // 设置响应头
  void setHeader(const std::string &key, const std::string &value);
  // 设置Content-Type响应头
  void setContentType(const std::string &contentType);
  // 获取响应头
  const std::string &getHeader(const std::string &key) const;
  // 获取所有响应头
  const std::unordered_map<std::string, std::string> &getHeaders() const;
  // 设置响应体
  void setBody(const std::string &body);
  // 获取响应体
  const std::string &getBody() const;
  // 设置JSON响应
  void setJson(const std::string &json);
  // 设置文件响应
  void setFile(const std::string &filePath);
  // 设置文本响应
  void setText(const std::string &text);
  // 设置HTML响应
  void setHtml(const std::string &html);
  // 设置二进制数据响应
  void setData(const std::vector<uint8_t> &data);
  // 设置404响应
  void setNotFound();
  // 设置500响应
  void setInternalError();
  // 设置重定向响应
  void setRedirect(const std::string &url);
  // 构建响应数据
  std::vector<char> build() const;

private:
  // 获取状态码对应的默认描述
  std::string getDefaultStatusMessage(int code) const;
  // 检查文件是否存在
  bool fileExists(const std::string &path) const;
  // 读取文件内容
  std::vector<char> readFile(const std::string &path) const;
  // 获取文件MIME类型
  std::string getMimeType(const std::string &path) const;

  int statusCode_;                                       // 响应状态码
  std::string statusMessage_;                            // 响应状态描述
  std::unordered_map<std::string, std::string> headers_; // 响应头
  std::string body_;                                     // 响应体
  bool isFileResponse_;                                  // 是否为文件响应
  std::string filePath_;                                 // 文件路径
};

#endif // 结束 HTTPRESPONSE_H