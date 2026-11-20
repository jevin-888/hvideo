#include "foundation/PathConfig.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <regex>

namespace hsvj {

void updateApiConfigIp() {
    std::string localIp = SystemUtils::getLocalIp();
    if (localIp.empty()) {
        LOG_DEBUG("无法获取本机 IP，跳过 apiConfig.js 更新");
        return;
    }
    std::string sharedDir = SHARED_WEB_DIR;
    if (!sharedDir.empty() && sharedDir.back() != '/' && sharedDir.back() != '\\') {
        sharedDir += '/';
    }
    std::string apiConfigPath = sharedDir + "config/apiConfig.js";
    if (!FileUtils::exists(apiConfigPath)) {
        LOG_DEBUG("apiConfig.js 不存在: %s", apiConfigPath.c_str());
        return;
    }

    std::string content;
    if (!FileUtils::readFile(apiConfigPath, content)) {
        LOG_WARN("读取 apiConfig.js 失败: %s", apiConfigPath.c_str());
        return;
    }

    std::regex hostRegex(R"((https?://)(\d{1,3}(?:\.\d{1,3}){3}|localhost)(:\d+))");
    std::string updated = std::regex_replace(content, hostRegex, std::string("$1") + localIp + "$3");
    if (updated != content) {
        if (FileUtils::writeTextFile(apiConfigPath, updated)) {
            LOG_INFO("已更新 apiConfig.js 默认 IP 为: %s", localIp.c_str());
        } else {
            LOG_WARN("写入 apiConfig.js 失败: %s", apiConfigPath.c_str());
        }
    } else {
        LOG_DEBUG("apiConfig.js 中未找到可替换的默认 IP");
    }
}

} // namespace hsvj
