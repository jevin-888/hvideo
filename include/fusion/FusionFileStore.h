#ifndef HSVJ_FUSION_FILE_STORE_H
#define HSVJ_FUSION_FILE_STORE_H

#include "fusion/FusionTypes.h"
#include <string>

namespace hsvj::fusion {

struct FusionFilePaths {
  std::string directory;
  std::string binaryPath;
  std::string maskPath;
  std::string correctionPath;
  std::string statePath;
};

FusionFilePaths buildFusionFilePaths(const std::string &rootPath);
bool loadFusionFiles(const std::string &rootPath, int expectedRegionCount,
                     int canvasWidth, int canvasHeight,
                     FusionProjectState *state, std::string *errorMessage = nullptr);
bool saveFusionFiles(const std::string &rootPath, const FusionProjectState &state,
                     int regionCount, int canvasWidth, int canvasHeight,
                     std::string *errorMessage = nullptr,
                     FusionFilePaths *savedPaths = nullptr);
bool deleteFusionFiles(const std::string &rootPath,
                       std::string *errorMessage = nullptr,
                       FusionFilePaths *deletedPaths = nullptr);
bool removeFusionFromMatrixConfig(const std::string &configPath,
                                  std::string *errorMessage = nullptr);

} // 命名空间 hsvj::fusion

#endif // 结束 HSVJ_FUSION_FILE_STORE_H
