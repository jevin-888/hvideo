#pragma once

#include <string>

namespace hsvj {

// Same init-owned service and protocol used by Android TvSettings.
class DisplayOutputModeClient {
public:
    explicit DisplayOutputModeClient(
        std::string socketPath = "/dev/socket/hsvj_outputd", int timeoutMs = 3000);
    static bool isValidMode(const std::string& mode);
    std::string getMode() const;
    std::string setMode(const std::string& mode) const;

private:
    std::string request(const std::string& command) const;
    std::string socketPath_;
    int timeoutMs_;
};

} // namespace hsvj
