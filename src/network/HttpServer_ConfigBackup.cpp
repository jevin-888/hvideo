#include "HttpServer_Internal.h"
#include "platform/ConfigBackupBridge.h"
#include "network/DeviceFileSync.h"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <poll.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
std::mutex backupMutex; // Shared by 8080/8081/9898 server instances.
constexpr uint64_t kMaxZipBytes = 8ULL * 1024 * 1024 * 1024;

struct Workspace {
  std::filesystem::path path;
  ~Workspace() {
    std::error_code error;
    if (!path.empty() && !std::filesystem::exists(path / "KEEP", error))
      std::filesystem::remove_all(path, error);
  }
};

bool sendChunk(int socket, const char* data, size_t size, const std::atomic<bool>& running) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (size > 0 && running.load()) {
    ssize_t sent = send(socket, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent > 0) {
      data += sent;
      size -= static_cast<size_t>(sent);
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    } else if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      pollfd descriptor{socket, POLLOUT, 0};
      if (poll(&descriptor, 1, 250) < 0 && errno != EINTR) return false;
    } else return false;
  }
  return size == 0;
}
}

void HttpServer::Impl::handleConfigBackup(int clientSocket, const HttpRequest& request,
                                        const std::vector<char>& initialData, size_t headerEndPos) {
  const auto reply = [&](int status, const std::string& message, int files = 0) {
    Json::Value envelope(Json::objectValue);
    envelope["ok"] = status == 200;
    envelope["data"] = Json::Value(Json::nullValue);
    envelope["error"] = Json::Value(Json::nullValue);
    if (status == 200) {
      envelope["data"]["files"] = files;
      envelope["data"]["restart_required"] = true;
      envelope["data"]["restore_pending"] = true;
    } else {
      envelope["error"]["code"] = "CONFIG_BACKUP_FAILED";
      envelope["error"]["message"] = message;
    }
    HttpResponse response;
    response.setStatusCode(status);
    response.setHeader("Cache-Control", "no-store");
    response.setJson(hsvj::JsonUtils::toString(envelope));
    sendResponseData(clientSocket, response.build());
  };
#ifndef __ANDROID__
  (void)request; (void)initialData; (void)headerEndPos;
  reply(503, "Configuration ZIP backup requires Android");
#else
  std::unique_lock<std::mutex> lock(backupMutex, std::try_to_lock);
  if (!lock.owns_lock()) { reply(409, "正在导入或导出配置，请稍后再试"); return; }
  Workspace work;
  bool headersSent = false;
  try {
    if (hsvj::ROOT_PATH.empty()) { reply(503, "数据目录尚未就绪"); return; }
    auto root = std::filesystem::canonical(hsvj::ROOT_PATH);
    std::string guardError;
    hsvj::DeviceDataGuard dataGuard(root.string(), hsvj::DeviceDataGuard::Purpose::Backup, guardError);
    if (!dataGuard.acquired()) { reply(409, guardError); return; }
    if (std::filesystem::exists(root / ".hsvj-backup-pending")) {
      reply(409, "已有已校验的备份等待完整还原，请先重启播放器"); return;
    }
    std::string pattern = (root / ".hsvj-backup-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    char* created = mkdtemp(name.data());
    if (!created) { reply(507, "无法创建备份临时目录，请检查剩余空间"); return; }
    work.path = created;
    const auto zipPath = work.path / "config.zip";
    const bool importing = request.getMethod() == "POST";
    if (importing) {
      timeval receiveTimeout{60, 0};
      setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
      size_t length = 0;
      if (!request.getContentLength(length) || length == 0 || length > kMaxZipBytes) {
        reply(413, "请选择不超过 8 GB 的 ZIP 压缩包"); return;
      }
      if (request.getHeader("Content-Type").find("application/zip") != 0) {
        reply(415, "请直接上传 ZIP 压缩包"); return;
      }
      struct statvfs space{};
      if (statvfs(root.c_str(), &space) != 0 ||
          uint64_t(space.f_bavail) * space.f_frsize < length + 16ULL * 1024 * 1024) {
        reply(507, "设备剩余空间不足"); return;
      }
      std::ofstream output(zipPath, std::ios::binary);
      if (!output) { reply(507, "无法写入备份压缩包"); return; }
      size_t received = initialData.size() >= headerEndPos ? initialData.size() - headerEndPos : 0;
      if (received > length) { reply(400, "上传长度不正确"); return; }
      output.write(initialData.data() + headerEndPos, static_cast<std::streamsize>(received));
      std::array<char, 128 * 1024> buffer{};
      while (received < length && isRunning_.load()) {
        ssize_t count = recv(clientSocket, buffer.data(), std::min(buffer.size(), length - received), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { reply(400, "压缩包上传中断，请重新导入"); return; }
        output.write(buffer.data(), count);
        if (!output) { reply(507, "压缩包写入失败，请检查剩余空间"); return; }
        received += static_cast<size_t>(count);
      }
      output.close();
      if (received != length || !output) { reply(400, "压缩包上传不完整"); return; }
    }
    const auto raw = callJavaConfigBackup(importing ? "import" : "export", root.string(), work.path.string());
    Json::Value result;
    std::string errors;
    if (!hsvj::JsonUtils::parseJson(raw, result, errors) || !result["ok"].asBool()) {
      reply(importing ? 422 : 500, result.get("message", "配置备份处理失败").asString()); return;
    }
    if (importing) { reply(200, "", result["files"].asInt()); return; }
    const auto bytes = std::filesystem::file_size(zipPath);
    if (bytes > kMaxZipBytes) { reply(413, "生成的压缩包超过 8 GB，暂不支持"); return; }
    const auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
           << "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " << bytes
           << "\r\nContent-Disposition: attachment; filename=\"hsvj-config-" << timestamp << ".zip\"\r\n\r\n";
    const auto text = header.str();
    headersSent = true;
    if (!sendChunk(clientSocket, text.data(), text.size(), isRunning_)) return;
    std::ifstream input(zipPath, std::ios::binary);
    std::array<char, 128 * 1024> buffer{};
    while (input) {
      input.read(buffer.data(), buffer.size());
      if (input.gcount() && !sendChunk(clientSocket, buffer.data(), static_cast<size_t>(input.gcount()), isRunning_)) return;
    }
  } catch (const std::exception& error) {
    LOG_ERROR("Config backup failed: %s", error.what());
    if (!headersSent) reply(500, std::string("配置备份处理异常：") + error.what());
  }
#endif
}
