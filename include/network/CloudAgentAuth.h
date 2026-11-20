#pragma once

#include <functional>
#include <string>

namespace hsvj {

struct CloudAuthReply {
  int status = 0;
  std::string body;
};

// Injectable transport keeps the cloud contract testable without real credentials.
using CloudAuthRequest = std::function<CloudAuthReply(
    const std::string& method, const std::string& url,
    const std::string& body, const std::string& bearerToken)>;

struct CloudAuthResult {
  int status = 503;
  std::string code = "CLOUD_UNAVAILABLE";
  std::string message = "云端认证服务暂不可用，请稍后重试";
  bool authenticated() const { return status == 200; }
};

CloudAuthResult authenticateCloudAgent(const std::string& baseUrl,
                                      const std::string& username,
                                      const std::string& password,
                                      const std::string& fingerprint,
                                      const CloudAuthRequest& request);

} // namespace hsvj
