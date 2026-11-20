/**
 * @file MediaController.cpp（文件名）
 * @brief Media Controller 配置工具实现
 */

#include "utils/MediaController.h"
#include "utils/Logger.h"
#include "utils/ProcessRunner.h"

#include <cstdlib>
#include <cctype>
#include <unistd.h>
#include <chrono>
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
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
  return line.find("[ENABLED") != std::string::npos ||
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

bool padHasFormat(const std::string &topology, const std::string &entity,
                  const std::string &pad, int width, int height) {
  std::istringstream lines(topology);
  std::string line;
  bool inEntity = false;
  bool inPad = false;
  while (std::getline(lines, line)) {
    if (line.find("- entity ") != std::string::npos) {
      inEntity = entityNameFromLine(line) == entity;
      inPad = false;
    } else if (inEntity) {
      const std::string content = trim(line);
      if (content.compare(0, 3, "pad") == 0) {
        inPad = content.compare(0, pad.size() + 4, "pad" + pad + ":") == 0;
      } else if (inPad) {
        auto pos = content.find("fmt:UYVY2X8/");
        size_t formatPrefixLength = 12;
        if (pos == std::string::npos) {
          pos = content.find("fmt:UYVY8_2X8/");
          formatPrefixLength = 14;
        }
        if (pos != std::string::npos) {
          std::istringstream dimensions(content.substr(pos + formatPrefixLength));
          int w = 0, h = 0;
          char separator = 0;
          dimensions >> w >> separator >> h;
          return !dimensions.fail() && separator == 'x' && w == width && h == height;
        }
      }
    }
  }
  return false;
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
  const auto availability = runShellCommand("command -v media-ctl", std::chrono::seconds(2), nullptr, 4096);
  if (!availability.succeeded()) {
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
  const std::string fullCommand = "media-ctl " + command;

  const auto result = runShellCommand(fullCommand, std::chrono::seconds(15), nullptr, 256 * 1024);
  if (output) *output = result.output;
  if (!result.succeeded()) {
    LOG_WARN("[采集][MediaController] Command failed (exit=%d): %s\nOutput: %s",
             result.exitCode, fullCommand.c_str(), result.output.c_str());
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
  invalidateMipiPipelineCache();
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
  LOG_WARN("[采集][MediaController] media-ctl reset links failed on %s",
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

  const bool cached = isCachedMipiPipeline(mediaDevice, targetVideo, targetWidth, targetHeight);

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
  const std::string mipiSource = findSourcePadToEntity(topology, mipiEntity, streamEntity);
  const std::vector<std::pair<std::string, std::string>> routePads = {
      {sensorEntity, "0"}, {dphyEntity, "0"}, {dphyEntity, "1"},
      {mipiEntity, "0"}, {mipiEntity, mipiSource}};
  auto formatsMatch = [&](const std::string &graph) {
    return std::all_of(routePads.begin(), routePads.end(), [&](const auto &pad) {
      return padHasFormat(graph, pad.first, pad.second, targetWidth, targetHeight);
    });
  };

  if (cached && routeAlreadyEnabled && formatsMatch(topology)) {
    rememberMipiPipeline(mediaDevice, targetVideo, targetWidth, targetHeight);
    LOG_INFO("[采集][MediaController] Reuse verified MIPI route and all pad formats");
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

  if (!routeAlreadyEnabled) {
    linkOk = linkRoute();
    if (!linkOk) {
      LOG_WARN("[采集][MediaController] Direct MIPI relink failed; reset graph and retry once");
      if (!resetMediaLinks(mediaDevice)) return false;
      linkOk = linkRoute();
    }
  }

  if (!linkOk) return false;
  for (const auto &pad : routePads) {
    if (!executeMediaCtl(mediaOpt + " --set-v4l2 '\"" + pad.first + "\":" +
                         pad.second + "[fmt:UYVY2X8/" + frameSize + "]'")) {
      LOG_WARN("[Capture][MediaController] Required pad configuration failed: %s:%s",
               pad.first.c_str(), pad.second.c_str());
      return false;
    }
  }

  std::string verifiedTopology;
  const bool readbackOk = executeMediaCtl(mediaOpt + " -p", &verifiedTopology);
  const bool streamLinkMatches =
      readbackOk && findUpstreamEntity(verifiedTopology, streamEntity) == mipiEntity;
  const bool mipiLinkMatches =
      readbackOk && findUpstreamEntity(verifiedTopology, mipiEntity) == dphyEntity;
  const bool dphyLinkMatches =
      readbackOk && findUpstreamEntity(verifiedTopology, dphyEntity) == sensorEntity;
  if (!readbackOk || !streamLinkMatches || !mipiLinkMatches || !dphyLinkMatches ||
      !formatsMatch(verifiedTopology)) {
    LOG_WARN("[Capture][MediaController] MIPI readback mismatch: topology=%s "
             "sensor-dphy=%s dphy-mipi=%s mipi-stream=%s; defer capture start",
             readbackOk ? "read" : "unavailable",
             dphyLinkMatches ? "ok" : "wrong", mipiLinkMatches ? "ok" : "wrong",
             streamLinkMatches ? "ok" : "wrong");
    for (const auto &pad : routePads) {
      if (!padHasFormat(verifiedTopology, pad.first, pad.second,
                        targetWidth, targetHeight)) {
        LOG_WARN("[Capture][MediaController] Format readback mismatch at %s:%s "
                 "expected UYVY %s",
                 pad.first.c_str(), pad.second.c_str(), frameSize.c_str());
      }
    }
    return false;
  }
  rememberMipiPipeline(mediaDevice, targetVideo, targetWidth, targetHeight);
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
