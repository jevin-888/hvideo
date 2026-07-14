/**
 * @file HttpServer_System.cpp（文件名）
 * @brief 系统管理 API 实现
 * 
 * 本文件包含系统管理相关的 API 路由注册
 */

#include "HttpServer.h"
#include "network/DeviceDiscoveryService.h"
#include "utils/FileUtils.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/LicenseManager.h"
#include "core/PeripheralManager.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "decoder/core/DecoderCore.h"
#include "network/NetworkManager.h"
#include "database/PlaylistDatabase.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include "utils/SystemUtils.h"
#include "utils/HttpClient.h"
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef __ANDROID__
std::string controlJavaMirrorService(const std::string& action, int layerId);
std::string controlJavaMirrorService(const std::string& action, int layerId,
                                     const std::string& payload);
extern "C" void callJavaApplyNetworkIpConfig(const std::string& mode, const std::string& staticIp,
                                             const std::string& gateway, const std::string& dns);
extern "C" void callJavaApplyPowerSchedule(bool scheduleEnabled, bool powerOnEnabled,
                                           const std::string& powerOnDate, const std::string& powerOnTime,
                                           bool powerOffEnabled, const std::string& powerOffDate,
                                           const std::string& powerOffTime);
std::string getJavaDeviceHsName();
bool setJavaDeviceHsName(const std::string& name, std::string& error);
bool sendJavaBootLogoChange(int slot, std::string& error);
#endif

namespace {
namespace fs = std::filesystem;


static std::string trimCopy(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), value.end());
  return value;
}

static std::string getSystemDeviceHsName() {
#ifdef __ANDROID__
  return trimCopy(getJavaDeviceHsName());
#else
  return "";
#endif
}

static std::string readCommandOutput(const std::string& command) {
  std::string output;
#ifdef _WIN32
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) return output;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return trimCopy(output);
}

static std::string getDisplayIp(hsvj::SystemConfig* systemConfig) {
  if (systemConfig && systemConfig->getNetworkIpMode() == "static" &&
      !systemConfig->getNetworkStaticIp().empty()) {
    return systemConfig->getNetworkStaticIp();
  }
  return hsvj::SystemUtils::getLocalIp();
}

static void showNetworkIpHintAfterSave(hsvj::Engine* engine, hsvj::SystemConfig* systemConfig) {
  if (!engine || !systemConfig) return;
  std::string mode = systemConfig->getNetworkIpMode();
  if (mode == "static") {
    engine->showNetworkIpHint(mode, systemConfig->getNetworkStaticIp());
    return;
  }
  std::thread([engine, mode]() {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    engine->showNetworkIpHint(mode, hsvj::SystemUtils::getLocalIp());
  }).detach();
}

static bool resolveLogFilePath(const std::string& date, const std::string& name,
                               fs::path& outPath) {
  if (date.empty() || name.empty() || date.find("..") != std::string::npos ||
      name.find("..") != std::string::npos || name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    return false;
  }
  fs::path root = fs::weakly_canonical(fs::path(hsvj::Logger::getLogDir()));
  fs::path target = fs::weakly_canonical(root / date / name);
  std::string rootStr = root.string();
  std::string targetStr = target.string();
  if (targetStr.compare(0, rootStr.size(), rootStr) != 0 || !fs::is_regular_file(target)) {
    return false;
  }
  outPath = target;
  return true;
}

static bool resolveLogDatePath(const std::string& date, fs::path& outPath) {
  if (date.empty() || date.find("..") != std::string::npos ||
      date.find('/') != std::string::npos || date.find('\\') != std::string::npos) {
    return false;
  }
  fs::path root = fs::weakly_canonical(fs::path(hsvj::Logger::getLogDir()));
  fs::path target = fs::weakly_canonical(root / date);
  std::string rootStr = root.string();
  std::string targetStr = target.string();
  if (targetStr.compare(0, rootStr.size(), rootStr) != 0 || !fs::is_directory(target)) {
    return false;
  }
  outPath = target;
  return true;
}

// Base64 解码，用于 sync/receive 的 content_base64
static std::string base64Decode(const std::string &in) {
  std::string out;
  std::vector<int> T(256, -1);
  for (int i = 0; i < 64; i++)
    T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
  int val = 0, valb = -8;
  for (unsigned char c : in) {
    if (T[c] == -1) break;
    val = (val << 6) + T[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

// 限制：单文件 50MB，单次最多 5000 个文件
constexpr size_t kSyncMaxFileSize = 50 * 1024 * 1024;
constexpr size_t kSyncMaxFiles = 5000;
constexpr const char* kBootLogoPackageName = "com.tyzc.resolutionratio";
constexpr const char* kBootLogoReceiverName =
    "com.tyzc.resolutionratio.TyzcBroadcastReceiver";
constexpr const char* kBootLogoActionPrefix = "android.intent.action.logo.change";
constexpr const char* kBootLogoCustomPath = "/sdcard/bootanimation5.zip";

static bool getNormalizedRootPath(std::string& rootPath, std::string& rootNorm) {
  rootPath = hsvj::ROOT_PATH;
  if (rootPath.empty()) return false;
  if (rootPath.back() != '/') rootPath += '/';
  rootNorm = hsvj::FileUtils::normalizePath(rootPath);
  if (rootNorm.empty()) return false;
  if (rootNorm.back() != '/') rootNorm += '/';
  return true;
}

static std::string shellQuote(const std::string& value) {
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

static bool isValidBootLogoSlot(int slot) {
  return slot >= 1 && slot <= 5;
}

static std::string bootLogoAction(int slot) {
  return std::string(kBootLogoActionPrefix) + std::to_string(slot);
}

static const char* bootLogoName(int slot) {
  switch (slot) {
    case 1: return "logo.change1";
    case 2: return "logo.change2";
    case 3: return "logo.change3";
    case 4: return "logo.change4";
    case 5: return "logo.change5";
    default: return "";
  }
}

static const char* bootLogoLabel(int slot) {
  switch (slot) {
    case 1: return "系统 Logo 1";
    case 2: return "系统 Logo 2";
    case 3: return "系统 Logo 3";
    case 4: return "系统 Logo 4";
    case 5: return "自定义 Logo";
    default: return "";
  }
}

static Json::Value bootLogoOption(int slot) {
  Json::Value item;
  item["slot"] = slot;
  item["name"] = bootLogoName(slot);
  item["label"] = bootLogoLabel(slot);
  item["action"] = bootLogoAction(slot);
  item["package"] = kBootLogoPackageName;
  item["receiver"] = kBootLogoReceiverName;
  item["custom"] = (slot == 5);
  if (slot == 5) {
    item["path"] = kBootLogoCustomPath;
  }
  return item;
}

static int parseBootLogoSlot(const Json::Value& body) {
  if (body.isMember("slot") && body["slot"].isInt()) {
    return body["slot"].asInt();
  }
  if (body.isMember("name") && body["name"].isString()) {
    std::string name = trimCopy(body["name"].asString());
    if (name.size() == 1 && name[0] >= '1' && name[0] <= '5') {
      return name[0] - '0';
    }
    for (int slot = 1; slot <= 5; ++slot) {
      if (name == bootLogoName(slot) || name == bootLogoAction(slot) ||
          name == ("change" + std::to_string(slot))) {
        return slot;
      }
    }
  }
  return 0;
}

static int runSystemShellCommand(const std::string& command, std::string& output) {
  std::string fullCommand = command + " 2>&1";
#ifdef _WIN32
  FILE* pipe = _popen(fullCommand.c_str(), "r");
#else
  FILE* pipe = popen(fullCommand.c_str(), "r");
#endif
  if (!pipe) {
    output = "无法启动命令";
    return -1;
  }
  std::array<char, 512> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
#ifdef _WIN32
  int rc = _pclose(pipe);
#else
  int rc = pclose(pipe);
  if (WIFEXITED(rc)) {
    rc = WEXITSTATUS(rc);
  }
#endif
  output = trimCopy(output);
  return rc;
}

static void scheduleSystemReboot() {
  std::string script =
      "log=/data/local/tmp/hsvj_reboot.log\n"
      "(\n"
      "  echo \"$(date '+%F %T') schedule reboot\"\n"
      "  sleep 2\n"
      "  sync\n"
      "  echo \"$(date '+%F %T') try setprop sys.powerctl reboot\"\n"
      "  setprop sys.powerctl reboot >/dev/null 2>&1 && exit 0\n"
      "  echo \"$(date '+%F %T') try svc power reboot\"\n"
      "  svc power reboot >/dev/null 2>&1 && exit 0\n"
      "  echo \"$(date '+%F %T') try reboot\"\n"
      "  reboot >/dev/null 2>&1 && exit 0\n"
      "  echo \"$(date '+%F %T') reboot command failed\"\n"
      ") >> \"$log\" 2>&1 &\n";

  std::string output;
  int rc = runSystemShellCommand("su 0 sh -c " + shellQuote(script), output);
  if (rc == 0) return;

  std::string fallbackOutput;
  runSystemShellCommand("sh -c " + shellQuote(script), fallbackOutput);
}
}  // 命名空间

// 从 multipart/form-data 中提取名为 name 的 part 的完整 body（到下一 boundary 或结尾）
static std::string extractMultipartPartBody(const std::string& body,
                                            const std::string& contentType,
                                            const std::string& name) {
  size_t bp = contentType.find("boundary=");
  if (bp == std::string::npos) return "";
  bp += 9;
  while (bp < contentType.size() && (contentType[bp] == ' ' || contentType[bp] == '"')) ++bp;
  size_t end = contentType.find_first_of(" \t\r\n;\"", bp);
  std::string boundary = (end == std::string::npos) ? contentType.substr(bp) : contentType.substr(bp, end - bp);
  while (!boundary.empty() && (boundary.back() == '\r' || boundary.back() == '\n' || boundary.back() == '"')) boundary.pop_back();
  if (boundary.empty()) return "";
  std::string firstDelim = "--" + boundary;
  std::string delim = "\r\n--" + boundary;
  size_t pos = 0;
  if (body.rfind(firstDelim, 0) == 0) {
    pos = firstDelim.size();
  }
  for (;;) {
    size_t next = body.find(delim, pos);
    std::string part = (next == std::string::npos) ? body.substr(pos) : body.substr(pos, next - pos);
    if (part.rfind("\r\n", 0) == 0) {
      part.erase(0, 2);
    }
    size_t nameStart = part.find("name=\"");
    if (nameStart != std::string::npos) {
      nameStart += 6;
      size_t nameEnd = part.find('"', nameStart);
      if (nameEnd != std::string::npos && part.substr(nameStart, nameEnd - nameStart) == name) {
        size_t valueStart = part.find("\r\n\r\n", nameEnd);
        if (valueStart != std::string::npos) {
          valueStart += 4;
          std::string value = part.substr(valueStart);
          while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
          }
          return value;
        }
      }
    }
    if (next == std::string::npos) break;
    pos = next + delim.size();
    if (pos + 2 <= body.size() && body[pos] == '-' && body[pos + 1] == '-') break;
  }
  return "";
}

void HttpServer::registerSystemRoutes() {
  auto respondMirrorResult = [this](const std::string &rawResult,
                                    HttpResponse &response) {
    Json::Value result;
    std::string errors;
    if (!hsvj::JsonUtils::parseJson(rawResult, result, errors) ||
        !result.isObject() || !result.isMember("ok") ||
        !result["ok"].isBool()) {
      setJsonErrorResponse(response, 500,
                           "Mirror service returned an invalid response");
      return;
    }
    if (!result["ok"].asBool()) {
      setJsonErrorResponse(
          response, 500,
          result.get("message", "Mirror service operation failed").asString());
      return;
    }
    if (!result.isMember("data") || !result["data"].isObject()) {
      setJsonErrorResponse(response, 500,
                           "Mirror service returned an invalid success payload");
      return;
    }
    setJsonDataResponse(response, result["data"], "镜像服务操作成功");
  };

  post("/api/v1/mirror/actions/{action}",
       [this, respondMirrorResult](const HttpRequest &request,
                                   HttpResponse &response) {
         const std::string action = request.getUrlParam("action");
         const bool supported =
             action == "status" || action == "start" || action == "stop" ||
             action == "reset_pin" || action == "android_status" ||
             action == "android_start" || action == "android_stop" ||
             action == "usb_status" || action == "usb_start" ||
             action == "usb_stop" || action == "usb_autostart_on" ||
             action == "usb_autostart_off" ||
             action == "usb_app_scene_config";
         if (!supported) {
           setJsonErrorResponse(response, 404,
                                "Unknown mirror action: " + action);
           return;
         }

         Json::Value body(Json::objectValue);
         if (!request.getBody().empty() &&
             !parseJsonBody(request, body, response)) {
           return;
         }
         if (!body.isObject()) {
           setJsonErrorResponse(response, 400,
                                "Mirror action request body must be an object");
           return;
         }
         static const char *kLegacyFields[] = {"type", "code", "param", "action"};
         for (const char *field : kLegacyFields) {
           if (body.isMember(field)) {
             setJsonErrorResponse(
                 response, 400,
                 std::string("Unsupported protocol field: ") + field);
             return;
           }
         }

         const int layerId = body.get("layerId", 0).asInt();
#ifdef __ANDROID__
         const std::string rawResult = action == "usb_app_scene_config"
             ? controlJavaMirrorService(action, layerId, request.getBody())
             : controlJavaMirrorService(action, layerId);
         respondMirrorResult(rawResult, response);
#else
         setJsonErrorResponse(
             response, 501,
             "Mirror service control is only available on Android");
#endif
       });

  get("/api/v1/logs",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        (void)request;
        Json::Value dates(Json::arrayValue);
        fs::path root(hsvj::Logger::getLogDir());
        if (fs::exists(root)) {
          std::vector<fs::directory_entry> dirs;
          for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory()) dirs.push_back(entry);
          }
          std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
            return a.path().filename().string() > b.path().filename().string();
          });
          for (const auto& dir : dirs) {
            Json::Value dateJson;
            dateJson["date"] = dir.path().filename().string();
            Json::Value files(Json::arrayValue);
            std::vector<fs::directory_entry> fileEntries;
            for (const auto& file : fs::directory_iterator(dir.path())) {
              if (file.is_regular_file()) fileEntries.push_back(file);
            }
            std::sort(fileEntries.begin(), fileEntries.end(), [](const auto& a, const auto& b) {
              return a.path().filename().string() > b.path().filename().string();
            });
            for (const auto& file : fileEntries) {
              Json::Value fileJson;
              fileJson["name"] = file.path().filename().string();
              fileJson["size"] = static_cast<Json::UInt64>(file.file_size());
              files.append(fileJson);
            }
            dateJson["files"] = files;
            dates.append(dateJson);
          }
        }
        setJsonDataResponse(response, dates, "日志列表获取成功");
      });

  get("/api/v1/logs/file",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path filePath;
        if (!resolveLogFilePath(request.getQueryParam("date"), request.getQueryParam("name"), filePath)) {
          setJsonErrorResponse(response, 404, "Log file not found");
          return;
        }
        std::ifstream file(filePath, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        response.setText(buffer.str());
      });

  get("/api/v1/logs/download",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path filePath;
        if (!resolveLogFilePath(request.getQueryParam("date"), request.getQueryParam("name"), filePath)) {
          setJsonErrorResponse(response, 404, "Log file not found");
          return;
        }
        response.setHeader("Content-Disposition",
                           "attachment; filename=\"" + filePath.filename().string() + "\"");
        response.setFile(filePath.string());
      });

  del("/api/v1/logs/file",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path filePath;
        if (!resolveLogFilePath(request.getQueryParam("date"), request.getQueryParam("name"), filePath)) {
          setJsonErrorResponse(response, 404, "Log file not found");
          return;
        }
        std::error_code ec;
        bool removed = fs::remove(filePath, ec);
        if (ec || !removed) {
          setJsonErrorResponse(response, 500, "Delete log file failed");
          return;
        }
        setJsonSuccessResponse(response, "Log file deleted");
      });

  del("/api/v1/logs/date",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path datePath;
        if (!resolveLogDatePath(request.getQueryParam("date"), datePath)) {
          setJsonErrorResponse(response, 404, "Log directory not found");
          return;
        }
        std::error_code ec;
        fs::remove_all(datePath, ec);
        if (ec) {
          setJsonErrorResponse(response, 500, "Delete log directory failed");
          return;
        }
        setJsonSuccessResponse(response, "Log directory deleted");
      });

  // 心跳检测API（仅用于连接检测，不触发任何业务逻辑）
  get("/api/v1/heartbeat",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        // 只返回简单的状态，不触发任何命令或业务逻辑
        Json::Value result;
        result["status"] = "ok";
        auto now_epoch = std::chrono::system_clock::now().time_since_epoch();
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(now_epoch);
        result["timestamp"] = static_cast<Json::Int64>(sec.count());

        setJsonDataResponse(response, result, "心跳检测成功");
      });

  // 诊断用：GET /api/v1/system/ping 返回 200，用于确认 v1 system 路由生效
  get("/api/v1/system/ping",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        Json::Value result;
        result["status"] = "pong";
        setJsonDataResponse(response, result, "ok");
      });

  // 设备信息 API（与文档 A2 一致：model/serial/cpu_serial/storage_serial/mac/fingerprint/ip）
  get("/api/v1/system/device-info",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          std::string ip = getDisplayIp(systemConfig_);
          if (ip.empty()) {
            Json::Value interfaces = hsvj::SystemUtils::getNetworkInterfaces();
            if (interfaces.isArray() && interfaces.size() > 0 && interfaces[0].isMember("ip")) {
              ip = interfaces[0]["ip"].asString();
            } else {
              ip = "127.0.0.1";
            }
          }
          Json::Value result;
          std::string deviceHsName = getSystemDeviceHsName();
          result["name"] = deviceHsName.empty() ? hsvj::SystemUtils::getDeviceName() : deviceHsName;
          result["device_hsname"] = deviceHsName;
          result["model"] = hsvj::SystemUtils::getHardwareModel();
          result["serial"] = hsvj::SystemUtils::getHardwareSerial();
          result["cpu_serial"] = hsvj::SystemUtils::getCpuSerial();
          result["storage_serial"] = hsvj::SystemUtils::getStorageSerial();
          result["mac"] = hsvj::SystemUtils::getMacAddress();
          result["fingerprint"] = hsvj::SystemUtils::generateDeviceFingerprint();
          result["ip"] = ip;

          setJsonDataResponse(response, result, "获取设备信息成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500,
                              std::string("获取设备信息失败: ") + e.what());
        }
      });

  // 设备名称修改 API：严格读写 Android Settings.System device_hsname
  post("/api/v1/system/device-alias",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value param;
         if (!parseJsonBody(request, param, response)) return;

         std::string name;
         if (param.isMember("device_hsname") && param["device_hsname"].isString()) {
           name = param["device_hsname"].asString();
         } else if (param.isMember("name") && param["name"].isString()) {
           name = param["name"].asString();
         }
         name = trimCopy(name);

         if (name.empty()) {
           setJsonErrorResponse(response, 400, "设备名称不能为空");
           return;
         }

#ifdef __ANDROID__
         std::string error;
         if (!setJavaDeviceHsName(name, error)) {
           setJsonErrorResponse(response, 500,
                                "写入系统设备名称失败: " + (error.empty() ? std::string("unknown") : error));
           return;
         }

         Json::Value data;
         data["name"] = name;
         data["device_hsname"] = name;
         setJsonDataResponse(response, data, "设备名称已写入系统");
#else
         setJsonErrorResponse(response, 501, "device_hsname 仅支持 Android 系统接口");
#endif
        });

  get("/api/v1/system/device-alias",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        std::string name = getSystemDeviceHsName();
        Json::Value data;
        data["name"] = name;
        data["device_hsname"] = name;
        setJsonDataResponse(response, data, "ok");
      });

  // 授权状态 API（与文档 A4 一致：status/customer_name/modules/enabled_layers/usage_mode/days_remaining/expiry_date/arrears_amount）
  get("/api/v1/system/license",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        if (!engine_) {
          Json::Value result;
          result["status"] = "unlicensed";
          result["customer_name"] = "";
          result["supplier_name"] = "";
          result["usage_mode"] = "";
          result["expiry_date"] = "";
          result["days_remaining"] = 0;
          result["input_channel_count"] = 0;
          result["arrears_amount"] = 0;
          result["modules"] = Json::arrayValue;
          result["enabled_layers"] = Json::arrayValue;
          setJsonDataResponse(response, result, "引擎未初始化");
          return;
        }
        hsvj::LicenseManager* lm = engine_->getLicenseManager();
        std::string browserTimeText = request.getQueryParam("browser_time");
        if (!browserTimeText.empty()) {
          int64_t browserTime = std::atoll(browserTimeText.c_str());
          lm->applyBrowserTimeHint(browserTime);
        }
        lm->triggerCloudSync();
        Json::Value result;
        int days = lm->getDaysUntilExpiry();
        if (!lm->isLicensed()) {
          result["status"] = "unlicensed";
        } else if (lm->getDaysSource() == "time_invalid" || lm->getDaysSource() == "license_duration") {
          result["status"] = "time_calibrating";
        } else {
          // 与 License管理器 保持一致：0 天即视为过期 (EXPIRED_1_15)
          result["status"] = (days <= 0) ? "expired" : "valid";
        }
        result["customer_name"] = lm->getCustomerName().empty() ? lm->getLicenseInfo() : lm->getCustomerName();
        result["supplier_name"] = lm->getSupplierName();
        result["usage_mode"] = lm->getUsageMode();
        result["expiry_date"] = lm->getExpiryDate();
        result["days_remaining"] = days;
        result["days_source"] = lm->getDaysSource();
        result["device_time_valid"] = lm->isDeviceTimeValid();
        result["input_channel_count"] = lm->getInputChannelCount();
        result["arrears_amount"] = lm->getArrearsAmount();
        Json::Value mods(Json::arrayValue);
        for (const std::string& m : lm->getModules()) {
          mods.append(m);
        }
        result["modules"] = mods;
        Json::Value layers(Json::arrayValue);
        for (int id : lm->getEnabledLayerIds()) {
          layers.append(id);
        }
        result["enabled_layers"] = layers;
        if (!lm->getPaymentQrUrl().empty()) {
          result["payment_qr_url"] = lm->getPaymentQrUrl();
        }
        setJsonDataResponse(response, result, "获取授权状态成功");
      });

  // 导出设备信息到 {ROOT_PATH}/config/device_info.json（文档 A2）
  post("/api/v1/system/device-info/export",
       [this](const HttpRequest &request, HttpResponse &response) {
         (void)request;
         try {
           std::string configDir = hsvj::CONFIG_DIR;
           if (!hsvj::FileUtils::exists(configDir)) {
             hsvj::FileUtils::createDirectory(configDir);
           }
           std::string path = configDir + "device_info.json";
           Json::Value root;
           root["version"] = 1;
           root["export_time"] = static_cast<Json::Int64>(std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count());
           Json::Value device;
           device["model"] = hsvj::SystemUtils::getHardwareModel();
           device["serial"] = hsvj::SystemUtils::getHardwareSerial();
           device["cpu_serial"] = hsvj::SystemUtils::getCpuSerial();
           device["storage_serial"] = hsvj::SystemUtils::getStorageSerial();
           device["mac"] = hsvj::SystemUtils::getMacAddress();
           device["fingerprint"] = hsvj::SystemUtils::generateDeviceFingerprint();
           root["device"] = device;
           std::string content = hsvj::JsonUtils::toString(root);
           if (hsvj::FileUtils::writeTextFile(path, content)) {
             setJsonSuccessResponse(response, "已导出至 config/device_info.json");
           } else {
             setJsonErrorResponse(response, 500, "写入文件失败");
           }
         } catch (const std::exception &e) {
           setJsonErrorResponse(response, 500, std::string("导出失败: ") + e.what());
         }
       });

  // 导入授权文件（multipart 字段 license），保存后重新加载授权（文档 A5）
  post("/api/v1/system/license/import",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!engine_) {
           setJsonErrorResponse(response, 503, "引擎未初始化");
           return;
         }
         std::string contentType = request.getHeader("Content-Type");
         std::string body = request.getBody();
         std::string licenseContent = extractMultipartPartBody(body, contentType, "license");
         if (licenseContent.empty()) {
           setJsonErrorResponse(response, 400, "请选择 license.dat 文件上传（表单字段名 license）");
           return;
         }
         std::string dir = hsvj::LICENSE_DIR;
         if (!dir.empty() && dir.back() != '/') dir += '/';
         std::string licensePath = dir + "license.dat";
         if (!hsvj::FileUtils::exists(dir)) {
           hsvj::FileUtils::createDirectory(dir);
         }
         std::string previousLicense;
         bool hadPreviousLicense = false;
         if (hsvj::FileUtils::exists(licensePath)) {
           previousLicense = hsvj::FileUtils::readTextFile(licensePath);
           hadPreviousLicense = !previousLicense.empty();
         }
         if (!hsvj::FileUtils::writeTextFile(licensePath, licenseContent)) {
           setJsonErrorResponse(response, 500, "写入授权文件失败");
           return;
         }
         if (!engine_->getLicenseManager()->reloadLicense()) {
           std::string reason = engine_->getLicenseManager()->getLastError();
           if (hadPreviousLicense) {
             hsvj::FileUtils::writeTextFile(licensePath, previousLicense);
             engine_->getLicenseManager()->reloadLicense();
           }
           std::string message = "授权文件格式无效或验证失败";
           if (!reason.empty()) {
             message += "：" + reason;
           }
           setJsonErrorResponse(response, 400, message);
           return;
         }
         engine_->refreshLicenseScreenHint();
         hsvj::NetworkManager::getInstance().refreshAudioEffectRoutes();
         setJsonSuccessResponse(response, "授权文件已导入并生效");
       });

  // 当前数据根路径（/huoshan 或 /sdcard/huoshan 等），供前端统一构建路径，避免硬编码
  get("/api/v1/system/data-root",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        std::string root = hsvj::ROOT_PATH;
        if (!root.empty() && root.back() == '/') root.pop_back();
        Json::Value result;
        result["root"] = root;
        setJsonDataResponse(response, result, "ok");
      });

  // 开机动画候选列表：严格使用系统 com.tyzc.resolutionratio 广播接口
  get("/api/v1/system/boot-animation/list",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        Json::Value result;
        result["target"] = "system_broadcast";
        result["package"] = kBootLogoPackageName;
        result["receiver"] = kBootLogoReceiverName;
        result["custom_path"] = kBootLogoCustomPath;
        result["animations"] = Json::arrayValue;
        for (int slot = 1; slot <= 5; ++slot) {
          result["animations"].append(bootLogoOption(slot));
        }
        setJsonDataResponse(response, result, "ok");
      });

  // 切换开机动画：严格发送系统 com.tyzc.resolutionratio 广播
  post("/api/v1/system/boot-animation/install",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value body;
         if (!parseJsonBody(request, body, response)) {
           return;
         }

         int slot = parseBootLogoSlot(body);
         if (!isValidBootLogoSlot(slot)) {
           setJsonErrorResponse(response, 400, "请选择 1-5 的系统开机动画编号");
           return;
         }

         if (slot == 5 && !fs::exists(kBootLogoCustomPath)) {
           setJsonErrorResponse(response, 400,
                                std::string("自定义开机动画不存在: ") + kBootLogoCustomPath);
           return;
         }

#ifdef __ANDROID__
         std::string error;
         if (!sendJavaBootLogoChange(slot, error)) {
           setJsonErrorResponse(response, 500,
                                "发送系统开机动画广播失败: " +
                                    (error.empty() ? std::string("unknown") : error));
           return;
         }

         Json::Value result = bootLogoOption(slot);
         result["sent"] = true;
         result["reboot"] = false;
         setJsonDataResponse(response, result, "已发送系统开机动画切换广播");
#else
         setJsonErrorResponse(response, 501, "开机动画系统广播仅支持 Android");
#endif
       });

  post("/api/v1/system/reboot",
       [this](const HttpRequest &request, HttpResponse &response) {
         (void)request;
         Json::Value result;
         result["reboot"] = true;
         result["reboot_delay_ms"] = 2000;
         scheduleSystemReboot();
         setJsonDataResponse(response, result, "设备即将重启");
       });

  // 系统状态API
  get("/api/v1/system/status",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 暂时未使
        if (!checkCommandRouter(response))
          return;

        // 构造系统状态命 type=0, code=0 (SystemConfig)
        Json::Value param;
        param["action"] = "status";
        std::string cmd = buildCommandJson(0, 0, param);
        executeCommandAndRespond(cmd, response);
      });

  // 获取网络接口信息和设备名称API
  get("/api/v1/system/network/info",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 暂时未使
        try {
          // 获取网络接口信息
          Json::Value interfaces = hsvj::SystemUtils::getNetworkInterfaces();

          // 获取设备名称
          std::string deviceName = hsvj::SystemUtils::getDeviceName();

          // 构建响应
          Json::Value result;
          result["device_name"] = deviceName;
          result["interfaces"] = interfaces;

          // 获取主IP地址
          std::string primaryIp = getDisplayIp(systemConfig_);
          if (interfaces.isArray() && interfaces.size() > 0) {
            if (primaryIp.empty()) primaryIp = interfaces[0]["ip"].asString();
          }
          if (primaryIp.empty()) primaryIp = "127.0.0.1";
          result["primary_ip"] = primaryIp;

          setJsonDataResponse(response, result, "获取网络信息成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500, std::string("获取网络信息失败: ") + e.what());
        }
      });

  get("/api/v1/system/network/current-ip-config",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          Json::Value data;
          data["interface"] = "eth0";
#ifdef __ANDROID__
          data["ip"] = readCommandOutput("ip -4 addr show dev eth0 | awk '/inet / {print $2; exit}' | cut -d/ -f1");
          std::string gateway = readCommandOutput("ip route show default | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          if (gateway.empty()) {
            gateway = readCommandOutput("ip route show dev eth0 | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          }
          if (gateway.empty()) {
            gateway = readCommandOutput("ip route show table main | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          }
          if (gateway.empty()) {
            gateway = readCommandOutput("ip route show table all | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          }
          data["gateway"] = gateway;
          data["routeDebug"] = readCommandOutput("ip route show default; ip route show dev eth0; ip route show table main | head -20");
          std::string dns = readCommandOutput("getprop net.dns1");
          if (dns.empty()) {
            dns = readCommandOutput("awk '/^nameserver / {print $2; exit}' /etc/resolv.conf 2>/dev/null");
          }
          data["dns"] = dns;
#else
          data["ip"] = "";
          data["gateway"] = "";
          data["dns"] = "";
#endif
          setJsonDataResponse(response, data, "获取当前网卡配置成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500, std::string("获取当前网卡配置失败: ") + e.what());
        }
      });

  // 局域网同型号设备列表（扫描本网段，过滤 model 一致且排除本机）
  get("/api/v1/system/lan-devices",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          std::string myIp = hsvj::SystemUtils::getLocalIp();
          if (myIp.empty()) {
            Json::Value ifs = hsvj::SystemUtils::getNetworkInterfaces();
            if (ifs.isArray() && ifs.size() > 0 && ifs[0].isMember("ip")) {
              myIp = ifs[0]["ip"].asString();
            }
          }
          std::string myModel = hsvj::SystemUtils::getHardwareModel();
          Json::Value devices(Json::arrayValue);

          // 解析本机 IP 得到网段前缀（如 192.168.1）
          std::string prefix;
          int lastOctet = -1;
          if (!myIp.empty()) {
            size_t lastDot = myIp.rfind('.');
            if (lastDot != std::string::npos && lastDot > 0) {
              prefix = myIp.substr(0, lastDot);
              try {
                lastOctet = std::stoi(myIp.substr(lastDot + 1));
              } catch (...) {}
            }
          }
          if (prefix.empty()) {
            Json::Value result;
            result["devices"] = devices;
            setJsonDataResponse(response, result, "ok");
            return;
          }

          const int kTimeoutSec = 1;
          const int kPort = 8080;
          for (int i = 1; i <= 254; i++) {
            if (i == lastOctet) continue;
            std::string ip = prefix + "." + std::to_string(i);
            std::string url = "http://" + ip + ":" + std::to_string(kPort) + "/api/v1/system/device-info";
            std::string body = hsvj::httpGet(url, kTimeoutSec);
            if (body.empty()) continue;
            std::string err;
            Json::Value root;
            if (!hsvj::JsonUtils::parseJson(body, root, err)) continue;
            const std::vector<std::string> members = root.getMemberNames();
            if (!root.isObject() || members.size() != 3 ||
                !root.isMember("ok") || !root["ok"].isBool() ||
                !root["ok"].asBool() || !root.isMember("data") ||
                !root["data"].isObject() || !root.isMember("error") ||
                !root["error"].isNull()) {
              continue;
            }
            Json::Value data = root["data"];
            if (!data.isMember("model") || !data["model"].isString()) continue;
            if (data["model"].asString() != myModel) continue;
            Json::Value dev;
            dev["ip"] = ip;
            dev["name"] = data.isMember("device_name") && data["device_name"].isString()
                ? data["device_name"].asString()
                : (data.isMember("serial") && data["serial"].isString() ? data["serial"].asString() : ip);
            dev["model"] = data["model"].asString();
            devices.append(dev);
          }

          Json::Value result;
          result["devices"] = devices;
          setJsonDataResponse(response, result, "ok");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500,
                              std::string("扫描局域网设备失败: ") + e.what());
        }
      });

  // 设备发现信息 API（与 UDP Beacon 格式一致，缺项不上报）
  get("/api/v1/system/discovery",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          Json::Value interfaces = hsvj::SystemUtils::getNetworkInterfaces();
          std::string deviceName = hsvj::SystemUtils::getDeviceName();
          std::string primaryIp = "127.0.0.1";
          if (interfaces.isArray() && interfaces.size() > 0 &&
              interfaces[0].isMember("ip")) {
            primaryIp = interfaces[0]["ip"].asString();
          }
          int wsPort = 9898;
          if (hsvj::NetworkManager::getInstance().getWebSocketServer()) {
            wsPort = hsvj::NetworkManager::getInstance().getWebSocketPort();
          }
          Json::Value result = hsvj::DeviceDiscoveryService::buildDiscoveryJson(
              primaryIp, deviceName, hsvj::SystemUtils::getMacAddress(),
              hsvj::SystemUtils::getHardwareSerial(),
              hsvj::SystemUtils::getHardwareModel(), 8080, 8081, 9898, wsPort,
              9000, 8000);
          setJsonDataResponse(response, result, "ok");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500,
                              std::string("获取发现信息失败: ") + e.what());
        }
      });

  // 局域网配置同步：本机作为源，向指定目标设备推送（供“从该设备同步到本机”时由对方调用）
  post("/api/v1/sync/push-to",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!checkCommandRouter(response)) return;
         Json::Value body;
         if (!parseJsonBody(request, body, response)) return;
         if (!body.isMember("target_ips") || !body["target_ips"].isArray()) {
           setJsonErrorResponse(response, 400, "Missing or invalid target_ips");
           return;
         }
         if (!body.isMember("dirs") || !body["dirs"].isArray()) {
           setJsonErrorResponse(response, 400, "Missing or invalid dirs");
           return;
         }
         Json::Value param;
         param["action"] = "sync_device";
         Json::Value syncData;
         syncData["target_ips"] = body["target_ips"];
         syncData["base_path"] = "current";
         syncData["dirs"] = body["dirs"];
         syncData["timestamp"] = static_cast<Json::Int64>(std::time(nullptr));
         syncData["sync_mode"] = 0;
         param["sync_data"] = syncData;
         std::string cmd = buildCommandJson(0, 0x06, param);
         executeCommandAndRespond(cmd, response);
       });

  // 局域网配置同步：目标设备接收源设备推送的文件，写入 ROOT_PATH 下
  post("/api/v1/sync/receive",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value body;
         if (!parseJsonBody(request, body, response)) return;
         if (!body.isMember("files") || !body["files"].isArray()) {
           setJsonErrorResponse(response, 400, "Missing or invalid files array");
           return;
         }
         const Json::Value &files = body["files"];
         if (files.size() > kSyncMaxFiles) {
           setJsonErrorResponse(response, 400,
                                "Too many files (max " +
                                    std::to_string(kSyncMaxFiles) + ")");
           return;
         }
         std::string rootPath;
         std::string rootNorm;
         if (!getNormalizedRootPath(rootPath, rootNorm)) {
           setJsonErrorResponse(response, 500, "ROOT_PATH is not initialized");
           return;
         }

         int received = 0;
         Json::Value errors(Json::arrayValue);
         for (Json::ArrayIndex i = 0; i < files.size(); i++) {
           const Json::Value &f = files[i];
           if (!f.isMember("path") || !f["path"].isString() ||
               !f.isMember("content_base64") || !f["content_base64"].isString()) {
             errors.append("file[" + std::to_string(i) + "]: missing path or content_base64");
             continue;
           }
           std::string rel = f["path"].asString();
           if (rel.empty() || rel.find("..") != std::string::npos ||
               rel[0] == '/') {
             errors.append("file[" + std::to_string(i) + "]: invalid path");
             continue;
           }
           std::string fullPath =
               hsvj::FileUtils::normalizePath(hsvj::FileUtils::joinPath(rootPath, rel));
           if (fullPath.find(rootNorm) != 0) {
             errors.append("file[" + std::to_string(i) + "]: path outside ROOT_PATH");
             continue;
           }
           std::string b64 = f["content_base64"].asString();
           if (b64.size() > kSyncMaxFileSize) {
             errors.append("file[" + std::to_string(i) + "]: content too large");
             continue;
           }
           std::string content = base64Decode(b64);
           std::string parentDir = hsvj::FileUtils::getDirectory(fullPath);
           if (!parentDir.empty() && !hsvj::FileUtils::exists(parentDir)) {
             if (!hsvj::FileUtils::createDirectory(parentDir)) {
               errors.append("file[" + std::to_string(i) + "]: failed to create directory");
               continue;
             }
           }
           std::ofstream ofs(fullPath, std::ios::out | std::ios::binary | std::ios::trunc);
           if (!ofs.is_open()) {
             errors.append("file[" + std::to_string(i) + "]: failed to open for write");
             continue;
           }
           if (!content.empty()) {
             ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
           }
           ofs.close();
           if (ofs.fail()) {
             errors.append("file[" + std::to_string(i) + "]: write failed");
             continue;
           }
           received++;
         }

         Json::Value result;
         result["received"] = received;
         result["errors"] = errors;
         result["root"] = rootNorm;
         setJsonDataResponse(response, result, "ok");
       });

  // 系统资源监控API（CPU和内存使用率
  get("/api/v1/system/resources",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          Json::Value result;
          result["cpu_usage"] = hsvj::SystemUtils::getCpuUsage();
          result["cpu_cores"] = hsvj::SystemUtils::getCpuCoreCount();
          result["memory"] = hsvj::SystemUtils::getMemoryInfo();
          setJsonDataResponse(response, result, "获取系统资源信息成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500, std::string("获取系统资源信息失败: ") + e.what());
        }
      });

  // 系统设置API
  get("/api/v1/system/settings",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 暂时未使
        if (!checkCommandRouter(response))
          return;

        // 构造系统设置命
        Json::Value param;
        param["action"] = "get_settings";
        std::string cmd = buildCommandJson(0, 0, param);
        // 注意：如果CommandRouter不支持get_settings，会返回错误响应
        executeCommandAndRespond(cmd, response);
      });

  // 系统配置API（返回系统配置信息，如分辨率等）
  get("/api/v1/system/config", [this](const HttpRequest &request,
                                    HttpResponse &response) {
    (void)request;
    if (!checkCommandRouter(response)) {
      return;
    }

    Json::Value param(Json::objectValue);
    param["action"] = "status";
    const std::string cmd = buildCommandJson(0, 0, param);
    const hsvj::CommandResponse cmdResponse = commandRouter_->processCommand(cmd);

    try {
      hsvj::PeripheralManager::getInstance().broadcastResult(cmdResponse.toJson());
    } catch (const std::exception &e) {
      LOG_ERROR("Failed to broadcast system config response: %s", e.what());
    }

    if (!cmdResponse.ok) {
      setJsonErrorResponse(
          response, 500,
          cmdResponse.message.empty() ? "Failed to load system config"
                                      : cmdResponse.message);
      return;
    }

    Json::Value data(Json::nullValue);
    std::string errors;
    if (!cmdResponse.dataJson.empty() &&
        !hsvj::JsonUtils::parseJson(cmdResponse.dataJson, data, errors)) {
      setJsonErrorResponse(response, 500,
                           "System config returned invalid response data");
      return;
    }

    if (data.isObject() && data.isMember("resolution") &&
        data["resolution"].isString()) {
      const std::string resolution = data["resolution"].asString();
      const size_t spacePos = resolution.find(' ');
      if (spacePos != std::string::npos) {
        try {
          data["width"] = std::stoi(resolution.substr(0, spacePos));
          data["height"] = std::stoi(resolution.substr(spacePos + 1));
        } catch (const std::exception &) {
          LOG_WARN("Failed to parse resolution: %s", resolution.c_str());
        }
      }
    }

    setJsonDataResponse(response, data, "系统配置加载成功");
  });

  // 保存系统配置API
  put("/api/v1/system/config", [this](const HttpRequest &request,
                                   HttpResponse &response) {
    if (!checkSystemConfig(response)) {
      return;
    }

    Json::Value param;
    if (!parseJsonBody(request, param, response)) {
      return;
    }

    // 更新系统配置
    if (param.isMember("resolution") && param["resolution"].isString()) {
      hsvj::Resolution res =
          hsvj::Resolution::fromString(param["resolution"].asString());
      if (res.width > 0 && res.height > 0) {
        systemConfig_->setResolution(res);
      }
    }

    if (param.isMember("audio_type") && param["audio_type"].isInt()) {
      int audioType = param["audio_type"].asInt();
      if (audioType >= 0 && audioType <= 3) {
        LOG_WARN("audio_type=%d 已废弃并忽略；运行时音频路由由 Engine::syncAudioOutputLayer 统一控制", audioType);
      }
    }

    if (param.isMember("device_type") && param["device_type"].isInt()) {
      int deviceType = param["device_type"].asInt();
      if (deviceType >= 0) {
        systemConfig_->setDeviceType(deviceType);
      }
    }

    if (param.isMember("screen_rotate") && param["screen_rotate"].isInt()) {
      int rotate = param["screen_rotate"].asInt();
      if (rotate == 0 || rotate == 90 || rotate == 180 || rotate == 270) {
        systemConfig_->setScreenRotate(rotate);
      }
    }

    if (param.isMember("lyric_enabled") && param["lyric_enabled"].isBool()) {
      systemConfig_->setLyricEnabled(param["lyric_enabled"].asBool());
    }

    if (param.isMember("localSongFileScanEnabled") && param["localSongFileScanEnabled"].isBool()) {
      systemConfig_->setLocalSongFileScanEnabled(param["localSongFileScanEnabled"].asBool());
    }

    if (param.isMember("networkIpMode") && param["networkIpMode"].isString()) {
      systemConfig_->setNetworkIpMode(param["networkIpMode"].asString());
    }

    if (param.isMember("networkStaticIp") && param["networkStaticIp"].isString()) {
      systemConfig_->setNetworkStaticIp(param["networkStaticIp"].asString());
    }

    if (param.isMember("networkGateway") && param["networkGateway"].isString()) {
      systemConfig_->setNetworkGateway(param["networkGateway"].asString());
    }

    if (param.isMember("networkDns") && param["networkDns"].isString()) {
      systemConfig_->setNetworkDns(param["networkDns"].asString());
    }

    if (param.isMember("debugHotspotEnabled") && param["debugHotspotEnabled"].isBool()) {
      systemConfig_->setDebugHotspotEnabled(param["debugHotspotEnabled"].asBool());
    }

    if (param.isMember("powerScheduleEnabled") && param["powerScheduleEnabled"].isBool()) {
      systemConfig_->setPowerScheduleEnabled(param["powerScheduleEnabled"].asBool());
    }

    if (param.isMember("powerOnScheduleEnabled") && param["powerOnScheduleEnabled"].isBool()) {
      systemConfig_->setPowerOnScheduleEnabled(param["powerOnScheduleEnabled"].asBool());
    }

    if (param.isMember("powerOnDate") && param["powerOnDate"].isString()) {
      systemConfig_->setPowerOnDate(param["powerOnDate"].asString());
    }

    if (param.isMember("powerOnTime") && param["powerOnTime"].isString()) {
      systemConfig_->setPowerOnTime(param["powerOnTime"].asString());
    }

    if (param.isMember("powerOffScheduleEnabled") && param["powerOffScheduleEnabled"].isBool()) {
      systemConfig_->setPowerOffScheduleEnabled(param["powerOffScheduleEnabled"].asBool());
    }

    if (param.isMember("powerOffDate") && param["powerOffDate"].isString()) {
      systemConfig_->setPowerOffDate(param["powerOffDate"].asString());
    }

    if (param.isMember("powerOffTime") && param["powerOffTime"].isString()) {
      systemConfig_->setPowerOffTime(param["powerOffTime"].asString());
    }

    if (param.isMember("system_volume") && param["system_volume"].isNumeric()) {
      float vol = param["system_volume"].asFloat();
      if (vol >= 0.0f && vol <= 1.0f) {
        systemConfig_->setSystemVolume(vol);
      }
    }

    // 更新图层配置：直接调用 parseLayerConfig 解析全部字段
    if (param.isMember("layers") && param["layers"].isObject()) {
      const Json::Value &layers = param["layers"];
      for (const auto &key : layers.getMemberNames()) {
        if (key.length() > 5 && key.substr(0, 5) == "layer") {
          bool allDigits = true;
          for (size_t i = 5; i < key.length(); i++) {
            if (!std::isdigit(static_cast<unsigned char>(key[i]))) {
              allDigits = false;
              break;
            }
          }
          if (allDigits) {
            systemConfig_->parseLayerConfig(key, &layers[key]);
          }
        }
      }
    }

    // 保存配置文件
    if (systemConfig_->save(hsvj::CONFIG_PATH)) {
      if (engine_) {
        engine_->applyVodConfigNow();
        showNetworkIpHintAfterSave(engine_, systemConfig_);
      }
#ifdef __ANDROID__
      callJavaApplyNetworkIpConfig(systemConfig_->getNetworkIpMode(), systemConfig_->getNetworkStaticIp(),
                                   systemConfig_->getNetworkGateway(), systemConfig_->getNetworkDns());
      callJavaApplyPowerSchedule(systemConfig_->isPowerScheduleEnabled(),
                                 systemConfig_->isPowerOnScheduleEnabled(),
                                 systemConfig_->getPowerOnDate(),
                                 systemConfig_->getPowerOnTime(),
                                 systemConfig_->isPowerOffScheduleEnabled(),
                                 systemConfig_->getPowerOffDate(),
                                 systemConfig_->getPowerOffTime());
#endif
      setJsonSuccessResponse(response, "配置保存成功");
    } else {
      setJsonErrorResponse(response, 500, "配置保存失败");
    }
  });

    // 将图层模板添加到当前配置API
  post("/api/v1/layers/templates/{name}/add",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!checkSystemConfig(response)) {
           return;
         }

         std::string name = request.getUrlParam("name");

         // 安全检
         if (!isValidTemplateName(name, response)) {
           return;
         }

         // 读取模板文件
         std::string layerDir = hsvj::LAYER_DIR;
         std::string filePath = ensureJsonExtension(layerDir + name);

         if (!hsvj::FileUtils::exists(filePath)) {
           setJsonErrorResponse(response, 404, "Template not found: " + name);
           return;
         }

         std::string content = hsvj::FileUtils::readTextFile(filePath);

         // 解析JSON
         Json::Value templateJson;
         std::string errors;
         if (!hsvj::JsonUtils::parseJson(content, templateJson, errors)) {
           setJsonErrorResponse(response, 500,
                                "Invalid JSON in template: " + errors);
           return;
         }

         // 将模板内容添加到系统配置：使用 parseLayerConfig 解析全部字段
         for (const auto &key : templateJson.getMemberNames()) {
           if (key.length() > 5 && key.substr(0, 5) == "layer") {
             bool allDigits = true;
             for (size_t i = 5; i < key.length(); i++) {
               if (!std::isdigit(static_cast<unsigned char>(key[i]))) {
                 allDigits = false;
                 break;
               }
             }
             if (allDigits) {
               systemConfig_->parseLayerConfig(key, &templateJson[key]);
             }
           }
         }

         // 不再自动保存配置，用户需点击"保存"按钮
         setJsonSuccessResponse(response, "图层模板已添加到内存（需点击保存才写入文件）");
       });

  // 更新系统设置 API (PUT)
  put("/api/v1/system/settings",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkCommandRouter(response))
          return;

        Json::Value param;
        if (!parseJsonBody(request, param, response)) {
          return;
        }

        std::string cmd = buildCommandJson(0, 0, param);
        executeCommandAndRespond(cmd, response);
      });

  // 设置屏幕旋转 API
  post("/api/v1/settings/screen_rotate",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!checkCommandRouter(response))
           return;

         Json::Value param;
         if (!parseJsonBody(request, param, response)) {
           return;
         }

         Json::Value cmdParam;
         if (param.isMember("rotate") && param["rotate"].isInt()) {
           cmdParam["screen_rotate"] = param["rotate"].asInt();
         } else if (param.isMember("screen_rotate") &&
                    param["screen_rotate"].isInt()) {
           cmdParam["screen_rotate"] = param["screen_rotate"].asInt();
         } else {
           setJsonErrorResponse(response, 400, "Missing 'rotate' or 'screen_rotate' parameter");
           return;
         }

         std::string cmd = buildCommandJson(0, 0, cmdParam);
         executeCommandAndRespond(cmd, response);
       });

  // 获取默认配置 (config.json)，仅返回矩阵与系统配置
  get("/api/v1/config.json", [this](const HttpRequest &request, HttpResponse &response) {
      (void)request;
      std::string configPath = hsvj::CONFIG_PATH;
      Json::Value result(Json::objectValue);
      
      if (hsvj::FileUtils::exists(configPath)) {
        std::string content = hsvj::FileUtils::readTextFile(configPath);
        if (!content.empty()) {
          std::string errors;
          if (!hsvj::JsonUtils::parseJson(content, result, errors)) {
            LOG_ERROR("Failed to parse config.json: %s", errors.c_str());
            result = Json::Value(Json::objectValue);
          }
        }
      } else {
          LOG_WARN("config.json file not found at: %s", configPath.c_str());
      }

      hsvj::SystemConfig::retainOnlyConfigJsonKeys(result);
      setJsonDataResponse(response, result, "获取系统配置成功");
  });

  // 保存默认配置 (config.json)
  post("/api/v1/config.json", [this](const HttpRequest &request, HttpResponse &response) {
      Json::Value body;
      if (!parseJsonBody(request, body, response)) {
        return;
      }

      if (!body.isObject()) {
        setJsonErrorResponse(response, 400, "Configuration must be a JSON object");
        return;
      }

      hsvj::SystemConfig::retainOnlyConfigJsonKeys(body);
      std::string configPath = hsvj::CONFIG_PATH;
      std::string content = jsonToString(body);

      if (hsvj::FileUtils::writeTextFile(configPath, content)) {
        if (systemConfig_) {
          systemConfig_->load(configPath);
          // 同步 MPEG-PS 硬解开关 + 音频唇同步偏移：与 Engine 启动时同样的位置
          hsvj::DecoderCore::sMpegPsHardwareDecode.store(
              systemConfig_->isMpegPsHardwareDecode(), std::memory_order_relaxed);
          hsvj::DecoderCore::sAudioLipSyncOffsetMs.store(
              systemConfig_->getAudioLipSyncOffsetMs(), std::memory_order_relaxed);
        }
        // VOD 启用时，清理所有占用 VOD 保留图层的手动播放列表。
        if (systemConfig_ && systemConfig_->isVodEnabled() && playlistManager_ && playlistManager_->getDatabase()) {
          int vodLayerId = systemConfig_->getVodLayerId();
          int deleted = 0;
          if (vodLayerId > 0) {
            deleted += playlistManager_->getDatabase()->deletePlaylistsUsingLayer(vodLayerId);
          }
          if (deleted > 0) {
            LOG_INFO("enable_vod=1: deleted %d manual playlists using VOD reserved layers", deleted);
          }
        }
        if (engine_) {
          engine_->applyVodConfigNow();
          showNetworkIpHintAfterSave(engine_, systemConfig_);
        }
#ifdef __ANDROID__
        if (systemConfig_) {
          callJavaApplyNetworkIpConfig(systemConfig_->getNetworkIpMode(), systemConfig_->getNetworkStaticIp(),
                                       systemConfig_->getNetworkGateway(), systemConfig_->getNetworkDns());
          callJavaApplyPowerSchedule(systemConfig_->isPowerScheduleEnabled(),
                                     systemConfig_->isPowerOnScheduleEnabled(),
                                     systemConfig_->getPowerOnDate(),
                                     systemConfig_->getPowerOnTime(),
                                     systemConfig_->isPowerOffScheduleEnabled(),
                                     systemConfig_->getPowerOffDate(),
                                     systemConfig_->getPowerOffTime());
        }
#endif
        setJsonSuccessResponse(response, "系统配置保存成功");
      } else {
        setJsonErrorResponse(response, 500, "保存系统配置失败");
      }
  });
  // 上传文件到 huoshan 目录
  post("/api/v1/system/upload-file",
       [this](const HttpRequest &request, HttpResponse &response) {
         try {
           // 检查是否为 multipart/form-data
           std::string contentType = request.getHeader("Content-Type");
           if (contentType.find("multipart/form-data") == std::string::npos) {
             setJsonErrorResponse(response, 400, "需要 multipart/form-data 格式的请求");
             return;
           }

           // 解析 multipart 数据
           std::string boundary;
           size_t boundaryPos = contentType.find("boundary=");
           if (boundaryPos != std::string::npos) {
             boundary = contentType.substr(boundaryPos + 9);
             // 去除可能的引号
             if (!boundary.empty() && boundary.front() == '"') {
               boundary = boundary.substr(1);
             }
             if (!boundary.empty() && boundary.back() == '"') {
               boundary.pop_back();
             }
           } else {
             setJsonErrorResponse(response, 400, "无法解析 boundary");
             return;
           }

           // 分割 body
           std::string boundaryMarker = "--" + boundary;
           std::vector<std::string> parts;
           size_t pos = 0;
           std::string body = request.getBody();
           
           while ((pos = body.find(boundaryMarker, pos)) != std::string::npos) {
             pos += boundaryMarker.length();
             size_t endPos = body.find(boundaryMarker, pos);
             if (endPos == std::string::npos) {
               // 最后一部分（可能带 -- 结尾）
               std::string part = body.substr(pos);
               // 去除尾部的 \r\n 和 --
               while (!part.empty() && (part.back() == '\r' || part.back() == '\n' || part.back() == '-')) {
                 part.pop_back();
               }
               if (!part.empty()) {
                 parts.push_back(part);
               }
               break;
             }
             std::string part = body.substr(pos, endPos - pos);
             // 去除首尾的 \r\n
             while (!part.empty() && (part.front() == '\r' || part.front() == '\n')) {
               part.erase(part.begin());
             }
             while (!part.empty() && (part.back() == '\r' || part.back() == '\n')) {
               part.pop_back();
             }
             if (!part.empty()) {
               parts.push_back(part);
             }
             pos = endPos;
           }

           // 解析每个 part
           std::string filePath;
           std::string fileContent;
           std::string fileName;
           bool hasFilePart = false;

           for (const auto &part : parts) {
             // 查找头部和内容的分隔符（\r\n\r\n）
             size_t headerEnd = part.find("\r\n\r\n");
             if (headerEnd == std::string::npos) {
               headerEnd = part.find("\n\n");
               if (headerEnd == std::string::npos) continue;
               headerEnd += 2;
             } else {
               headerEnd += 4;
             }

             std::string headers = part.substr(0, headerEnd);
             std::string content = part.substr(headerEnd);

             // 解析 Content-Disposition 获取 filename 和 name
             size_t namePos = headers.find("name=\"");
             size_t filenamePos = headers.find("filename=\"");

             if (namePos != std::string::npos) {
               namePos += 6;
               size_t nameEnd = headers.find("\"", namePos);
               if (nameEnd != std::string::npos) {
                 std::string fieldName = headers.substr(namePos, nameEnd - namePos);
                 
                 if (fieldName == "path") {
                   filePath = trimCopy(content);
                 }
               }
             }

             if (filenamePos != std::string::npos) {
               filenamePos += 10;
               size_t filenameEnd = headers.find("\"", filenamePos);
               if (filenameEnd != std::string::npos) {
                 fileName = headers.substr(filenamePos, filenameEnd - filenamePos);
                 fileContent = content;
                 if (fileContent.size() >= 2 && fileContent.compare(fileContent.size() - 2, 2, "\r\n") == 0) {
                   fileContent.resize(fileContent.size() - 2);
                 } else if (!fileContent.empty() && fileContent.back() == '\n') {
                   fileContent.pop_back();
                 }
                 hasFilePart = true;
               }
             }
           }

           // 验证参数
           if (!hasFilePart) {
             setJsonErrorResponse(response, 400, "未找到文件内容");
             return;
           }

           if (filePath.empty()) {
             setJsonErrorResponse(response, 400, "未指定目标路径");
             return;
           }

           if (filePath.find("..") != std::string::npos || filePath[0] == '/' || filePath[0] == '\\') {
             setJsonErrorResponse(response, 400, "目标路径必须是 ROOT_PATH 下的相对路径");
             return;
           }

           std::string rootPath;
           std::string rootNorm;
           if (!getNormalizedRootPath(rootPath, rootNorm)) {
             setJsonErrorResponse(response, 500, "ROOT_PATH is not initialized");
             return;
           }
           std::string fullPath = hsvj::FileUtils::normalizePath(hsvj::FileUtils::joinPath(rootPath, filePath));
           if (fullPath.find(rootNorm) != 0) {
             setJsonErrorResponse(response, 400, "目标路径超出 ROOT_PATH");
             return;
           }
           
           // 确保目录存在
           std::string dirPath = fullPath.substr(0, fullPath.find_last_of('/'));
           if (!hsvj::FileUtils::exists(dirPath)) {
             if (!hsvj::FileUtils::createDirectory(dirPath)) {
               setJsonErrorResponse(response, 500, "无法创建目录: " + dirPath);
               return;
             }
           }

           // 写入文件
           std::ofstream outFile(fullPath, std::ios::binary);
           if (!outFile.is_open()) {
             setJsonErrorResponse(response, 500, "无法打开文件: " + fullPath);
             return;
           }

           outFile.write(fileContent.c_str(), fileContent.size());
           outFile.close();

           LOG_INFO("[Upload] 文件上传成功: %s (%zu bytes)", fullPath.c_str(), fileContent.size());
           
           Json::Value result;
           result["path"] = filePath;
           result["size"] = static_cast<Json::UInt64>(fileContent.size());
           setJsonDataResponse(response, result, "文件上传成功");

         } catch (const std::exception &e) {
           LOG_ERROR("[Upload] 文件上传失败: %s", e.what());
           setJsonErrorResponse(response, 500, std::string("上传失败: ") + e.what());
         }
       });
}
