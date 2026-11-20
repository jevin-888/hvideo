#include "foundation/VideoStorage.h"
#include "HttpServer_Internal.h"
#include "database/MaterialIndex.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/SystemUtils.h"
#include "foundation/PathConfig.h"
#include "database/PlaylistManager.h"
#include "network/PreviewStreamConverter.h"
#include "decoder/frame/FrameTypes.h"
#include <chrono>
#include <thread>
#include <cctype>
#include <algorithm>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <json/json.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
bool isSafeUploadPathPart(const std::string& value, bool allowSubdirs) {
  if (value.empty()) return true;
  if (value.find('\0') != std::string::npos ||
      value.find("..") != std::string::npos ||
      value.front() == '/' || value.front() == '\\') {
    return false;
  }
  if (value.find('\\') != std::string::npos) return false;
  if (!allowSubdirs && value.find('/') != std::string::npos) return false;
  return true;
}

std::string materialUploadBaseDir(const std::string& type) {
  if (type == "image") return hsvj::IMAGE_DIR;
  if (type == "audio") return hsvj::MUSIC_DIR;
  if (type == "font") return hsvj::FONT_DIR;
  return hsvj::VIDEO_DIR;
}

std::string apiSuccessJson(const Json::Value &data) {
  Json::Value root(Json::objectValue);
  root["ok"] = true;
  root["data"] = data;
  root["error"] = Json::Value(Json::nullValue);
  return hsvj::JsonUtils::toString(root);
}

std::string apiErrorJson(const std::string &code, const std::string &message) {
  Json::Value root(Json::objectValue);
  root["ok"] = false;
  root["data"] = Json::Value(Json::nullValue);
  root["error"] = Json::Value(Json::objectValue);
  root["error"]["code"] = code;
  root["error"]["message"] = message;
  return hsvj::JsonUtils::toString(root);
}
}  // 命名空间

void HttpServer::Impl::handleSSEConnection(int clientSocket) {
    // 发送SSE响应头
    std::stringstream responseHeader;
    responseHeader << "HTTP/1.1 200 OK\r\n";
    responseHeader << "Content-Type: text/event-stream\r\n";
    responseHeader << "Cache-Control: no-cache\r\n";
    responseHeader << "Connection: keep-alive\r\n";
    responseHeader << "Access-Control-Allow-Origin: *\r\n";
    responseHeader << "\r\n";

    std::string headerStr = responseHeader.str();
    if (!sendResponseData(clientSocket, std::vector<char>(headerStr.begin(), headerStr.end()))) {
      LOG_ERROR("Failed to send SSE header");
      return;
    }

    // 发送初始连接消息
    std::string initMessage = ": connected\n\n";
    if (!sendResponseData(clientSocket, std::vector<char>(initMessage.begin(), initMessage.end()))) return;

    struct SseRegistration {
      std::mutex& mutex;
      std::vector<int>& clients;
      int socket;
      ~SseRegistration() {
        std::lock_guard<std::mutex> lock(mutex);
        clients.erase(std::remove(clients.begin(), clients.end(), socket), clients.end());
      }
    } registration{sseClientsMutex_, sseClients_, clientSocket};

    // 将客户端添加到SSE客户端列表
    {
      std::lock_guard<std::mutex> lock(sseClientsMutex_);
      sseClients_.push_back(clientSocket);
      LOG_DEBUG("SSE client connected, total clients: %zu", sseClients_.size());
    }

    // 保持连接打开，通过 poll 快速检测客户端断开
    const int heartbeatIntervalSeconds = 30;
    const int pollTimeoutMs = 500;
    int elapsedMs = 0;

    struct pollfd pfd;
    pfd.fd = clientSocket;
    pfd.events = POLLIN;

    while (isRunning_) {
      int ret = poll(&pfd, 1, pollTimeoutMs);
      if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
      if (ret > 0 && (pfd.revents & POLLIN)) {
        // 客户端发数据或断开，尝试读取确认
        char buf[64];
        ssize_t n = recv(clientSocket, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) {
          // 连接已断开（n==0 正常关闭，n<0 错误）
          break;
        }
        // SSE 是单向的，忽略客户端发来的数据
      } else if (ret < 0) {
        if (errno == EINTR) continue;
        // poll 出错
        break;
      }

      elapsedMs += pollTimeoutMs;

      // 定期发送心跳
      if (elapsedMs >= heartbeatIntervalSeconds * 1000) {
        elapsedMs = 0;
        std::string heartbeat = ": heartbeat\n\n";
        std::lock_guard<std::mutex> lock(sseClientsMutex_);
        ssize_t result = send(clientSocket, heartbeat.c_str(),
                              heartbeat.length(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (result != static_cast<ssize_t>(heartbeat.length())) {
          break;
        }
      }
    }

    // 从客户端列表中移除
    size_t remainingClients = 0;
    {
      std::lock_guard<std::mutex> lock(sseClientsMutex_);
      auto it = std::find(sseClients_.begin(), sseClients_.end(), clientSocket);
      if (it != sseClients_.end()) {
        sseClients_.erase(it);
      }
      remainingClients = sseClients_.size();
    }

    LOG_DEBUG("SSE client disconnected, remaining clients: %zu",
             remainingClients);
  }

void HttpServer::Impl::handleMaterialVideoStream(int clientSocket, const HttpRequest &request) {
    // 获取文件路径参数
    std::string path = request.getQueryParam("path");
    if (path.empty()) {
      sendErrorResponse(clientSocket, 400, "Missing path parameter");
      return;
    }

    // 注意：path参数已经在HttpRequest::parseQueryParams中进行了URL解码

    // 使用统一的路径验证函数
    if (!validateMaterialPathForSocket(clientSocket, path)) {
      return;
    }

    // 检查文件是否存在
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
      sendErrorResponse(clientSocket, 404, "File not found");
      return;
    }

    // 获取文件大小
    std::uintmax_t fileSizeUint = fs::file_size(path);
    if (fileSizeUint == 0) {
      sendErrorResponse(clientSocket, 404, "File is empty");
      return;
    }

    // 转换为size_t（注意：对于超大文件可能需要使用uintmax_t）
    // 但为了兼容性，先使用size_t，如果超过size_t最大值则限制
    const size_t MAX_SIZE_T = std::numeric_limits<size_t>::max();
    size_t fileSize = (fileSizeUint > MAX_SIZE_T)
                          ? MAX_SIZE_T
                          : static_cast<size_t>(fileSizeUint);

    // 解析Range请求头
    std::string rangeHeader = request.getHeader("Range");
    size_t startByte = 0;
    size_t endByte = fileSize - 1;
    bool hasRange = false;

    if (!rangeHeader.empty() && rangeHeader.find("bytes=") == 0) {
      hasRange = true;
      std::string rangeValue = rangeHeader.substr(6); // 跳过 "bytes="
      size_t dashPos = rangeValue.find('-');

      if (dashPos != std::string::npos) {
        std::string startStr = rangeValue.substr(0, dashPos);
        std::string endStr = rangeValue.substr(dashPos + 1);

        if (!startStr.empty()) {
          try {
            startByte = std::stoull(startStr);
          } catch (...) {
            sendErrorResponse(clientSocket, 416, "Range Not Satisfiable");
            return;
          }
        }

        if (!endStr.empty()) {
          try {
            endByte = std::stoull(endStr);
          } catch (...) {
            sendErrorResponse(clientSocket, 416, "Range Not Satisfiable");
            return;
          }
        }
      }
    }

    // 验证范围
    if (startByte > endByte || endByte >= fileSize) {
      sendErrorResponse(clientSocket, 416, "Range Not Satisfiable");
      return;
    }

    size_t contentLength = endByte - startByte + 1;

    // 打开文件
    // 对于大文件，使用更大的缓冲区以提高性能
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      LOG_ERROR("Failed to open video file for streaming: %s", path.c_str());
      sendErrorResponse(clientSocket, 500, "Failed to open file");
      return;
    }

    // 设置更大的读取缓冲区（对于大文件很重要）
    const size_t bufferSize = 64 * 1024; // 技术标识：64KB
    char fileBuffer[bufferSize];
    file.rdbuf()->pubsetbuf(fileBuffer, bufferSize);

    // 跳转到起始位置
    // 对于大文件，seek操作可能需要一些时间
    file.seekg(startByte, std::ios::beg);
    if (!file.good()) {
      LOG_ERROR("Failed to seek to position %zu in file: %s", startByte,
                path.c_str());
      sendErrorResponse(clientSocket, 500, "Failed to seek file");
      file.close();
      return;
    }

    LOG_DEBUG("Video stream: serving range %zu-%zu/%zu from file: %s", startByte,
             endByte, fileSize, path.c_str());

    // 构建响应头
    std::ostringstream responseHeader;
    if (hasRange) {
      responseHeader << "HTTP/1.1 206 Partial Content\r\n";
      responseHeader << "Content-Range: bytes " << startByte << "-" << endByte
                     << "/" << fileSize << "\r\n";
    } else {
      responseHeader << "HTTP/1.1 200 OK\r\n";
    }

    // 检测MIME类型
    std::string contentType = "video/mp4";
    bool isDownload = (request.getPath() == "/api/v1/materials/download_file");
    
    if (isDownload) {
      contentType = "application/octet-stream";
    } else {
      std::string ext = path.substr(path.find_last_of('.'));
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (ext == ".hvideo" || ext == ".mp4" || ext == ".m4v") {
        contentType = "video/mp4";
      } else if (ext == ".webm") {
        contentType = "video/webm";
      } else if (ext == ".ogg" || ext == ".ogv") {
        contentType = "video/ogg";
      } else if (ext == ".mpg" || ext == ".mpeg") {
        contentType = "video/mpeg";
      } else if (ext == ".avi") {
        contentType = "video/x-msvideo";
      } else if (ext == ".mkv") {
        contentType = "video/x-matroska";
      } else if (ext == ".mov") {
        contentType = "video/quicktime";
      } else if (ext == ".flv") {
        contentType = "video/x-flv";
      } else if (ext == ".wmv") {
        contentType = "video/x-ms-wmv";
      } else if (ext == ".ts" || ext == ".m2ts") {
        contentType = "video/mp2t";
      } else if (ext == ".3gp") {
        contentType = "video/3gpp";
      } else {
        contentType = "application/octet-stream";
      }
    }

    responseHeader << "Content-Type: " << contentType << "\r\n";
    responseHeader << "Content-Length: " << contentLength << "\r\n";
    
    if (isDownload) {
      fs::path filePath(path);
      responseHeader << "Content-Disposition: attachment; filename=\"" << filePath.filename().string() << "\"\r\n";
    }
    
    responseHeader << "Accept-Ranges: bytes\r\n";
    responseHeader << "Cache-Control: no-cache\r\n";
    responseHeader << "Connection: close\r\n";
    // CORS 头部 - 允许跨域访问
    responseHeader << "Access-Control-Allow-Origin: *\r\n";
    responseHeader << "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n";
    responseHeader << "Access-Control-Allow-Headers: Range\r\n";
    responseHeader << "Access-Control-Expose-Headers: Content-Range, "
                      "Content-Length, Accept-Ranges\r\n";
    responseHeader << "\r\n";

    // 发送响应头
    std::string headerStr = responseHeader.str();
    ssize_t sent = send(clientSocket, headerStr.c_str(), headerStr.length(), 0);
    if (sent < 0) {
      LOG_ERROR("Failed to send video stream header");
      return;
    }

    // 发送文件内容（分块发送，避免大文件一次性加载到内存）
    std::vector<char> buffer(bufferSize);
    size_t remaining = contentLength;
    size_t totalSent = 0;

    while (remaining > 0 && file.good() && isRunning_) {
      size_t toRead = std::min(remaining, bufferSize);
      file.read(buffer.data(), toRead);
      size_t bytesRead = file.gcount();

      if (bytesRead == 0) {
        break;
      }

      // 发送数据块
      size_t offset = 0;
      while (offset < bytesRead) {
        ssize_t sentBytes =
            send(clientSocket, buffer.data() + offset, bytesRead - offset, 0);
        if (sentBytes < 0) {
          int sendErrno = errno;
          if (sendErrno == EPIPE || sendErrno == ECONNRESET) {
            LOG_DEBUG("Client disconnected while sending video stream");
          } else {
            LOG_ERROR("Failed to send video stream data: errno=%d (%s)",
                      sendErrno, strerror(sendErrno));
          }
          file.close();
          return;
        }
        offset += sentBytes;
        totalSent += sentBytes;
      }

      remaining -= bytesRead;
    }

    file.close();
    LOG_DEBUG("Video stream sent: %zu bytes (range: %zu-%zu)", totalSent,
             startByte, endByte);
  }

void HttpServer::Impl::handleMaterialUpload(int clientSocket, const HttpRequest &headerRequest,
                            const std::vector<char> &headerData,
                            size_t headerEndPos) {
    struct UploadCleanup {
      std::string path;
      ~UploadCleanup() {
        if (!path.empty()) {
          std::error_code error;
          fs::remove(path, error);
        }
      }
    } uploadCleanup;
    try {
      // 从查询参数获取文件名和类型
      std::string filename = headerRequest.getQueryParam("filename");
      std::string type = headerRequest.getQueryParam("type");
      std::string targetFolder = headerRequest.getQueryParam("targetFolder");
      std::string folderName = headerRequest.getQueryParam("folderName");

      if (filename.empty()) {
        sendErrorResponse(clientSocket, 400, "Missing filename parameter");
        return;
      }
      if (!isSafeUploadPathPart(filename, true) ||
          !isSafeUploadPathPart(targetFolder, false) ||
          !isSafeUploadPathPart(folderName, false)) {
        sendErrorResponse(clientSocket, 400, "Invalid upload path");
        return;
      }

      std::string storage = headerRequest.getQueryParam("storage");
      if (storage.empty()) storage = "internal";
      if (!hsvj::videoStorage::validSelection(storage) ||
          (storage == "sata" && type != "video")) {
        sendErrorResponse(clientSocket, 400, "无效的存储位置，SATA 仅支持视频上传");
        return;
      }
      if (storage == "sata" && !hsvj::videoStorage::sataWritable()) {
        sendErrorResponse(clientSocket, 503, "SATA 硬盘未挂载、只读或没有写入权限，请检查硬盘");
        return;
      }

      // 获取 Content-Length
      std::string contentLengthStr = headerRequest.getHeader("Content-Length");
      if (contentLengthStr.empty()) {
        sendErrorResponse(clientSocket, 400, "Missing Content-Length header");
        return;
      }

      size_t contentLength = 0;
      if (!headerRequest.getContentLength(contentLength)) {
        sendErrorResponse(clientSocket, 400, "Invalid Content-Length header");
        return;
      }

      constexpr uint64_t MAX_UPLOAD_BYTES = 4ULL * 1024ULL * 1024ULL * 1024ULL;
      constexpr size_t DISK_RESERVE_BYTES = 200ULL * 1024ULL * 1024ULL;
      if (contentLength > MAX_UPLOAD_BYTES ||
          contentLength > std::numeric_limits<size_t>::max() - DISK_RESERVE_BYTES) {
        sendErrorResponse(clientSocket, 413, "Upload body too large");
        return;
      }
      if (headerEndPos > headerData.size() ||
          headerData.size() - headerEndPos > contentLength) {
        sendErrorResponse(clientSocket, 400, "Invalid upload body length");
        return;
      }

      std::string baseDir = storage == "sata" ? hsvj::videoStorage::sataRoot : materialUploadBaseDir(type);
      if (baseDir.empty()) {
        sendErrorResponse(clientSocket, 500, "ROOT_PATH is not initialized");
        return;
      }

      // 磁盘容量保护：检查目标分区可用空间是否足够
      try {
        fs::path checkPath(baseDir);
        if (!fs::exists(checkPath)) {
          fs::create_directories(checkPath);
        }
        auto spaceInfo = fs::space(checkPath);
        size_t requiredSpace = contentLength + DISK_RESERVE_BYTES;
        if (spaceInfo.available < requiredSpace) {
          double availMB = spaceInfo.available / (1024.0 * 1024.0);
          double needMB = contentLength / (1024.0 * 1024.0);
          std::string msg = "磁盘空间不足: 可用 " + std::to_string((int)availMB)
                          + "MB, 需要 " + std::to_string((int)needMB)
                          + "MB (保留 200MB 安全余量)";
          LOG_WARN("Upload rejected - %s, path: %s", msg.c_str(), baseDir.c_str());

          HttpResponse diskResponse;
          diskResponse.setStatusCode(507);
          diskResponse.setStatusMessage("Insufficient Storage");
          diskResponse.setJson(apiErrorJson("DISK_SPACE_INSUFFICIENT", msg));
          std::vector<char> respData = diskResponse.build();
          send(clientSocket, respData.data(), respData.size(), 0);
          return;
        }
      } catch (const std::exception &e) {
        LOG_WARN("Failed to check disk space: %s", e.what());
        sendErrorResponse(clientSocket, 507, std::string("无法写入目标存储或读取剩余空间: ") + e.what());
        return;
      }

      // 确定保存路径
      std::string relativePath = filename;
      if (!targetFolder.empty()) {
        relativePath = hsvj::FileUtils::joinPath(targetFolder, filename);
      } else if (!folderName.empty()) {
        relativePath = hsvj::FileUtils::joinPath(folderName, filename);
      }
      std::string savePath = hsvj::FileUtils::joinPath(baseDir, relativePath);

      // 处理相对路径中的目录结构（确保所有父目录都存在）
      fs::path savePathObj(savePath);
      fs::path normalizedParentDir = savePathObj.parent_path();

      if (storage == "sata" && !hsvj::videoStorage::safeSataPath((fs::path(hsvj::videoStorage::sataRoot) / fs::path(relativePath).parent_path()).string())) {
        sendErrorResponse(clientSocket, 400, "硬盘已断开或上传目录超出视频目录");
        return;
      }

      // 确保目录存在（包括所有父目录）
      try {
        if (!fs::exists(normalizedParentDir) ||
            !fs::is_directory(normalizedParentDir)) {
          fs::create_directories(normalizedParentDir);
          LOG_DEBUG("Created materials directory: %s",
                   normalizedParentDir.c_str());
        }
      } catch (const std::exception &e) {
        LOG_ERROR("Error creating directory %s: %s",
                  normalizedParentDir.c_str(), e.what());
        sendErrorResponse(clientSocket, 500,
                          std::string("Failed to create directory: ") +
                              e.what());
        return;
      }

      // Reject symlinked destination folders that escape the configured root.
      std::error_code pathError;
      const fs::path canonicalBase = fs::weakly_canonical(fs::path(baseDir), pathError);
      if (pathError) throw fs::filesystem_error("Cannot resolve upload root", pathError);
      const fs::path canonicalParent = fs::weakly_canonical(normalizedParentDir, pathError);
      if (pathError) throw fs::filesystem_error("Cannot resolve upload parent", pathError);
      const fs::path relativeParent = canonicalParent.lexically_relative(canonicalBase);
      const bool escaped = relativeParent.empty() || relativeParent.is_absolute() ||
                           *relativeParent.begin() == "..";
      if (canonicalBase.empty() || canonicalParent.empty() || escaped) {
        sendErrorResponse(clientSocket, 400, "Invalid upload destination");
        return;
      }

      savePath = (canonicalParent / savePathObj.filename()).string();
      const std::string tempTemplate = savePath + ".uploading.XXXXXX";
      std::vector<char> tempName(tempTemplate.begin(), tempTemplate.end());
      tempName.push_back('\0');
      const int tempFd = mkstemp(tempName.data());
      if (tempFd < 0) {
        sendErrorResponse(clientSocket, 500, std::string("无法创建上传文件: ") + strerror(errno));
        return;
      }
      const std::string tempPath(tempName.data());
      uploadCleanup.path = tempPath;

      // 写入临时文件，完整接收并校验后再原子替换目标，避免中断上传
      // 删除或破坏之前已经可用的素材。
      std::unique_ptr<FILE, decltype(&std::fclose)> outFile(fdopen(tempFd, "wb"), &std::fclose);
      if (!outFile) {
        close(tempFd);
        LOG_ERROR("Failed to create file: %s", tempPath.c_str());
        sendErrorResponse(clientSocket, 500, "Failed to create file");
        return;
      }
      if (fcntl(tempFd, F_SETFD, FD_CLOEXEC) < 0) {
        sendErrorResponse(clientSocket, 500, "Failed to secure upload file");
        return;
      }

      // 计算已接收的数据（头部数据中可能已经包含部分请求体）
      size_t bodyReceived = headerData.size() - headerEndPos;
      size_t bodyToWrite = bodyReceived;

      // 如果头部数据中已经包含部分请求体，先写入文件
      if (bodyToWrite > 0) {
        if (std::fwrite(headerData.data() + headerEndPos, 1, bodyToWrite, outFile.get()) != bodyToWrite) {
          LOG_ERROR("Failed to write initial data to file: %s",
                    tempPath.c_str());
          sendErrorResponse(clientSocket, 500, "Failed to write file");
          return;
        }
      }

      // 流式读取剩余数据并写入文件（使用64KB缓冲区）
      const size_t BUFFER_SIZE = 64 * 1024; // 64KB 缓冲区
      std::vector<char> buffer(BUFFER_SIZE);
      size_t totalWritten = bodyToWrite;

      // 设置socket接收超时（大文件上传可能需要更长时间）
      struct timeval tv;
      tv.tv_sec = 30; // 30秒超时
      tv.tv_usec = 0;
      setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      while (isRunning_ && bodyReceived < contentLength) {
        size_t remaining = contentLength - bodyReceived;
        size_t toRead = std::min(buffer.size(), remaining);

        ssize_t bytesRead = recv(clientSocket, buffer.data(), toRead, 0);
        if (bytesRead <= 0) {
          if (bytesRead == 0) {
            // 连接关闭
            LOG_WARN("Client closed connection during upload, received %zu/%zu "
                     "bytes",
                     bodyReceived, contentLength);
          } else {
            // 读取错误
            int recvErrno = errno;
            if (recvErrno == EINTR) continue;
            if (recvErrno == EAGAIN || recvErrno == EWOULDBLOCK) {
              LOG_WARN(
                  "Upload read timeout (received %zu/%zu bytes)",
                  bodyReceived, contentLength);
            } else {
              LOG_ERROR("Failed to read upload data: errno=%d (%s)", recvErrno,
                        strerror(recvErrno));
            }
          }
          break;
        }

        // 写入文件
        if (std::fwrite(buffer.data(), 1, static_cast<size_t>(bytesRead), outFile.get()) !=
            static_cast<size_t>(bytesRead)) {
          LOG_ERROR("Failed to write data to file: %s", tempPath.c_str());
          break;
        }

        bodyReceived += bytesRead;
        totalWritten += bytesRead;
      }

      bool writeOk = std::fflush(outFile.get()) == 0;
      int writeError = writeOk ? 0 : errno;
      if (writeOk && !hsvj::videoStorage::syncUpload(tempFd, storage == "sata")) {
        writeOk = false;
        writeError = errno;
      }
      if (std::fclose(outFile.release()) != 0) {
        if (writeOk) writeError = errno;
        writeOk = false;
      }

      // 检查是否完整接收
      if (bodyReceived != contentLength || !writeOk) {
        LOG_ERROR("Upload incomplete: received %zu/%zu bytes, file: %s",
                  bodyReceived, contentLength, tempPath.c_str());
        // 删除不完整的文件
        try {
          fs::remove(tempPath);
        } catch (...) {
        }
        sendErrorResponse(clientSocket, 500, writeError ? std::string("保存上传文件失败: ") + strerror(writeError) : "上传未完成，请检查网络后重试");
        return;
      }

      if (storage == "sata" && !hsvj::videoStorage::safeSataPath(savePath)) {
        sendErrorResponse(clientSocket, 503, "上传期间硬盘已断开，请重新连接后重试");
        return;
      }

      // ========== 10位视频格式检查 ==========
      // 在上传完成后检查视频是否为10位格式，如果是则拒绝上传
      // 这是因为当前GPU不支持10位视频的硬件解码
      if (type != "image") {
        // 只对视频文件进行检查
        std::string ext = "";
        size_t dotPos = savePath.rfind('.');
        if (dotPos != std::string::npos) {
          ext = savePath.substr(dotPos);
          std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }

        // 只检查视频文件扩展名
        bool isVideoFile = (ext == ".hvideo" || ext == ".mp4" || ext == ".mkv" || ext == ".avi" ||
                            ext == ".mov" || ext == ".mpg" || ext == ".mpeg" ||
                            ext == ".webm" || ext == ".flv" || ext == ".wmv");

        if (isVideoFile) {
          LOG_DEBUG("检查上传视频的格式支持: %s", tempPath.c_str());

          std::string detectedFormat = "";
          std::string errorCode = "";
          bool isFormatSupported = hsvj::MediaUtils::checkVideoFormatSupport(
              tempPath, detectedFormat, errorCode);

          if (!isFormatSupported) {
            // 格式不兼容：保留文件，但在响应中附带警告信息
            std::string warningMessage;
            if (errorCode == "UNSUPPORTED_DXV_VIDEO" || errorCode == "UNSUPPORTED_PRO_CODEC" ||
                errorCode == "CODEC_TAG_MATCH" || errorCode == "METADATA_TAG") {
                warningMessage = "该视频使用了不支持的专业编码格式，可能无法正常播放";
            } else if (errorCode == "COLOR_CONFIG") {
                warningMessage = "该视频使用了不支持的色彩配置 (BT2020 或高色度)，可能无法正常播放";
            } else if (errorCode == "PRO_ENCODER_STRICT") {
                warningMessage = "该视频由专业编码器生成，格式可能不兼容，建议重新编码";
            } else if (errorCode == "FILENAME_KEYWORD") {
                warningMessage = "该视频格式可能不兼容，建议检查编码格式";
            } else {
                // 说明：10BIT or fallback
                warningMessage = "该视频为 10 位色深格式，可能无法正常播放，建议转换为 8 位视频";
            }

            LOG_WARN("上传了格式不兼容的视频 (已保留文件): %s (代码: %s, 格式: %s)",
                      tempPath.c_str(), errorCode.c_str(),
                      detectedFormat.c_str());
            std::error_code renameError;
            fs::rename(tempPath, savePath, renameError);
            if (renameError) {
              fs::remove(tempPath);
              sendErrorResponse(clientSocket, 500, "Failed to publish uploaded file");
              return;
            }
            uploadCleanup.path.clear();
            if (materialIndex_) materialIndex_->upsertFile(savePath);

            // 上传成功但附带警告
            Json::Value data;
            data["path"] = savePath;
            data["size"] = (Json::Int64)totalWritten;
            data["warning"] = Json::Value(Json::objectValue);
            data["warning"]["code"] = errorCode;
            data["warning"]["message"] =
                warningMessage + " 检测到格式: " + detectedFormat;

            std::string jsonStr = apiSuccessJson(data);
            HttpResponse warnResponse;
            warnResponse.setStatusCode(200);
            warnResponse.setStatusMessage("OK");
            warnResponse.setJson(jsonStr);

            std::vector<char> responseData = warnResponse.build();
            sendResponseData(clientSocket, responseData);
            return;
          }
        }
      }

      std::error_code renameError;
      fs::rename(tempPath, savePath, renameError);
      if (renameError) {
        fs::remove(tempPath);
        sendErrorResponse(clientSocket, 500, "Failed to publish uploaded file");
        return;
      }
      uploadCleanup.path.clear();

      // 上传成功，发送JSON响应
      Json::Value data;
      data["path"] = savePath;
      data["size"] = (Json::Int64)totalWritten;
      if (materialIndex_) materialIndex_->upsertFile(savePath);

      const std::string jsonStr = apiSuccessJson(data);

      HttpResponse response;
      response.setStatusCode(200);
      response.setStatusMessage("OK");
      response.setJson(jsonStr);

      std::vector<char> responseData = response.build();

      if (sendResponseData(clientSocket, responseData)) {
        LOG_DEBUG("File uploaded successfully: %s (%zu bytes)", savePath.c_str(),
                 totalWritten);

        if (type == "video") {
          LOG_DEBUG("Triggering async thumbnail generation for: %s",
                   savePath.c_str());
          hsvj::MediaUtils::generateThumbnailAsync(
              savePath,
              [this](const std::string &event, const std::string &data) {
                this->broadcastSSEMessage(event, data);
              });
        }
      }

    } catch (const std::exception &e) {
      LOG_ERROR("Exception in handleMaterialUpload: %s", e.what());
      try {
        sendErrorResponse(clientSocket, 500,
                          std::string("Upload failed: ") + e.what());
      } catch (...) {
        LOG_ERROR("Failed to send error response");
      }
    } catch (...) {
      LOG_ERROR("Unknown exception in handleMaterialUpload");
      try {
        sendErrorResponse(clientSocket, 500, "Upload failed");
      } catch (...) {
        LOG_ERROR("Failed to send error response");
      }
    }

  }

void HttpServer::Impl::handleRequest(HttpRequest &request, HttpResponse &response) {
    // 获取请求方法和路径
    std::string method = request.getMethod();
    std::string path = request.getPath();
    // 路径无前导 / 时补上，与路由注册格式一致
    if (!path.empty() && path[0] != '/') {
      path = "/" + path;
    }

    // 默认 HTTP 地址上的手机融合页复用 8081 API；只允许本设备页面跨端口访问。
    if (port_ == 8081 && path.find("/api/v1/") == 0) {
      const std::string origin = request.getHeader("Origin");
      std::string host = request.getHeader("Host");
      const std::string portSuffix = ":8081";
      if (host.size() > portSuffix.size() &&
          host.compare(host.size() - portSuffix.size(), portSuffix.size(), portSuffix) == 0) {
        host.resize(host.size() - portSuffix.size());
      }
      if (!host.empty() &&
          (origin == "http://" + host || origin == "http://" + host + ":8080")) {
        response.setHeader("Access-Control-Allow-Origin", origin);
        response.setHeader("Vary", "Origin");
        if (method == "OPTIONS") {
          response.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
          response.setHeader("Access-Control-Allow-Headers", "Content-Type");
          response.setStatusCode(204);
          response.setStatusMessage("No Content");
          response.setBody("");
          return;
        }
      }
    }

    // 检查是否是API请求
    if (path.find("/api/v1/") == 0) {
      handleApiRequest(method, path, request, response);
    } else {
      // 处理静态文件请求（单端口时 / 在 handleStaticRequest 中重定向到 /vod/）
      handleStaticRequest(path, response);
    }
  }

namespace {

// 路径 URL 解码（用于路由匹配：如 %2D -> -），避免浏览器编码导致 404
static std::string urlDecodePath(const std::string &str) {
  std::string decoded;
  decoded.reserve(str.size());
  for (size_t i = 0; i < str.length(); ++i) {
    if (str[i] == '%' && i + 2 < str.length()) {
      std::string hex = str.substr(i + 1, 2);
      try {
        char c = static_cast<char>(std::stoi(hex, nullptr, 16));
        decoded += c;
        i += 2;
      } catch (...) {
        decoded += str[i];
      }
    } else if (str[i] == '+') {
      decoded += ' ';
    } else {
      decoded += str[i];
    }
  }
  return decoded;
}

} // 命名空间

void HttpServer::Impl::handleApiRequest(const std::string &method, const std::string &path,
                        HttpRequest &request, HttpResponse &response) {
    // 路径规范化：URL 解码（%2D -> - 等）、合并连续斜杠、去除首尾空白、去除尾部斜杠，便于路由匹配
    std::string normalizedPath = urlDecodePath(path);
    // 合并连续斜杠，避免 /api//播放列表/play 等导致 404
    while (true) {
      size_t pos = normalizedPath.find("//");
      if (pos == std::string::npos) break;
      normalizedPath.erase(pos, 1);
    }
    while (!normalizedPath.empty() &&
           (normalizedPath.front() == ' ' || normalizedPath.front() == '\t')) {
      normalizedPath.erase(0, 1);
    }
    while (!normalizedPath.empty() &&
           (normalizedPath.back() == ' ' || normalizedPath.back() == '\t' ||
            normalizedPath.back() == '/')) {
      normalizedPath.pop_back();
    }
    if (normalizedPath.empty()) {
      normalizedPath = "/";
    }
    // 确保路径以 / 开头（部分浏览器或代理可能发送 api/system/ping 导致无法匹配）
    if (normalizedPath.size() && normalizedPath[0] != '/') {
      normalizedPath = "/" + normalizedPath;
    }

    // Web界面认证检查（非9898端口）
    if (port_ != 9898 || normalizedPath == "/api/v1/system/factory-serial") {
      // 登录接口不需要认证
      if (normalizedPath != "/api/v1/auth/config" && normalizedPath != "/api/v1/auth/login" && normalizedPath != "/api/v1/auth/logout" && normalizedPath != "/api/v1/auth/validate") {
        if (!isAuthenticated(request)) {
          response.setStatusCode(401);
          response.setStatusMessage("Unauthorized");
          response.setJson(apiErrorJson(
              "UNAUTHORIZED", "Unauthorized - Please login first"));
          return;
        }
      }
    }

    // 9898 点歌端口（VOD/KTV）：保留现有 pad 协议白名单；其他路由必须
    // 通过统一的兼容头校验，避免认证规则在不同处理路径重复实现。
    static const int VOD_KTV_PORT = 9898;
    const bool isPadApi =
        normalizedPath.find("/api/v1/songdb/") == 0 ||
        normalizedPath.find("/api/v1/rooms/") == 0 ||
        normalizedPath == "/api/v1/service-types" ||
        normalizedPath.find("/api/v1/peripheral/") == 0 ||
        normalizedPath.find("/api/v1/products") == 0 ||
        normalizedPath.find("/api/v1/orders") == 0 ||
        normalizedPath.find("/api/v1/materials") == 0 ||
        normalizedPath.find("/api/v1/streams") == 0 ||
        normalizedPath.find("/api/v1/youtube/") == 0 ||
        normalizedPath.find("/api/v1/singerimg") == 0;
    const bool isArtistImageApi =
        normalizedPath.find("/api/v1/artists/") == 0 &&
        normalizedPath.size() >= 6 &&
        normalizedPath.rfind("/image") == normalizedPath.size() - 6;
    if (port_ == VOD_KTV_PORT && isPadApi) {
      std::string body = request.getBody();
      if (body.size() > 512) body = body.substr(0, 512) + "...";
      LOG_INFO("[PadAPI] recv port=%d method=%s path=%s body=%s",
               port_, method.c_str(), normalizedPath.c_str(), body.c_str());
    }
    if (port_ == VOD_KTV_PORT && !isPadApi) {
      if (!isVodClientAuthenticated(request)) {
        if (!isArtistImageApi) {
          LOG_WARN("[PadAPI] unauthorized port=%d method=%s path=%s uname=%s clientid=%s",
                   port_, method.c_str(), normalizedPath.c_str(),
                   request.getHeader("_uname").c_str(),
                   request.getHeader("clientid").c_str());
        }
        response.setStatusCode(401);
        response.setStatusMessage("Unauthorized");
        response.setJson(apiErrorJson("UNAUTHORIZED", "Unauthorized"));
        return;
      }
    }

    // 查找匹配的路由（需要线程安全保护）
    std::function<void(const HttpRequest &, HttpResponse &)> matchedHandler;

    {
      std::lock_guard<std::mutex> lock(routesMutex_);
      auto methodIt = routes_.find(method);
      if (methodIt == routes_.end()) {
        LOG_WARN("API request method not found: %s %s", method.c_str(), normalizedPath.c_str());
        response.setStatusCode(405);
        response.setStatusMessage("Method Not Allowed");
        response.setJson(apiErrorJson("METHOD_NOT_ALLOWED",
                                      "Method not allowed"));
        return;
      }

      // 尝试精确匹配
      auto pathIt = methodIt->second.find(normalizedPath);
      if (pathIt != methodIt->second.end()) {
        matchedHandler = pathIt->second;
      } else {
        // 尝试路径参数匹配（如/api/scenes/{name}）
        // 优先匹配更具体的路由（非参数部分更多的路由）
        std::pair<std::string, std::function<void(const HttpRequest &, HttpResponse &)>> bestMatch;
        int bestNonParamCount = -1;
        bool foundMatch = false;
        
        for (const auto &route : methodIt->second) {
          // 临时创建一个新的request对象来测试匹配，避免污染原始request
          HttpRequest testRequest;
          if (matchPath(route.first, normalizedPath, testRequest)) {
            // 计算非参数部分数量
            std::vector<std::string> parts = splitPath(route.first);
            int nonParamCount = 0;
            for (const auto &part : parts) {
              if (!part.empty() && (part[0] != '{' || part.back() != '}')) {
                nonParamCount++;
              }
            }
            
            // 选择非参数部分最多的路由（更具体的路由）
            if (nonParamCount > bestNonParamCount) {
              bestNonParamCount = nonParamCount;
              bestMatch = route;
              foundMatch = true;
            }
          }
        }
        
        if (foundMatch) {
          // 重新匹配以设置URL参数到原始request
          matchPath(bestMatch.first, normalizedPath, request);
          matchedHandler = bestMatch.second;
          LOG_DEBUG("Matched route: %s %s -> %s (nonParamCount=%d)", method.c_str(), normalizedPath.c_str(), bestMatch.first.c_str(), bestNonParamCount);
        }
      }
    } // 释放锁，避免handler执行时间过长导致其他请求阻塞

    if (matchedHandler) {
      matchedHandler(request, response);
      return;
    }

    // 未找到匹配的路由，返回 JSON 以便前端正确解析
    LOG_WARN("API endpoint not found: %s %s", method.c_str(), normalizedPath.c_str());
    response.setStatusCode(404);
    response.setStatusMessage("未找到");
    response.setJson(apiErrorJson("NOT_FOUND",
                                  "API endpoint not found: " + normalizedPath));
  }

void HttpServer::Impl::handleStaticRequest(const std::string &path, HttpResponse &response) {
    std::string pathDecoded = urlDecodePath(path);
    // 兼容已打开或收藏的旧登录地址，回到手机控制主页。
    if (port_ == 8081 && pathDecoded == "/login.html") {
      response.setRedirect("/");
      return;
    }
    std::string normalizedPath = pathDecoded;
    std::string baseDir = staticDir_;

    if (!ktvStaticDir_.empty()) {
      // 单端口模式：/ktv、/vod、/shared 按路径区分静态根，/ 重定向到 /vod/
      std::string p = pathDecoded.empty() ? "/" : pathDecoded;
      if (p == "/") {
        response.setStatusCode(404);
        response.setStatusMessage("未找到");
        response.setText("Use /vod/ or /ktv/");
        return;
      }
      // /vod、/ktv 无尾部斜杠时重定向到 /vod/、/ktv/，否则相对路径会解析到根导致 404
      if (p == "/vod") {
        response.setRedirect("/vod/");
        return;
      }
      if (p == "/ktv") {
        response.setRedirect("/ktv/");
        return;
      }
      if (p == "/ktv" || p == "/ktv/" || (p.size() > 5 && p.compare(0, 5, "/ktv/") == 0)) {
        baseDir = ktvStaticDir_;
        normalizedPath = (p.size() <= 5) ? "index.html" : p.substr(5);
        if (normalizedPath.empty()) normalizedPath = "index.html";
      } else if (p == "/vod" || p == "/vod/" || (p.size() > 5 && p.compare(0, 5, "/vod/") == 0)) {
        baseDir = staticDir_;
        normalizedPath = (p.size() <= 5) ? "index.html" : p.substr(5);
        if (normalizedPath.empty()) normalizedPath = "index.html";
      } else if (p.size() >= 8 && p.compare(0, 8, "/shared/") == 0) {
        baseDir = sharedStaticDir_;
        normalizedPath = p.substr(8);
        if (normalizedPath.empty()) normalizedPath = "index.html";
      } else {
        response.setStatusCode(404);
        response.setStatusMessage("未找到");
        response.setText("未找到");
        return;
      }
      if (!normalizedPath.empty() && normalizedPath[0] == '/')
        normalizedPath = normalizedPath.substr(1);
    } else {
      // 原逻辑：单静态根 + /shared/ 映射
      if (normalizedPath.empty() || normalizedPath == "/")
        normalizedPath = "/index.html";
      if (!normalizedPath.empty() && normalizedPath[0] == '/')
        normalizedPath = normalizedPath.substr(1);
      if (!sharedStaticDir_.empty() && normalizedPath.size() >= 7 &&
          normalizedPath.compare(0, 7, "shared/") == 0) {
        baseDir = sharedStaticDir_;
        normalizedPath = normalizedPath.substr(7);
      }
    }

    // 路径规范化：解析 . 与 ..，再检查是否试图逃出静态根（仅拒绝规范化后仍含 .. 的路径）
    {
      fs::path relPath(normalizedPath);
      relPath = relPath.lexically_normal();
      std::string normStr = relPath.string();
      std::replace(normStr.begin(), normStr.end(), '\\', '/');
      if (normStr.find("..") != std::string::npos) {
        LOG_WARN("Path traversal attempt detected: %s (normalized: %s)", normalizedPath.c_str(), normStr.c_str());
        response.setStatusCode(403);
        response.setStatusMessage("Forbidden");
        response.setText("Path traversal not allowed");
        return;
      }
      normalizedPath = normStr;
    }

    // 使用filesystem::path自动处理跨平台路径分隔符
    fs::path baseDirPath(baseDir);
    fs::path normalizedPathObj(normalizedPath);
    fs::path fullPath = baseDirPath / normalizedPathObj;
    std::string filePath;

    // 安全检查：使用filesystem::canonical规范化路径并检查是否在静态目录内
    // 注意：如果文件不存在，canonical会抛出异常，所以先检查文件是否存在
    try {
      // 先获取静态目录的规范路径（当前请求可能来自 staticDir_ 或 sharedStaticDir_）
      fs::path canonicalBaseDir;
      if (fs::exists(baseDir) && fs::is_directory(baseDir)) {
        canonicalBaseDir = fs::canonical(fs::absolute(baseDir));
      } else {
        // 如果静态目录不存在，使用绝对路径并规范化
        canonicalBaseDir = fs::absolute(baseDir);
        canonicalBaseDir = canonicalBaseDir.lexically_normal();
      }

      // 检查文件是否存在，如果存在则获取规范路径
      fs::path canonicalFullPath;
      if (fs::exists(fullPath)) {
        canonicalFullPath = fs::canonical(fs::absolute(fullPath));
      } else {
        // 如果文件不存在，使用绝对路径并规范化（用于安全检查）
        // 使用 lexically_normal 规范化路径格式，使其与 canonical 结果一致
        canonicalFullPath = fs::absolute(fullPath);
        canonicalFullPath = canonicalFullPath.lexically_normal();
      }

      // 检查规范化后的路径是否在静态目录内（使用统一的辅助函数）
      // 先将路径转换为字符串，然后统一规范化（处理Windows反斜杠问题）
      std::string baseDirStr = canonicalBaseDir.string();
      std::string fullPathStr = canonicalFullPath.string();

      // 规范化路径字符串：统一使用正斜杠，目录路径添加末尾斜杠
      normalizePathStringForDir(baseDirStr);
      normalizePathString(fullPathStr);

      // 调试日志：记录路径信息（仅在路径检查失败时记录，避免日志过多）
      // 先检查路径是否在基础目录内（文件路径应以基础目录路径开头）
      bool isWithinBase =
          (fullPathStr.length() >= baseDirStr.length() &&
           fullPathStr.compare(0, baseDirStr.length(), baseDirStr) == 0);

      // Windows 下路径大小写可能不一致，导致误判 403；再做一次不区分大小写的前缀比较
#ifdef _WIN32
      if (!isWithinBase && fullPathStr.length() >= baseDirStr.length()) {
        auto ciEqual = [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) ==
                 std::tolower(static_cast<unsigned char>(b));
        };
        isWithinBase = std::equal(baseDirStr.begin(), baseDirStr.end(),
                                  fullPathStr.begin(), ciEqual);
      }
#endif

      if (!isWithinBase) {
        LOG_DEBUG("Path check failed - baseDir=%s (len=%zu), fullPath=%s "
                 "(len=%zu), normalized=%s, request=%s",
                 baseDirStr.c_str(), baseDirStr.length(), fullPathStr.c_str(),
                 fullPathStr.length(), normalizedPath.c_str(), path.c_str());
        LOG_WARN("Path traversal attempt detected: base=%s, full=%s, "
                 "normalized=%s, request=%s",
                 baseDirStr.c_str(), fullPathStr.c_str(),
                 normalizedPath.c_str(), path.c_str());
        response.setStatusCode(403);
        response.setStatusMessage("Forbidden");
        response.setText("Path traversal not allowed");
        return;
      }

      // 如果文件存在，使用规范路径；否则使用规范化后的完整路径
      if (fs::exists(fullPath)) {
        filePath = canonicalFullPath.string();
      } else {
        // 文件不存在时，仍然使用规范化后的路径（后续会返回404）
        filePath = canonicalFullPath.string();
      }
    } catch (const fs::filesystem_error &e) {
      LOG_WARN("Failed to resolve static path: %s", e.what());
      response.setStatusCode(403);
      response.setStatusMessage("Forbidden");
      response.setText("Unable to validate static path");
      return;
    }

    // 检查静态目录是否存在
    if (!dirExists(baseDir)) {
      LOG_ERROR("Static directory does not exist: %s", baseDir.c_str());
      response.setStatusCode(500);
      response.setStatusMessage("Internal Server Error");
      response.setText("Static directory not found");
      return;
    }

    // 如果路径是目录，添加 index.html
    if (dirExists(filePath)) {
      if (filePath.back() != '/' && filePath.back() != '\\') {
        filePath += '/';
      }
      filePath += "index.html";
      std::replace(filePath.begin(), filePath.end(), '\\', '/');
    }

    // 检查文件是否存在
    if (fileExists(filePath)) {
      response.setFile(filePath);
    } else {
      LOG_WARN("File not found: %s", filePath.c_str());
      response.setNotFound();
    }
  }
