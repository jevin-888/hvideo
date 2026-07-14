#ifndef HSVJ_LICENSE_VERIFY_H
#define HSVJ_LICENSE_VERIFY_H

#include <string>

namespace Json {
class Value;
}

namespace hsvj {

/** Base64 解码，用于解析混淆后的授权文件 */
std::string licenseBase64Decode(const std::string& encoded);

/**
 * 校验授权 JSON 的 HMAC 签名（与 license-tool 生成时一致）
 * @param root 解析后的 JSON 根（含 signature 字段）
 * @param hmacKey 与生成端相同的密钥
 * @return 校验是否通过
 */
bool verifyLicenseSignature(const Json::Value& root, const std::string& hmacKey);

}  // 命名空间 hsvj

#endif
