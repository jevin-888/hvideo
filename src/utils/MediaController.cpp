/**
 * @file MediaController.cpp（文件名）
 * @brief Media Controller 配置工具实现
 */

#include "utils/MediaController.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>

namespace hsvj {

namespace {

std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string lowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

std::string basenameOf(const std::string &path) {
  size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string entityNameFromLine(const std::string &line) {
  size_t colon = line.find(":");
  size_t paren = line.find("(", colon == std::string::npos ? 0 : colon);
  if (colon == std::string::npos || paren == std::string::npos || paren <= colon) return "";
  return trim(line.substr(colon + 1, paren - colon - 1));
}

std::string quotedEntityOnIncomingLink(const std::string &line) {
  size_t arrowPos = line.find("<-");
  if (arrowPos == std::string::npos) return "";
  size_t startQuote = line.find('"', arrowPos + 2);
  if (startQuote == std::string::npos) return "";
  size_t endQuote = line.find('"', startQuote + 1);
  if (endQuote == std::string::npos || endQuote <= startQuote) return "";
  return line.substr(startQuote + 1, endQuote - startQuote - 1);
}

std::string quotedEntityAfter(const std::string &line, const std::string &arrow) {
  size_t arrowPos = line.find(arrow);
  if (arrowPos == std::string::npos) return "";
  size_t startQuote = line.find('"', arrowPos + arrow.size());
  if (startQuote == std::string::npos) return "";
  size_t endQuote = line.find('"', startQuote + 1);
  if (endQuote == std::string::npos || endQuote <= startQuote) return "";
  return line.substr(startQuote + 1, endQuote - startQuote - 1);
}

bool linkEnabled(const std::string &line) {
  return line.find("[ENABLED]") != std::string::npos ||
         line.find("Enabled") != std::string::npos;
}

std::string findEntityByDeviceNode(const std::string &topology, const std::string &deviceNode) {
  if (deviceNode.empty()) return "";
  std::istringstream iss(topology);
  std::string line;
  std::string currentEntity;
  while (std::getline(iss, line)) {
    if (line.find("- entity ") != std::string::npos) {
      currentEntity = entityNameFromLine(line);
    } else if (!currentEntity.empty() && line.find("device node name") != std::string::npos &&
               line.find(deviceNode) != std::string::npos) {
      return currentEntity;
    }
  }
  return "";
}

std::string findUpstreamEntity(const std::string &topology, const std::string &entity) {
  if (entity.empty()) return "";
  std::istringstream iss(topology);
  std::string line;
  bool inEntity = false;
  while (std::getline(iss, line)) {
    if (line.find("- entity ") != std::string::npos) {
      inEntity = (entityNameFromLine(line) == entity);
      continue;
    }
    if (inEntity && linkEnabled(line) && line.find("<-") != std::string::npos) {
      std::string upstream = quotedEntityOnIncomingLink(line);
      if (!upstream.empty()) return upstream;
    }
  }
  return "";
}

std::string findAnyUpstreamEntity(const std::string &topology, const std::string &entity) {
  if (entity.empty()) return "";
  std::istringstream iss(topology);
  std::string line;
  bool inEntity = false;
  while (std::getline(iss, line)) {
    if (line.find("- entity ") != std::string::npos) {
      inEntity = (entityNameFromLine(line) == entity);
      continue;
    }
    if (inEntity && line.find("<-") != std::string::npos) {
      std::string upstream = quotedEntityOnIncomingLink(line);
      if (!upstream.empty()) return upstream;
    }
  }
  return "";
}

std::string findSourcePadToEntity(const std::string &topology, const std::string &entity,
                                  const std::string &downstreamEntity) {
  if (entity.empty() || downstreamEntity.empty()) return "1";
  std::istringstream iss(topology);
  std::string line;
  bool inEntity = false;
  std::string currentPad = "1";
  while (std::getline(iss, line)) {
    if (line.find("- entity ") != std::string::npos) {
      inEntity = (entityNameFromLine(line) == entity);
      continue;
    }
    if (!inEntity) continue;
    size_t padPos = line.find("pad");
    size_t sourcePos = line.find("Source");
    if (padPos != std::string::npos && sourcePos != std::string::npos) {
      size_t colon = line.find(":", padPos);
      if (colon != std::string::npos && colon > padPos + 3) {
        currentPad = trim(line.substr(padPos + 3, colon - (padPos + 3)));
      }
      continue;
    }
    if (line.find("->") != std::string::npos &&
        quotedEntityAfter(line, "->") == downstreamEntity) {
      return currentPad.empty() ? "1" : currentPad;
    }
  }
  return "1";
}

std::string findFirstEntityByKeyword(const std::string &topology, const std::vector<std::string> &keywords) {
  std::istringstream iss(topology);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find("- entity ") == std::string::npos) continue;
    std::string entity = entityNameFromLine(line);
    std::string lower = lowerCopy(entity);
    for (const auto &keyword : keywords) {
      if (lower.find(lowerCopy(keyword)) != std::string::npos) return entity;
    }
  }
  return "";
}

std::string findSensorEntity(const std::string &topology) {
  std::istringstream iss(topology);
  std::string line;
  std::string currentEntity;
  while (std::getline(iss, line)) {
    if (line.find("- entity ") != std::string::npos) {
      currentEntity = entityNameFromLine(line);
    } else if (!currentEntity.empty() && line.find("subtype Sensor") != std::string::npos) {
      return currentEntity;
    }
  }
  return findFirstEntityByKeyword(topology, {"sensor", "lt6911", "rk628"});
}

int countOccurrences(const std::string &text, const std::string &needle) {
  if (needle.empty()) return 0;
  int count = 0;
  size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

struct MipiPipelineCache {
  bool valid = false;
  std::string mediaDevice;
  std::string videoDevice;
  int width = 0;
  int height = 0;
};

std::mutex gMipiPipelineCacheMutex;
MipiPipelineCache gMipiPipelineCache;

bool isCachedMipiPipeline(const std::string &mediaDevice,
                          const std::string &videoDevice,
                          int width,
                          int height) {
  std::lock_guard<std::mutex> lock(gMipiPipelineCacheMutex);
  return gMipiPipelineCache.valid &&
         gMipiPipelineCache.mediaDevice == mediaDevice &&
         gMipiPipelineCache.videoDevice == videoDevice &&
         gMipiPipelineCache.width == width &&
         gMipiPipelineCache.height == height;
}

void rememberMipiPipeline(const std::string &mediaDevice,
                          const std::string &videoDevice,
                          int width,
                          int height) {
  std::lock_guard<std::mutex> lock(gMipiPipelineCacheMutex);
  gMipiPipelineCache.valid = true;
  gMipiPipelineCache.mediaDevice = mediaDevice;
  gMipiPipelineCache.videoDevice = videoDevice;
  gMipiPipelineCache.width = width;
  gMipiPipelineCache.height = height;
}

} // 命名空间

bool MediaController::isMediaControllerAvailable(const std::string &mediaDevice) {
#ifdef __ANDROID__
  // 检查 media 设备是否存在
  if (access(mediaDevice.c_str(), F_OK) != 0) {
    LOG_DEBUG("[采集][MediaController] Media device not found: %s", mediaDevice.c_str());
    return false;
  }

  // 检查 media-ctl 命令是否可用
  int ret = system("which media-ctl > /dev/null 2>&1");
  if (ret != 0) {
    LOG_DEBUG("[采集][MediaController] media-ctl command not available");
    return false;
  }

  return true;
#else
  (void)mediaDevice;
  return false;
#endif
}

bool MediaController::executeMediaCtl(const std::string &command, std::string *output) {
#ifdef __ANDROID__
  std::string fullCommand = "media-ctl " + command + " 2>&1";

  FILE *pipe = popen(fullCommand.c_str(), "r");
  if (!pipe) {
    LOG_ERROR("[采集][MediaController] Failed to execute: %s", fullCommand.c_str());
    return false;
  }

  char buffer[256];
  std::string localOutput;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    localOutput += buffer;
  }

  int exitCode = pclose(pipe);

  if (output) *output = localOutput;

  if (exitCode != 0) {
    LOG_WARN("[采集][MediaController] Command failed (exit=%d): %s\nOutput: %s",
             exitCode, fullCommand.c_str(), localOutput.c_str());
    return false;
  }

  LOG_DEBUG("[采集][MediaController] Command success: %s", fullCommand.c_str());
  return true;
#else
  (void)command; (void)output;
  return false;
#endif
}

bool MediaController::resetMediaLinks(const std::string &mediaDevice) {
#ifdef __ANDROID__
  if (!isMediaControllerAvailable(mediaDevice)) {
    return false;
  }
  const std::string mediaOpt = "-d " + mediaDevice;
  if (executeMediaCtl(mediaOpt + " -r")) {
    LOG_INFO("[采集][MediaController] Reset all media links on %s",
             mediaDevice.c_str());
    usleep(50000);
    return true;
  }
  LOG_WARN("[采集][MediaController] media-ctl reset links failed on %s; "
           "continue with explicit route setup",
           mediaDevice.c_str());
  return false;
#else
  (void)mediaDevice;
  return false;
#endif
}

bool MediaController::setupMIPIPipeline(const std::string &mediaDevice,
                                        const std::string &videoDevice,
                                        int width,
                                        int height) {
#ifdef __ANDROID__
  LOG_INFO("[采集][MediaController] Setting up MIPI pipeline on %s", mediaDevice.c_str());

  std::string targetVideo = videoDevice.empty() ? "/dev/video0" : videoDevice;
  const int targetWidth = width > 0 ? width : 1920;
  const int targetHeight = height > 0 ? height : 1080;
  const std::string frameSize =
      std::to_string(targetWidth) + "x" + std::to_string(targetHeight);

  if (access(mediaDevice.c_str(), F_OK) != 0) {
    LOG_DEBUG("[采集][MediaController] Media device not found: %s", mediaDevice.c_str());
    return false;
  }

  if (isCachedMipiPipeline(mediaDevice, targetVideo, targetWidth, targetHeight)) {
    LOG_INFO("[采集][MediaController] Reuse cached MIPI pipeline %s %s %s",
             mediaDevice.c_str(), targetVideo.c_str(), frameSize.c_str());
    return true;
  }

  if (!isMediaControllerAvailable(mediaDevice)) {
    LOG_WARN("[采集][MediaController] Media controller not available");
    return false;
  }

  std::string mediaOpt = "-d " + mediaDevice;

  std::string topology;
  if (!executeMediaCtl(mediaOpt + " -p", &topology)) {
    LOG_WARN("[采集][MediaController] Failed to read media topology from %s", mediaDevice.c_str());
    return false;
  }

  std::string streamEntity = findEntityByDeviceNode(topology, targetVideo);
  if (streamEntity.empty()) {
    streamEntity = findEntityByDeviceNode(topology, basenameOf(targetVideo));
  }
  if (streamEntity.empty()) {
    streamEntity = findFirstEntityByKeyword(topology, {"stream_cif_mipi_id0", "stream_cif_mipi"});
  }

  std::string enabledMipiEntity = findUpstreamEntity(topology, streamEntity);
  std::string mipiEntity = enabledMipiEntity;
  if (mipiEntity.empty()) {
    mipiEntity = findAnyUpstreamEntity(topology, streamEntity);
  }
  if (mipiEntity.empty()) {
    mipiEntity = findFirstEntityByKeyword(topology, {"mipi-csi2", "mipi_csi2"});
  }

  std::string enabledDphyEntity = findUpstreamEntity(topology, enabledMipiEntity);
  std::string dphyEntity = enabledDphyEntity;
  if (dphyEntity.empty()) {
    dphyEntity = findUpstreamEntity(topology, mipiEntity);
  }
  if (dphyEntity.empty()) {
    dphyEntity = findFirstEntityByKeyword(topology, {"csi2-dphy", "csi2_dphy", "dphy"});
  }

  std::string enabledSensorEntity = findUpstreamEntity(topology, enabledDphyEntity);
  std::string sensorEntity = enabledSensorEntity;
  if (sensorEntity.empty()) {
    sensorEntity = findUpstreamEntity(topology, dphyEntity);
  }
  if (sensorEntity.empty()) {
    sensorEntity = findSensorEntity(topology);
  }

  if (sensorEntity.empty() || dphyEntity.empty() || mipiEntity.empty() || streamEntity.empty()) {
    LOG_WARN("[采集][MediaController] Incomplete MIPI topology on %s: sensor='%s' dphy='%s' mipi='%s' stream='%s' video='%s'",
             mediaDevice.c_str(), sensorEntity.c_str(), dphyEntity.c_str(), mipiEntity.c_str(),
             streamEntity.c_str(), targetVideo.c_str());
    return false;
  }

  LOG_INFO("[采集][MediaController] Dynamic MIPI route: '%s' -> '%s' -> '%s' -> '%s' (%s)",
           sensorEntity.c_str(), dphyEntity.c_str(), mipiEntity.c_str(), streamEntity.c_str(),
           targetVideo.c_str());

  const bool routeAlreadyEnabled =
      !enabledSensorEntity.empty() && !enabledDphyEntity.empty() &&
      !enabledMipiEntity.empty() && enabledSensorEntity == sensorEntity &&
      enabledDphyEntity == dphyEntity && enabledMipiEntity == mipiEntity;
  const bool routeFormatAlreadySet =
      routeAlreadyEnabled &&
      countOccurrences(topology, "fmt:UYVY2X8/" + frameSize) >= 3;

  if (routeFormatAlreadySet) {
    rememberMipiPipeline(mediaDevice, targetVideo, targetWidth, targetHeight);
    LOG_INFO("[采集][MediaController] MIPI route already enabled/formatted; skip media-ctl relink");
    return true;
  }

  bool linkOk = routeAlreadyEnabled;
  auto linkRoute = [&]() {
    bool ok = true;
    if (!executeMediaCtl(mediaOpt + " -l '\"" + sensorEntity + "\":0->\"" + dphyEntity + "\":0[1]'")) {
      LOG_WARN("[采集][MediaController] Failed to link sensor to dphy");
      ok = false;
    }
    if (!executeMediaCtl(mediaOpt + " -l '\"" + dphyEntity + "\":1->\"" + mipiEntity + "\":0[1]'")) {
      LOG_WARN("[采集][MediaController] Failed to link dphy to mipi-csi2");
      ok = false;
    }
    std::string mipiSource = findSourcePadToEntity(topology, mipiEntity, streamEntity);
    if (!executeMediaCtl(mediaOpt + " -l '\"" + mipiEntity + "\":" + mipiSource + "->\"" + streamEntity + "\":0[1]'")) {
      LOG_WARN("[采集][MediaController] Failed to link mipi-csi2 to video0");
      ok = false;
    }
    return ok;
  };

  std::string mipiSource = findSourcePadToEntity(topology, mipiEntity, streamEntity);
  if (!routeAlreadyEnabled) {
    linkOk = linkRoute();
    if (!linkOk) {
      LOG_WARN("[采集][MediaController] Direct MIPI relink failed; reset graph and retry once");
      resetMediaLinks(mediaDevice);
      linkOk = linkRoute();
    }
  }

  // 6. 设置全链路格式 (UYVY)
  executeMediaCtl(mediaOpt + " --set-v4l2 '\"" + sensorEntity + "\":0[fmt:UYVY2X8/" + frameSize + "]'");
  executeMediaCtl(mediaOpt + " --set-v4l2 '\"" + dphyEntity + "\":0[fmt:UYVY2X8/" + frameSize + "]'");
  executeMediaCtl(mediaOpt + " --set-v4l2 '\"" + dphyEntity + "\":1[fmt:UYVY2X8/" + frameSize + "]'");
  executeMediaCtl(mediaOpt + " --set-v4l2 '\"" + mipiEntity + "\":0[fmt:UYVY2X8/" + frameSize + "]'");
  executeMediaCtl(mediaOpt + " --set-v4l2 '\"" + mipiEntity + "\":" + mipiSource + "[fmt:UYVY2X8/" + frameSize + "]'");

  if (linkOk || routeAlreadyEnabled) {
    rememberMipiPipeline(mediaDevice, targetVideo, targetWidth, targetHeight);
  }
  LOG_INFO("[采集][MediaController] MIPI pipeline setup completed");
  return true;
#else
  (void)mediaDevice; (void)videoDevice;
  LOG_WARN("[采集][MediaController] Media controller only supported on Android");
  return false;
#endif
}

void MediaController::invalidateMipiPipelineCache() {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(gMipiPipelineCacheMutex);
  if (gMipiPipelineCache.valid) {
    LOG_WARN("[采集][MediaController] Invalidate cached MIPI pipeline");
  }
  gMipiPipelineCache = MipiPipelineCache{};
#endif
}

} // 命名空间 hsvj
