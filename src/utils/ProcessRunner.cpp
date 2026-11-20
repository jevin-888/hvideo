#include "utils/ProcessRunner.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hsvj {

ProcessResult runShellCommand(const std::string& command,
                              std::chrono::milliseconds timeout,
                              const std::atomic<bool>* cancel,
                              size_t maxOutputBytes) {
    ProcessResult result;
    if (timeout.count() <= 0 || command.find('\0') != std::string::npos) return result;
    if (cancel && cancel->load()) {
        result.cancelled = true;
        return result;
    }
#ifdef _WIN32
    (void)maxOutputBytes;
    result.output = "Shell execution requires the Android/Linux runtime";
    return result;
#else
    if (maxOutputBytes == 0) return result;
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) != 0) return result;
    const int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return result;
    }
    // Keep all owned descriptors above stderr even when the caller closed one
    // of its standard descriptors before invoking us.
    auto moveAboveStdio = [](int fd) {
        if (fd > STDERR_FILENO) return fd;
        const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        return replacement;
    };
    pipeFds[0] = moveAboveStdio(pipeFds[0]);
    pipeFds[1] = moveAboveStdio(pipeFds[1]);
    const int inputFd = moveAboveStdio(input);
    if (pipeFds[0] < 0 || pipeFds[1] < 0 || inputFd < 0 ||
        fcntl(pipeFds[0], F_SETFL, O_NONBLOCK) < 0) {
        if (pipeFds[0] >= 0) close(pipeFds[0]);
        if (pipeFds[1] >= 0) close(pipeFds[1]);
        if (inputFd >= 0) close(inputFd);
        return result;
    }
    // Prepare all C++ storage before fork; the child only uses async-signal-safe
    // functions because the engine may have many other threads holding locks.
    const char* text = command.c_str();
#ifdef __ANDROID__
    const char* shell = "/system/bin/sh";
#else
    const char* shell = "/bin/sh";
#endif
    const pid_t pid = fork();
    if (pid == 0) {
        if (setpgid(0, 0) != 0 || dup2(inputFd, STDIN_FILENO) < 0 ||
            dup2(pipeFds[1], STDOUT_FILENO) < 0 || dup2(pipeFds[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(inputFd);
        close(pipeFds[0]);
        close(pipeFds[1]);
        execl(shell, "sh", "-c", text, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(inputFd);
    close(pipeFds[1]);
    if (pid < 0) {
        close(pipeFds[0]);
        return result;
    }
    setpgid(pid, pid);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    bool exited = false;
    bool readError = false;
    // Stop draining after one bounded batch so an output flood cannot starve
    // the timeout/cancellation checks.
    auto drain = [&] {
        char buffer[4096];
        for (int i = 0; i < 16; ++i) {
            const ssize_t n = read(pipeFds[0], buffer, sizeof(buffer));
            if (n <= 0) {
                readError = n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
                break;
            }
            const size_t available = maxOutputBytes - result.output.size();
            result.output.append(buffer, std::min(available, static_cast<size_t>(n)));
            if (static_cast<size_t>(n) > available) {
                result.outputLimitExceeded = true;
                break;
            }
        }
    };
    try {
        while (true) {
            drain();
            result.cancelled = cancel && cancel->load();
            result.timedOut = std::chrono::steady_clock::now() >= deadline;
            if (result.cancelled || result.timedOut || result.outputLimitExceeded || readError) break;
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                exited = true;
                drain();
                break;
            }
            if (waited < 0 && errno != EINTR) break;
            pollfd event{pipeFds[0], POLLIN, 0};
            // A descendant may retain stdout after the shell exits. Never wait
            // for EOF from such descendants before observing the shell's status.
            if (poll(&event, 1, 25) > 0 && (event.revents & POLLHUP)) usleep(1000);
        }
    } catch (...) {
        if (!exited) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        close(pipeFds[0]);
        throw;
    }
    if (!exited) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    }
    close(pipeFds[0]);
    return result;
#endif
}

} // namespace hsvj
