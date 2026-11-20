#include "DisplayOutputModeClient.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace hsvj {

DisplayOutputModeClient::DisplayOutputModeClient(std::string socketPath, int timeoutMs)
    : socketPath_(std::move(socketPath)), timeoutMs_(timeoutMs) {}

bool DisplayOutputModeClient::isValidMode(const std::string& mode) {
    return mode == "hdmi" || mode == "mipi";
}

std::string DisplayOutputModeClient::getMode() const {
    return request("GET\n");
}

std::string DisplayOutputModeClient::setMode(const std::string& mode) const {
    if (!isValidMode(mode)) throw std::invalid_argument("Invalid display output mode");
    const auto saved = request("SET " + mode + "\n");
    if (saved != mode) throw std::runtime_error("Output mode verification failed");
    return saved;
}

std::string DisplayOutputModeClient::request(const std::string& command) const {
#ifdef _WIN32
    (void)command;
    throw std::runtime_error("System output selection is unavailable on this platform");
#else
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath_.empty() || socketPath_.size() >= sizeof(address.sun_path) || timeoutMs_ <= 0)
        throw std::invalid_argument("Invalid output service connection settings");
    std::memcpy(address.sun_path, socketPath_.c_str(), socketPath_.size() + 1);
    struct Socket {
        int fd;
        ~Socket() { if (fd >= 0) close(fd); }
    } socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (socket.fd < 0) throw std::runtime_error("Cannot open output service socket");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
    const auto waitFor = [&](short events) {
        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) throw std::runtime_error("Output service timed out");
            pollfd descriptor{socket.fd, events, 0};
            const int result = poll(&descriptor, 1, static_cast<int>(left));
            if (result > 0) return;
            if (result < 0 && errno == EINTR) continue;
            throw std::runtime_error(result == 0 ? "Output service timed out" : "Output service poll failed");
        }
    };
    if (connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) throw std::runtime_error("System output service unavailable");
        waitFor(POLLOUT);
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
            throw std::runtime_error("Cannot connect to system output service");
    }
    size_t sent = 0;
    while (sent < command.size()) {
        waitFor(POLLOUT);
        const auto count = send(socket.fd, command.data() + sent, command.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) throw std::runtime_error("Output service write failed");
        sent += static_cast<size_t>(count);
    }
    std::string reply;
    while (reply.size() < 32) {
        waitFor(POLLIN);
        char buffer[32];
        const auto count = recv(socket.fd, buffer, sizeof(buffer) - reply.size(), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) throw std::runtime_error("Incomplete output service response");
        reply.append(buffer, static_cast<size_t>(count));
        if (reply.find('\n') == std::string::npos) continue;
        if (reply == "OK hdmi\n") return "hdmi";
        if (reply == "OK mipi\n") return "mipi";
        throw std::runtime_error("Output service rejected request or returned invalid mode");
    }
    throw std::runtime_error("Oversized output service response");
#endif
}

} // namespace hsvj
