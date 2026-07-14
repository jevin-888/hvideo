#include "HttpServer_Internal.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/SystemUtils.h"
#include "database/PlaylistManager.h"
#include "network/PreviewStreamConverter.h"
#include "decoder/frame/FrameTypes.h"
#include "layer/LayerVideo.h"
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <sstream>

// 辅助函数：分割URL路径为部分
std::vector<std::string> HttpServer::Impl::splitPath(const std::string &path) {
  std::vector<std::string> parts;
  std::stringstream ss(path);
  std::string part;
  while (std::getline(ss, part, '/')) {
    if (!part.empty()) {
      parts.push_back(part);
    }
  }
  return parts;
}
