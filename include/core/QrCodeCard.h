#pragma once

#include "core/SystemConfig.h"
#include <string>

namespace hsvj {
// Only the VOD mobile route is device-bound. Custom QR contents stay literal.
bool isVodMobileQr(const std::string& content);
std::string resolveVodMobileQr(const std::string& content, bool onlineVod,
                              const std::string& serverBase,
                              const std::string& localIp);
bool writeQrCodeCard(const LayerConfigData& config, const std::string& fontPath,
                     const std::string& outputPath);
} // namespace hsvj
