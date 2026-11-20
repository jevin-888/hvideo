#pragma once

#include <atomic>
#include <chrono>
#include <string>

namespace hsvj {

struct ProcessResult {
    int exitCode = -1;
    bool timedOut = false;
    bool cancelled = false;
    bool outputLimitExceeded = false;
    std::string output;

    bool succeeded() const {
        return exitCode == 0 && !timedOut && !cancelled && !outputLimitExceeded;
    }
};

// POSIX shell execution for the Android/Linux runtime. Output and duration are
// bounded; cancellation terminates the command's process group.
ProcessResult runShellCommand(const std::string& command,
                              std::chrono::milliseconds timeout,
                              const std::atomic<bool>* cancel = nullptr,
                              size_t maxOutputBytes = 64 * 1024);

} // namespace hsvj
