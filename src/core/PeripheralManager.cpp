/**
 * @file PeripheralManager.cpp（文件名）
 * @brief 外部外设管理 实现
 */

#include "core/PeripheralManager.h"
#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "network/Dmx512Receiver.h"
#include "network/NetworkManager.h"
#include "network/Rs232Server.h"
#include "network/Rs485Server.h"
#include "network/TcpServer.h"
#include "network/UdpServer.h"
#include "network/WebSocketServer.h"
#include "effect/EffectManager.h"
#include "layer/Layer.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "text/MessageHintRenderer.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <ctime>
#include <dirent.h>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

namespace hsvj {

namespace {
std::string bytesToHexCompact(const uint8_t *data, size_t len) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = kHex[data[i] >> 4];
    out[2 * i + 1] = kHex[data[i] & 0x0F];
  }
  return out;
}

std::vector<uint8_t> hexToBytes(const std::string &hex, std::string &error) {
  std::string compact;
  compact.reserve(hex.size());
  for (char c : hex) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-' || c == ':') {
      continue;
    }
    if (c >= '0' && c <= '9') compact.push_back(c);
    else if (c >= 'a' && c <= 'f') compact.push_back(static_cast<char>(c - 'a' + 'A'));
    else if (c >= 'A' && c <= 'F') compact.push_back(c);
    else {
      error = "invalid hex character";
      return {};
    }
  }
  if (compact.empty() || (compact.size() & 1)) {
    error = "hex length must be non-empty and even";
    return {};
  }

  std::vector<uint8_t> bytes;
  bytes.reserve(compact.size() / 2);
  for (size_t i = 0; i < compact.size(); i += 2) {
    unsigned int value = 0;
    std::stringstream ss;
    ss << std::hex << compact.substr(i, 2);
    ss >> value;
    bytes.push_back(static_cast<uint8_t>(value));
  }
  return bytes;
}

bool normalizeHexString(const std::string &hex, std::string &normalized,
                        std::string &error) {
  const std::vector<uint8_t> bytes = hexToBytes(hex, error);
  if (!error.empty()) {
    return false;
  }
  normalized = bytesToHexCompact(bytes.data(), bytes.size());
  return true;
}

std::string hexPreview(const std::vector<uint8_t> &bytes) {
  std::string preview;
  preview.reserve(bytes.size());
  for (uint8_t b : bytes) {
    preview.push_back((b >= 32 && b <= 126) ? static_cast<char>(b) : '.');
  }
  return preview;
}

bool isSerialDeviceName(const std::string &name) {
  return name.rfind("ttyS", 0) == 0 || name.rfind("ttyUSB", 0) == 0 ||
         name.rfind("ttyACM", 0) == 0 || name.rfind("ttyRS", 0) == 0 ||
         name.rfind("ttyAMA", 0) == 0 || name.rfind("ttyXRUSB", 0) == 0 ||
         name.rfind("ttySWK", 0) == 0 || name.rfind("ttysWK", 0) == 0;
}

int serialDevicePreference(const std::string &name) {
  if (name.rfind("ttySWK", 0) == 0) return 0;
  if (name.rfind("ttysWK", 0) == 0) return 10;
  return 5;
}

std::string serialDeviceKey(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode)) {
    return "chr:" + std::to_string(static_cast<unsigned long long>(st.st_rdev));
  }
  return "path:" + path;
}

std::string serialDeviceType(const std::string &name) {
  if (name == "ttySWK4" || name == "ttysWK4") {
    return "rs232";
  }
  if (name.rfind("ttySWK", 0) == 0 || name.rfind("ttysWK", 0) == 0 ||
      name.rfind("ttyRS", 0) == 0 || name.rfind("ttyXRUSB", 0) == 0) {
    return "rs485";
  }
  return "rs232";
}

std::string normalizeSerialTemplateType(const std::string &type) {
  return type == "rs485" ? "rs485" : "rs232";
}

DmxInputMode parseDmxInputMode(const std::string &mode) {
  return (mode == "external" || mode == "remote" || mode == "rs232")
             ? DmxInputMode::EXTERNAL
             : DmxInputMode::LOCAL;
}

const char *dmxInputModeToString(DmxInputMode mode) {
  return mode == DmxInputMode::EXTERNAL ? "external" : "local";
}

int clampDmxAddress(int address) {
  return std::max(1, std::min(512, address));
}

int normalizeBaudrate(int baudrate, int fallback) {
  return baudrate > 0 ? baudrate : fallback;
}

int normalizeDataBits(int dataBits) {
  return (dataBits >= 5 && dataBits <= 8) ? dataBits : 8;
}

int normalizeStopBits(int stopBits) {
  return stopBits == 2 ? 2 : 1;
}

Json::Value buildDefaultFunctionConfig() {
  Json::Value root(Json::objectValue);

  Json::Value hvac(Json::objectValue);
  hvac["enabled"] = true;
  hvac["title"] = "空调";
  hvac["default_temperature"] = 26;
  hvac["temperature_min"] = 16;
  hvac["temperature_max"] = 32;
  hvac["default_mode"] = "cool";
  hvac["default_fan_speed"] = 3;

  Json::Value hvacItems(Json::arrayValue);
  auto addHvacItem = [&](const std::string &id, const std::string &label,
                         const std::string &kind, const std::string &mode,
                         int speed, bool enabled) {
    Json::Value item(Json::objectValue);
    item["id"] = id;
    item["label"] = label;
    item["kind"] = kind;
    item["enabled"] = enabled;
    if (!mode.empty()) {
      item["mode"] = mode;
    }
    if (speed > 0) {
      item["speed"] = speed;
    }
    item["functions"] = Json::Value(Json::arrayValue);
    hvacItems.append(item);
  };
  addHvacItem("temp_down", "降温", "temperature", "", 0, true);
  addHvacItem("temp_up", "升温", "temperature", "", 0, true);
  addHvacItem("power", "打开", "power", "", 0, true);
  addHvacItem("cool", "制冷", "mode", "cool", 0, true);
  addHvacItem("heat", "制热", "mode", "heat", 0, true);
  addHvacItem("fan_low", "低风", "fan", "", 1, true);
  addHvacItem("fan_medium", "中风", "fan", "", 2, true);
  addHvacItem("fan_high", "高风", "fan", "", 3, true);
  hvac["items"] = hvacItems;
  root["hvac"] = hvac;

  Json::Value lighting(Json::objectValue);
  lighting["enabled"] = true;
  lighting["title"] = "灯光";
  Json::Value dimmer(Json::objectValue);
  dimmer["enabled"] = true;
  dimmer["default_value"] = 50;
  lighting["dimmer"] = dimmer;

  Json::Value lightItems(Json::arrayValue);
  auto addLightItem = [&](const std::string &id, const std::string &label,
                          bool defaultOn, bool enabled) {
    Json::Value item(Json::objectValue);
    item["id"] = id;
    item["label"] = label;
    item["default_on"] = defaultOn;
    item["enabled"] = enabled;
    item["functions"] = Json::Value(Json::arrayValue);
    lightItems.append(item);
  };
  addLightItem("spot", "射灯", true, true);
  addLightItem("strip", "灯带", false, true);
  addLightItem("floor", "地台灯", false, true);
  addLightItem("flood", "聚光灯", false, true);
  lighting["items"] = lightItems;
  root["lighting"] = lighting;

  return root;
}

int readLe16(const uint8_t *data) {
  return (data[0] & 0xFF) | ((data[1] & 0xFF) << 8);
}

Json::Value listSerialDevices() {
  Json::Value ports(Json::arrayValue);
  if (DIR *dir = opendir("/dev")) {
    std::map<std::string, std::string> uniqueDevices;
    while (dirent *entry = readdir(dir)) {
      std::string name = entry->d_name;
      if (!isSerialDeviceName(name)) {
        continue;
      }

      std::string path = "/dev/" + name;
      std::string key = serialDeviceKey(path);
      auto existing = uniqueDevices.find(key);
      if (existing == uniqueDevices.end()) {
        uniqueDevices[key] = path;
      } else {
        std::string existingName = existing->second.substr(existing->second.find_last_of('/') + 1);
        if (serialDevicePreference(name) < serialDevicePreference(existingName)) {
          existing->second = path;
        }
      }
    }
    closedir(dir);

    std::vector<std::string> devices;
    devices.reserve(uniqueDevices.size());
    for (const auto &kv : uniqueDevices) {
      devices.push_back(kv.second);
    }
    std::sort(devices.begin(), devices.end());

    int rs485Count = 0;
    int rs232Count = 0;
    for (const auto &device : devices) {
      Json::Value portObj(Json::objectValue);
      portObj["value"] = device;

      const std::string name = device.substr(device.find_last_of('/') + 1);
      const std::string type = serialDeviceType(name);
      portObj["type"] = type;
      portObj["role"] = type;

      std::string displayName;
      if (type == "rs485") {
        rs485Count++;
        displayName = "RS-485串口" + std::to_string(rs485Count);
      } else if (type == "rs232") {
        rs232Count++;
        displayName = "RS-232串口" + std::to_string(rs232Count);
      } else {
        displayName = "调试串口(" + device + ")";
      }
      portObj["label"] = displayName;
      ports.append(portObj);
    }
  }
  return ports;
}
} // 命名空间

PeripheralManager &PeripheralManager::getInstance() {
  static PeripheralManager instance;
  return instance;
}

std::string PeripheralManager::getDmxInputModeName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dmxInputModeToString(dmxConfig_.inputMode);
}

bool PeripheralManager::isDmxExternalInputRunning() const {
  std::lock_guard<std::mutex> lock(dmxExternalMutex_);
  return dmxExternalRs232_ && dmxExternalRs232_->isRunning();
}

PeripheralManager::~PeripheralManager() {
  stopDmxExternalInput();
  stopSerialLearnThread();
}

void PeripheralManager::initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  LOG_DEBUG("PeripheralManager: Initializing native DMX & Peripheral system...");

  rs232Config_.port = "";
  rs232Config_.baudrate = 0;
  rs232Config_.enabled = false;

  rs485Config_.port = "/dev/ttyRS485";
  rs485Config_.baudrate = 9600;

  dmxConfig_.port = "artnet";
  dmxConfig_.protocol = DmxProtocol::ARTNET;
  dmxConfig_.inputMode = DmxInputMode::LOCAL;
  dmxConfig_.externalPort = "/dev/ttySWK4";
  dmxConfig_.externalBaudrate = 115200;
  dmxConfig_.externalDataBits = 8;
  dmxConfig_.externalStopBits = 1;
  dmxConfig_.externalEnabled = true;
  dmxConfig_.externalHandleEnabled = true;
  dmxConfig_.externalMaterialEnabled = true;
  dmxConfig_.externalShowInfo = false;
  dmxConfig_.externalInfoPos = "";
  dmxConfig_.electronVersion = 1;
  dmxConfig_.effectInterval = 3000;
  dmxConfig_.startAddress = 1;
  dmxConfig_.baudrate = 250000;
  dmxConfig_.masterDimmer = 255;

  udpConfig_.bindAddress = "0.0.0.0";
  udpConfig_.bindPort = 8000;
  udpConfig_.targetAddress = "255.255.255.255";
  udpConfig_.targetPort = 8000;
  udpConfig_.multicastGroup = "239.255.0.1";
  udpConfig_.enabled = false;

  tcpConfig_.bindAddress = "0.0.0.0";
  tcpConfig_.bindPort = 9000;
  tcpConfig_.mode = "server";
  tcpConfig_.enabled = false;

  websocketConfig_.bindAddress = "0.0.0.0";
  websocketConfig_.bindPort = 8080;
  websocketConfig_.path = "/ws";
  websocketConfig_.enabled = false;

  functionConfig_ = buildDefaultFunctionConfig();

  simulationChannels_.assign(512, 0);
  simulationMode_ = false;

  applyConfigs();
}

Json::Value
PeripheralManager::processPeripheralCommand(const std::string &action,
                                            const Json::Value &param) {
  std::unique_lock<std::mutex> lock(mutex_);
  Json::Value result;
  result["ok"] = true;
  bool sendDmxExternalAddressAfterUnlock = false;

  if (action == "set_config") {
    std::string type = param["peripheral_type"].asString();
    if (type == "rs232") {
      rs232Config_.port = param["port"].asString();
      rs232Config_.baudrate = param["baudrate"].asInt();
      rs232Config_.learnMode = param.get("learn_mode", rs232Config_.learnMode).asBool();
      rs232Config_.enabled = true;
      rs232Config_.cmdMap.clear();
      if (param.isMember("commands") && param["commands"].isArray()) {
        for (const auto &cmd : param["commands"]) {
          rs232Config_.cmdMap[cmd["hex"].asString()] = cmd["action"].asString();
        }
      }
      LOG_DEBUG("PeripheralManager: RS232 updated, %zu commands mapped",
               rs232Config_.cmdMap.size());
    } else if (type == "rs485") {
      rs485Config_.port = param["port"].asString();
      rs485Config_.baudrate = param["baudrate"].asInt();
      rs485Config_.slaveId = param["slave_id"].asInt();
      rs485Config_.learnMode = param.get("learn_mode", rs485Config_.learnMode).asBool();
      rs485Config_.enabled = true;
      LOG_DEBUG("PeripheralManager: RS485 updated, SlaveID: %d",
               rs485Config_.slaveId);
    } else if (type == "dmx512") {
      const bool dmxAddressUpdated =
          param.isMember("address") || param.isMember("light");
      dmxConfig_.port = param.get("port", dmxConfig_.port).asString();
      dmxConfig_.inputMode =
          parseDmxInputMode(param.get("mode", dmxInputModeToString(dmxConfig_.inputMode))
                                .asString());
      dmxConfig_.externalPort =
          param.get("external_port",
                    param.get("port232",
                              param.get("rs232_port", dmxConfig_.externalPort)))
              .asString();
      dmxConfig_.externalBaudrate = normalizeBaudrate(
          param.get("external_baudrate",
                    param.get("baud_rate", dmxConfig_.externalBaudrate))
              .asInt(),
          115200);
      dmxConfig_.externalDataBits = normalizeDataBits(
          param.get("external_data_bit",
                    param.get("data_bit", dmxConfig_.externalDataBits))
              .asInt());
      dmxConfig_.externalStopBits = normalizeStopBits(
          param.get("external_stop_bit",
                    param.get("stop_bit", dmxConfig_.externalStopBits))
              .asInt());
      dmxConfig_.externalEnabled =
          param.get("external_enable",
                    param.get("enable", dmxConfig_.externalEnabled))
              .asBool();
      dmxConfig_.externalHandleEnabled =
          !param.get("stop_handle", !dmxConfig_.externalHandleEnabled).asBool();
      // stop_material is kept 仅 for backward-compatible payloads; material
      // CH6/CH7 素材通道始终启用。
      dmxConfig_.externalMaterialEnabled = true;
      dmxConfig_.externalShowInfo =
          param.get("dmx_info_show", dmxConfig_.externalShowInfo).asBool();
      dmxConfig_.externalInfoPos =
          param.get("dmx_info_pos", dmxConfig_.externalInfoPos).asString();
      dmxConfig_.electronVersion =
          param.get("electron_version", dmxConfig_.electronVersion).asInt();
      dmxConfig_.effectInterval =
          param.get("effect_interval", dmxConfig_.effectInterval).asInt();
      dmxConfig_.startAddress = clampDmxAddress(
          param.get("address", dmxConfig_.startAddress).asInt());
      if (param.isMember("light")) {
        dmxConfig_.startAddress = clampDmxAddress(param["light"].asInt());
      }
      if (dmxAddressUpdated && dmxConfig_.inputMode == DmxInputMode::EXTERNAL) {
        sendDmxExternalAddressAfterUnlock = true;
      }
      dmxConfig_.baudrate = 250000;
      dmxConfig_.masterDimmer =
          param.get("master", dmxConfig_.masterDimmer).asInt();
      dmxConfig_.enabled = true;
      if (dmxConfig_.port == "artnet")
        dmxConfig_.protocol = DmxProtocol::ARTNET;
      else if (dmxConfig_.port == "sacn")
        dmxConfig_.protocol = DmxProtocol::SACN;
      else
        dmxConfig_.protocol = DmxProtocol::SERIAL;
      if (systemConfig_) {
        systemConfig_->setDmxBaudRate(dmxConfig_.baudrate);
        systemConfig_->setDmxStartAddress(dmxConfig_.startAddress);
        systemConfig_->save(CONFIG_PATH);
        LOG_DEBUG("PeripheralManager: DMX baudrate %d startAddress %d saved to config",
                 dmxConfig_.baudrate, dmxConfig_.startAddress);
      }
      if (engine_ && engine_->getDmxReceiver()) {
        auto *receiver = engine_->getDmxReceiver();
        if (dmxConfig_.inputMode == DmxInputMode::EXTERNAL) {
          receiver->stop();
        } else {
          if (dmxConfig_.protocol == DmxProtocol::SERIAL) {
            receiver->setDevice(dmxConfig_.port);
          }
          if (!receiver->isRunning()) {
            receiver->start();
          }
        }
        receiver->setBaudRate(dmxConfig_.baudrate);
        receiver->setStartAddress(dmxConfig_.startAddress);
      }
      dmxConfig_.channelMap.clear();
      if (param.isMember("mappings") && param["mappings"].isArray()) {
        for (const auto &map : param["mappings"]) {
          dmxConfig_.channelMap[map["offset"].asInt()] =
              map["function"].asString();
        }
      }
      LOG_DEBUG(
          "PeripheralManager: DMX512 updated, Mode: %s, StartAddr: %d, MappedSize: %zu",
          dmxInputModeToString(dmxConfig_.inputMode), dmxConfig_.startAddress,
          dmxConfig_.channelMap.size());
    } else if (type == "udp") {
      udpConfig_.bindAddress = param.get("bind_address", "0.0.0.0").asString();
      udpConfig_.bindPort = param.get("bind_port", 8000).asInt();
      udpConfig_.targetAddress =
          param.get("target_address", "255.255.255.255").asString();
      udpConfig_.targetPort = param.get("target_port", 8000).asInt();
      udpConfig_.enabled = true;
      parseNetworkMappings(udpConfig_, param);

      // 组播：若配置了 multicast_group，加入该组播组
      std::string newGroup = param.get("multicast_group", "239.255.0.1").asString();
      if (newGroup != udpConfig_.multicastGroup) {
        if (auto *udp = NetworkManager::getInstance().getUdpServer()) {
          // 离开旧组
          if (!udpConfig_.multicastGroup.empty()) {
            udp->leaveMulticastGroup(udpConfig_.multicastGroup);
            LOG_DEBUG("PeripheralManager: UDP 离开旧组播组 %s", udpConfig_.multicastGroup.c_str());
          }
          // 加入新组
          if (!newGroup.empty()) {
            if (udp->joinMulticastGroup(newGroup)) {
              LOG_DEBUG("PeripheralManager: UDP 加入组播组 %s", newGroup.c_str());
            } else {
              LOG_WARN("PeripheralManager: UDP 加入组播组 %s 失败", newGroup.c_str());
            }
          }
        }
        udpConfig_.multicastGroup = newGroup;
      }

      LOG_DEBUG("PeripheralManager: UDP updated, Bind: %s:%d, Target: %s:%d, "
               "Multicast: %s, %zu triggers (%zu legacy cmds)",
               udpConfig_.bindAddress.c_str(), udpConfig_.bindPort,
               udpConfig_.targetAddress.c_str(), udpConfig_.targetPort,
               udpConfig_.multicastGroup.empty() ? "disabled" : udpConfig_.multicastGroup.c_str(),
               udpConfig_.triggers.size(), udpConfig_.cmdMap.size());
    } else if (type == "tcp") {
      tcpConfig_.bindAddress = param.get("bind_address", "0.0.0.0").asString();
      tcpConfig_.bindPort = param.get("bind_port", 9999).asInt();
      tcpConfig_.mode = param.get("mode", "server").asString();
      tcpConfig_.targetAddress = param.get("target_address", "").asString();
      tcpConfig_.targetPort = param.get("target_port", 0).asInt();
      tcpConfig_.enabled = true;
      parseNetworkMappings(tcpConfig_, param);
      LOG_DEBUG("PeripheralManager: TCP updated, Mode: %s, Bind: %s:%d, %zu "
               "triggers (%zu legacy cmds)",
               tcpConfig_.mode.c_str(), tcpConfig_.bindAddress.c_str(),
               tcpConfig_.bindPort, tcpConfig_.triggers.size(),
               tcpConfig_.cmdMap.size());
    } else if (type == "websocket") {
      websocketConfig_.bindAddress =
          param.get("bind_address", "0.0.0.0").asString();
      websocketConfig_.bindPort = param.get("bind_port", 8080).asInt();
      websocketConfig_.path = param.get("path", "/ws").asString();
      websocketConfig_.enabled = true;
      websocketConfig_.cmdMap.clear();
      if (param.isMember("commands") && param["commands"].isArray()) {
        for (const auto &cmd : param["commands"]) {
          websocketConfig_.cmdMap[cmd["key"].asString()] =
              cmd["action"].asString();
        }
      }
      LOG_DEBUG("PeripheralManager: WebSocket updated, Bind: %s:%d%s, %zu "
               "commands mapped",
               websocketConfig_.bindAddress.c_str(), websocketConfig_.bindPort,
               websocketConfig_.path.c_str(), websocketConfig_.cmdMap.size());
    } else if (type == "function_config") {
      Json::Value config = param;
      config.removeMember("action");
      config.removeMember("type");
      functionConfig_ = config.isObject() ? config : buildDefaultFunctionConfig();
      LOG_DEBUG("PeripheralManager: Function config updated");
    }
    if (type == "rs232" || type == "rs485") {
      const SerialConfig &cfg = (type == "rs485") ? rs485Config_ : rs232Config_;
      const std::string port = cfg.port;
      const int baudrate = cfg.baudrate > 0 ? cfg.baudrate : 9600;
      lock.unlock();
      std::string error;
      if (!startSerialLearnThread(type, port, baudrate, error)) {
        result["ok"] = false;
        result["message"] = error;
        return result;
      }
      lock.lock();
    } else if (type == "dmx512") {
      const bool useExternal = dmxConfig_.inputMode == DmxInputMode::EXTERNAL &&
                               dmxConfig_.externalEnabled;
      const std::string externalPort = dmxConfig_.externalPort;
      const int externalBaudrate = dmxConfig_.externalBaudrate;
      lock.unlock();
      if (useExternal) {
        stopSerialLearnThread("rs232");
        std::string error;
        if (!startDmxExternalInput(externalPort, externalBaudrate, error)) {
          result["ok"] = false;
          result["message"] = error;
          return result;
        }
      } else {
        stopDmxExternalInput();
      }
      lock.lock();
    }
    applyConfigs();
    // 写入磁盘（注意：当前已持有 mutex_，saveToDisk 内部不再重新加锁）
    saveToDisk();
  } else if (action == "execute_function_button") {
    const std::string region = param.get("region", "").asString();
    const std::string buttonId =
        param.get("button_id", param.get("id", "")).asString();
    Json::Value functions(Json::arrayValue);
    std::string triggerLabel = region + ":" + buttonId;

    if (functionConfig_.isObject() && functionConfig_.isMember(region) &&
        functionConfig_[region].isObject()) {
      const Json::Value &area = functionConfig_[region];
      if (area.isMember("items") && area["items"].isArray()) {
        for (const auto &item : area["items"]) {
          if (!item.isObject() ||
              item.get("id", "").asString() != buttonId) {
            continue;
          }
          triggerLabel =
              area.get("title", region).asString() + "/" +
              item.get("label", buttonId).asString();
          if (item.isMember("functions") && item["functions"].isArray()) {
            functions = item["functions"];
          }
          break;
        }
      }
    }

    std::vector<TriggerFunction> matchedFunctions;
    matchedFunctions.reserve(functions.size());
    for (const auto &f : functions) {
      if (!f.isObject()) continue;
      TriggerFunction tf;
      tf.functionId = f.get("function_id", "").asString();
      tf.functionName = f.get("function_name", tf.functionId).asString();
      tf.action = f.get("action", "").asString();
      tf.code = f.get("function_code", 2).asInt();

      Json::Value extra(Json::objectValue);
      for (const auto &key : f.getMemberNames()) {
        if (key == "function_id" || key == "function_name" ||
            key == "function_code" || key == "action") {
          continue;
        }
        extra[key] = f[key];
      }
      tf.extraParam = extra;
      matchedFunctions.push_back(std::move(tf));
    }

    result["matched"] = static_cast<Json::UInt64>(matchedFunctions.size());
    if (!matchedFunctions.empty()) {
      lock.unlock();
      executeTriggerFunctions(matchedFunctions, triggerLabel, "mobile_control",
                              "", "");
      result["executed"] = static_cast<Json::UInt64>(matchedFunctions.size());
      lock.lock();
    } else {
      result["executed"] = 0;
      result["message"] = "button has no bound functions";
    }
  } else if (action == "get_config") {
    std::string type = param["peripheral_type"].asString();
    Json::Value cfg = buildGetConfigResponse(type);
    // 将配置业务字段合并到进程内执行结果，HTTP 层再包装唯一 API envelope。
    for (const auto &k : cfg.getMemberNames()) {
      result[k] = cfg[k];
    }
  } else if (action == "list_serial_ports") {
    result["ports"] = listSerialDevices();
  } else if (action == "set_learn_mode") {
    lock.unlock();
    result = setSerialLearnMode(param);
  } else if (action == "get_templates") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    result["templates"] = buildSerialTemplatesResponse(type);
  } else if (action == "add_template") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string rawHex = param.get("data", "").asString();
    const std::string requestedName = param.get("name", "").asString();
    std::string normalizedHex;
    std::string error;
    if (!normalizeHexString(rawHex, normalizedHex, error)) {
      result["ok"] = false;
      result["message"] = error.empty() ? "invalid hex data" : error;
      return result;
    }

    auto it = std::find_if(serialTemplates_.begin(), serialTemplates_.end(),
                           [&](const SerialTemplate &tpl) {
                             const std::string tplType =
                                 tpl.type.empty()
                                     ? "rs232"
                                     : normalizeSerialTemplateType(tpl.type);
                             return (tpl.type.empty() || tplType == type) &&
                                    (tpl.code == normalizedHex ||
                                     tpl.data == normalizedHex);
                           });

    if (it != serialTemplates_.end()) {
      bool changed = false;
      if (it->type != type) {
        it->type = type;
        changed = true;
      }
      if (it->code != normalizedHex) {
        it->code = normalizedHex;
        changed = true;
      }
      if (it->data != normalizedHex) {
        it->data = normalizedHex;
        changed = true;
      }
      if (!requestedName.empty() && it->name != requestedName) {
        it->name = requestedName;
        changed = true;
      }
      if (changed) {
        saveToDisk();
      }
      result["created"] = false;
      result["template_code"] = it->code;
      result["code"] = it->code;
      result["data"] = it->data;
      result["name"] = it->name;
      result["type"] = type;
      return result;
    }

    SerialTemplate tpl;
    tpl.type = type;
    tpl.code = normalizedHex;
    tpl.data = normalizedHex;
    tpl.name = requestedName.empty()
                   ? ("手动模板" + std::to_string(serialTemplates_.size() + 1))
                   : requestedName;
    tpl.functions = Json::Value(Json::arrayValue);
    serialTemplates_.push_back(std::move(tpl));
    saveToDisk();

    result["created"] = true;
    result["template_code"] = normalizedHex;
    result["code"] = normalizedHex;
    result["data"] = normalizedHex;
    result["name"] = serialTemplates_.back().name;
    result["type"] = type;
  } else if (action == "clear_templates") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    serialTemplates_.erase(
        std::remove_if(serialTemplates_.begin(), serialTemplates_.end(),
                       [&](const SerialTemplate &tpl) {
                         const std::string tplType = tpl.type.empty()
                                                         ? "rs232"
                                                         : normalizeSerialTemplateType(tpl.type);
                         return tpl.type.empty() || tplType == type;
                       }),
        serialTemplates_.end());
    saveToDisk();
    result["count"] = static_cast<Json::UInt64>(
        buildSerialTemplatesResponse(type).size());
  } else if (action == "delete_template") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    const std::string data = param.get("data", "").asString();
    const size_t before = serialTemplates_.size();
    serialTemplates_.erase(
        std::remove_if(serialTemplates_.begin(), serialTemplates_.end(),
                       [&](const SerialTemplate &tpl) {
                         const std::string tplType = tpl.type.empty()
                                                         ? "rs232"
                                                         : normalizeSerialTemplateType(tpl.type);
                         const bool sameTemplate =
                             (!code.empty() && (tpl.code == code || tpl.data == code)) ||
                             (!data.empty() && (tpl.code == data || tpl.data == data));
                         return (tpl.type.empty() || tplType == type) &&
                                sameTemplate;
                       }),
        serialTemplates_.end());
    const size_t deleted = before - serialTemplates_.size();
    result["deleted_count"] = static_cast<Json::UInt64>(deleted);
    result["count"] = static_cast<Json::UInt64>(
        buildSerialTemplatesResponse(type).size());
    if (deleted == 0) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      saveToDisk();
    }
  } else if (action == "rename_template") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    const std::string name = param.get("name", "").asString();
    bool found = false;
    for (auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == type) &&
          (tpl.code == code || tpl.data == code)) {
        tpl.name = name;
        tpl.type = type;
        found = true;
        break;
      }
    }
    if (!found) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      saveToDisk();
      result["name"] = name;
    }
  } else if (action == "get_template_functions") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    bool found = false;
    for (const auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == type) &&
          (tpl.code == code || tpl.data == code)) {
        result["functions"] = tpl.functions.isArray()
                                  ? tpl.functions
                                  : Json::Value(Json::arrayValue);
        result["count"] = static_cast<Json::UInt64>(result["functions"].size());
        found = true;
        break;
      }
    }
    if (!found) {
      result["ok"] = false;
      result["message"] = "template not found";
    }
  } else if (action == "save_template_functions") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    bool found = false;
    for (auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == type) &&
          (tpl.code == code || tpl.data == code)) {
        tpl.type = type;
        tpl.functions = param.get("functions", Json::Value(Json::arrayValue));
        found = true;
        result["count"] = static_cast<Json::UInt64>(tpl.functions.size());
        break;
      }
    }
    if (!found) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      saveToDisk();
      result["template_code"] = code;
      result["type"] = type;
    }
  } else if (action == "forward_to_serial") {
    const std::string code = param.get("template_code", "").asString();
    const std::string serialType =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const SerialConfig &cfg = (serialType == "rs485") ? rs485Config_ : rs232Config_;
    std::string port = param.get("port", cfg.port).asString();
    int baudrate = param.get("baudrate", cfg.baudrate > 0 ? cfg.baudrate : 9600).asInt();
    std::string hex;
    for (auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == serialType) &&
          (tpl.code == code || tpl.data == code)) {
        tpl.type = serialType;
        tpl.forwardToSerial = true;
        hex = tpl.data;
        break;
      }
    }
    if (hex.empty()) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      lock.unlock();
      std::string error;
      if (!writeHexToSerial(serialType, port, baudrate, hex, error)) {
        result["ok"] = false;
        result["message"] = error;
      } else {
        result["sent_hex"] = hex;
        lock.lock();
        saveToDisk();
      }
    }
  } else if (action == "send_test") {
    std::string target = param["target"].asString();
    std::string data = param["data"].asString();
    result["reply"] = sendTest(target, data);
  } else if (action == "set_master") {
    int val = param["value"].asInt();
    dmxConfig_.masterDimmer = std::clamp(val, 0, 255);
    updateDmxOutput();
  } else if (action == "set_channel") {
    int offset = param["offset"].asInt();
    int value = param["value"].asInt();
    const bool showInfo =
        param.get("show_info", dmxConfig_.externalShowInfo).asBool();
    if (offset >= 0 && offset < 512) {
      if (simulationChannels_.size() < 512) {
        simulationChannels_.assign(512, 0);
      }
      simulationChannels_[offset] =
          static_cast<uint8_t>(std::clamp(value, 0, 255));
      simulationMode_ = true;
      LOG_DEBUG(
          "PeripheralManager: Simulation set CH-%d = %d (Mode: SIMULATION)",
          offset + 1, static_cast<int>(simulationChannels_[offset]));
      if (showInfo) {
        showDmxChannelInfoLocked();
      }
    }
  } else if (action == "clear_simulation") {
    simulationMode_ = false;
    LOG_DEBUG(
        "PeripheralManager: Simulation cleared, switched to REAL-TIME mode");
  } else if (action == "ctrl512" || action == "dmx512_legacy") {
    lock.unlock();
    handleLegacyDmxCommand(param, result);
    return result;
  }

  if (sendDmxExternalAddressAfterUnlock) {
    if (lock.owns_lock()) {
      lock.unlock();
    }
    writeDmxExternalAddressCommand();
  }

  return result;
}

// =====================================================================
//  UDP/TCP 触发墙板 —— 解析、匹配与派发
// =====================================================================

void PeripheralManager::parseNetworkMappings(NetworkConfig &cfg,
                                             const Json::Value &param) {
  cfg.cmdMap.clear();
  cfg.triggers.clear();

  auto compactHexForTriggerRepair = [](const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
      char c = value[i];
      if (c == '0' && i + 1 < value.size() &&
          (value[i + 1] == 'x' || value[i + 1] == 'X')) {
        ++i;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '-' || c == ':') continue;
      if (c >= '0' && c <= '9') out.push_back(c);
      else if (c >= 'a' && c <= 'f') out.push_back(static_cast<char>(c - 'a' + 'A'));
      else if (c >= 'A' && c <= 'F') out.push_back(c);
      else return std::string();
    }
    if (out.size() < 4 || (out.size() & 1)) return std::string();
    return out;
  };

  auto looksLikeRowNumber = [](const std::string &value) {
    if (value.empty() || value.size() > 3 || value[0] == '0') return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return c >= '0' && c <= '9';
    });
  };

  auto repairTriggerFromName = [&](std::string trigger, const Json::Value &m) {
    const std::string name = m.get("name", "").asString();
    if (looksLikeRowNumber(trigger) && !name.empty() && name != trigger) {
      const std::string nameHex = compactHexForTriggerRepair(name);
      if (!nameHex.empty()) {
        LOG_WARN("[PeripheralManager] 修正网络触发配置：trigger=%s name=%s -> trigger=%s",
                 trigger.c_str(), name.c_str(), nameHex.c_str());
        trigger = nameHex;
      }
    }
    return trigger;
  };

  // 优先解析新 schema：mappings[]，每项形如 {trigger, name, functions:[{...}]}
  if (param.isMember("mappings") && param["mappings"].isArray()) {
    for (const auto &m : param["mappings"]) {
      if (!m.isObject() || !m.isMember("trigger")) continue;
      std::string triggerStr = repairTriggerFromName(m["trigger"].asString(), m);
      if (triggerStr.empty()) continue;

      TriggerEntry entry;
      entry.trigger = triggerStr;
      entry.name = m.get("name", triggerStr).asString();

      if (m.isMember("functions") && m["functions"].isArray()) {
        for (const auto &f : m["functions"]) {
          if (!f.isObject()) continue;
          TriggerFunction tf;
          tf.functionId = f.get("function_id", "").asString();
          tf.functionName = f.get("function_name", tf.functionId).asString();
          tf.action = f.get("action", "").asString();
          tf.code = f.get("function_code", 2).asInt();

          // extraParam = function 对象本身（去掉元字段，剩下的全部带上）
          Json::Value extra(Json::objectValue);
          for (const auto &key : f.getMemberNames()) {
            if (key == "function_id" || key == "function_name" ||
                key == "function_code" || key == "action") {
              continue;
            }
            extra[key] = f[key];
          }
          tf.extraParam = extra;
          entry.functions.push_back(std::move(tf));
        }
      } else if (m.isMember("action")) {
        // 兼容：单 function 简写为 mapping.action
        TriggerFunction tf;
        const Json::Value &actionValue = m["action"];
        if (actionValue.isObject()) {
          tf.functionId = actionValue.get("function_id",
                              actionValue.get("id",
                                  actionValue.get("action", ""))).asString();
          tf.functionName = actionValue.get("function_name",
                                actionValue.get("name", tf.functionId)).asString();
          tf.action = actionValue.get("action", "").asString();
          tf.code = actionValue.get("function_code",
                        actionValue.get("code", 2)).asInt();

          Json::Value extra(Json::objectValue);
          for (const auto &key : actionValue.getMemberNames()) {
            if (key == "function_id" || key == "function_name" ||
                key == "function_code" || key == "action" ||
                key == "id" || key == "name" || key == "code") {
              continue;
            }
            extra[key] = actionValue[key];
          }
          tf.extraParam = extra;
        } else {
          tf.action = actionValue.asString();
          tf.functionId = tf.action;
          tf.functionName = tf.action;
          tf.code = m.get("function_code", 2).asInt();
        }
        entry.functions.push_back(std::move(tf));
      }

      // 同步写入 cmdMap 提供旧调用路径的兼容性（仅在单 function 时有效）
      if (entry.functions.size() == 1) {
        cfg.cmdMap[triggerStr] = entry.functions[0].action;
      }
      cfg.triggers.push_back(std::move(entry));
    }
    return;
  }

  // 旧 schema：commands[] = [{hex/key, action}]
  if (param.isMember("commands") && param["commands"].isArray()) {
    for (const auto &cmd : param["commands"]) {
      std::string trigger;
      if (cmd.isMember("hex")) trigger = cmd["hex"].asString();
      else if (cmd.isMember("key")) trigger = cmd["key"].asString();
      else if (cmd.isMember("trigger")) trigger = cmd["trigger"].asString();
      if (trigger.empty()) continue;

      TriggerEntry entry;
      entry.trigger = trigger;
      entry.name = trigger;
      TriggerFunction tf;
      const Json::Value &actionValue = cmd["action"];
      if (actionValue.isObject()) {
        tf.functionId = actionValue.get("function_id",
                            actionValue.get("id",
                                actionValue.get("action", ""))).asString();
        tf.functionName = actionValue.get("function_name",
                              actionValue.get("name", tf.functionId)).asString();
        tf.action = actionValue.get("action", "").asString();
        tf.code = actionValue.get("function_code",
                      actionValue.get("code", 2)).asInt();

        Json::Value extra(Json::objectValue);
        for (const auto &key : actionValue.getMemberNames()) {
          if (key == "function_id" || key == "function_name" ||
              key == "function_code" || key == "action" ||
              key == "id" || key == "name" || key == "code") {
            continue;
          }
          extra[key] = actionValue[key];
        }
        tf.extraParam = extra;
        cfg.cmdMap[trigger] = tf.action;
      } else {
        std::string actionStr = cmd.get("action", "").asString();
        cfg.cmdMap[trigger] = actionStr;
        tf.action = actionStr;
        tf.functionId = actionStr;
        tf.functionName = actionStr;
        tf.code = 2;
      }
      entry.functions.push_back(std::move(tf));
      cfg.triggers.push_back(std::move(entry));
    }
  }
}

namespace {

// 判断字符串是否是"纯十六进制 + 偶数长度"（允许可选 0x/0X 前缀与空白分隔）
// 返回规范化后的大写紧凑 hex 字符串；不是则返回空字符串。
// 例： "FF01" -> "FF01"；"0xff 01" -> "FF01"；"hello" -> ""
static std::string normalizeHexTrigger(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
  for (; i < s.size(); ++i) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '-' || c == ':') continue; // 允许分隔符
    if (c >= '0' && c <= '9') out += c;
    else if (c >= 'a' && c <= 'f') out += static_cast<char>(c - 'a' + 'A');
    else if (c >= 'A' && c <= 'F') out += c;
    else return {};
  }
  if (out.empty() || (out.size() & 1)) return {};
  return out;
}

// 把二进制 bytes 转成大写紧凑 hex 字符串（"\xFF\x01" -> "FF01"）
static std::string bytesToHex(const std::string &data) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.resize(data.size() * 2);
  for (size_t i = 0; i < data.size(); ++i) {
    unsigned char b = static_cast<unsigned char>(data[i]);
    out[2 * i] = kHex[b >> 4];
    out[2 * i + 1] = kHex[b & 0x0F];
  }
  return out;
}

} // 命名空间

bool PeripheralManager::isCommandDispatchBlocked() const {
  return commandRouter_ && commandRouter_->isMirroringCommandBlocked();
}

std::string PeripheralManager::commandDispatchBlockedResponseJson() const {
  if (!commandRouter_) {
    return "{\"ok\":false,\"message\":\"正在投屏，请先结束投屏\"}";
  }
  return commandRouter_->buildMirroringBlockedResponse().toJson();
}

bool PeripheralManager::dispatchNetworkTrigger(PeripheralType protocol,
                                               const std::string &data) {
  if (data.empty()) return false;
  if (isCommandDispatchBlocked()) {
    const std::string response = commandDispatchBlockedResponseJson();
    broadcastResult(response);
    LOG_WARN("[PeripheralManager] 投屏中，阻断外设网络触发");
    return true;
  }

  std::vector<TriggerFunction> matchedFunctions;
  std::string triggerLabel;
  bool enabled = false;
  bool triggerMatched = false;
  size_t triggerCount = 0;
  std::string firstFewTriggers; // 用于未命中日志
  const char *protoName =
      (protocol == PeripheralType::UDP) ? "UDP"
      : (protocol == PeripheralType::TCP) ? "TCP" : "?";

  // 预先把收到的数据转成 hex 备用，供 hex 模式比对
  const std::string dataHex = bytesToHex(data);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const NetworkConfig *cfg = nullptr;
    if (protocol == PeripheralType::UDP) cfg = &udpConfig_;
    else if (protocol == PeripheralType::TCP) cfg = &tcpConfig_;
    else return false;
    if (!cfg) return false;

    enabled = cfg->enabled;
    triggerCount = cfg->triggers.size();
    if (!enabled) {
      LOG_DEBUG("[PeripheralManager] %s 未启用，跳过触发匹配", protoName);
      Json::Value event(Json::objectValue);
      event["type"] = "peripheral_trigger";
      event["source"] = protoName;
      event["status"] = "disabled";
      event["message"] = std::string(protoName) + " 未启用，跳过触发匹配";
      event["hex"] = dataHex;
      broadcastTriggerEvent(event);
      return false;
    }

    // 匹配规则：
    //  1) 文本精确匹配（entry.trigger == data）——适合 ASCII 命令，如 "play"
    //  2) 十六进制匹配：若 entry.trigger 规范化后是纯 hex（偶长度），则对比 data 的 hex 形
    //     （适合串口风格的二进制指令，如 "FF01" 匹配收到的 0xFF 0x01 两字节）
    for (const auto &entry : cfg->triggers) {
      if (entry.trigger == data) {
        triggerMatched = true;
        matchedFunctions = entry.functions;
        triggerLabel = entry.name.empty() ? entry.trigger : entry.name;
        break;
      }
      std::string hexForm = normalizeHexTrigger(entry.trigger);
      if (!hexForm.empty() && hexForm == dataHex) {
        triggerMatched = true;
        matchedFunctions = entry.functions;
        triggerLabel = entry.name.empty() ? entry.trigger : entry.name;
        break;
      }
    }

    // 未命中：抓前 3 条触发字符串拼成日志供 diff
    if (!triggerMatched) {
      size_t shown = 0;
      for (const auto &entry : cfg->triggers) {
        if (shown >= 3) break;
        if (!firstFewTriggers.empty()) firstFewTriggers += " | ";
        firstFewTriggers += "\"" + entry.trigger + "\"";
        ++shown;
      }
    }
  }

  if (!triggerMatched) {
    LOG_INFO("[PeripheralManager] %s 未命中触发：收到\"%s\"(%zu字节,hex=%s) vs 已配置 %zu 项: %s%s",
             protoName, data.c_str(), data.size(), dataHex.c_str(),
             triggerCount, firstFewTriggers.c_str(),
             (triggerCount > 3 ? " ..." : ""));
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = protoName;
    event["status"] = "miss";
    event["message"] = std::string(protoName) + " 未命中触发";
    event["hex"] = dataHex;
    event["trigger_count"] = static_cast<Json::UInt64>(triggerCount);
    event["configured"] = firstFewTriggers;
    broadcastTriggerEvent(event);
    return false;
  }

  if (matchedFunctions.empty()) {
    LOG_WARN("[PeripheralManager] %s 触发命中但未绑定功能：trigger=%s hex=%s",
             protoName, triggerLabel.c_str(), dataHex.c_str());
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = protoName;
    event["status"] = "empty";
    event["message"] = std::string(protoName) + " 触发命中但未绑定功能";
    event["trigger"] = triggerLabel;
    event["hex"] = dataHex;
    event["function_count"] = 0;
    broadcastTriggerEvent(event);
    return true;
  }

  // 锁外派发（避免 CommandRouter → 其他子系统重入死锁）
  {
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = protoName;
    event["status"] = "matched";
    event["message"] = std::string(protoName) + " 触发命中";
    event["trigger"] = triggerLabel;
    event["hex"] = dataHex;
    event["function_count"] = static_cast<Json::UInt64>(matchedFunctions.size());
    broadcastTriggerEvent(event);
  }
  executeTriggerFunctions(matchedFunctions, triggerLabel, protoName, data, dataHex);
  return true;
}

bool PeripheralManager::dispatchSerialTemplate(const std::string &type,
                                               const std::string &hex) {
  return dispatchSerialTemplate(type, hex, hex);
}

bool PeripheralManager::dispatchSerialTemplate(const std::string &type,
                                               const std::string &hex,
                                               const std::string &payload) {
  if (hex.empty()) return false;
  if (isCommandDispatchBlocked()) {
    const std::string response = commandDispatchBlockedResponseJson();
    broadcastResult(response);
    LOG_WARN("[PeripheralManager] 投屏中，阻断 %s 串口模板触发",
             type.c_str());
    return true;
  }

  const std::string serialType = normalizeSerialTemplateType(type);
  Json::Value functions(Json::arrayValue);
  std::string triggerLabel;
  size_t templateCount = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &tpl : serialTemplates_) {
      const std::string tplType =
          tpl.type.empty() ? "rs232" : normalizeSerialTemplateType(tpl.type);
      if (!tpl.type.empty() && tplType != serialType) {
        continue;
      }
      ++templateCount;
      if ((tpl.code == hex || tpl.data == hex) && tpl.functions.isArray()) {
        functions = tpl.functions;
        triggerLabel = tpl.name.empty() ? hex : tpl.name;
        break;
      }
    }
  }

  if (!functions.isArray() || functions.empty()) {
    LOG_INFO("[PeripheralManager] %s 串口模板未命中或未配置功能: hex=%s, templates=%zu",
             serialType.c_str(), hex.c_str(), templateCount);
    return false;
  }

  std::vector<TriggerFunction> matchedFunctions;
  matchedFunctions.reserve(functions.size());
  for (const auto &f : functions) {
    if (!f.isObject()) continue;
    TriggerFunction tf;
    tf.functionId = f.get("function_id", "").asString();
    tf.functionName = f.get("function_name", tf.functionId).asString();
    tf.action = f.get("action", "").asString();
    tf.code = f.get("function_code", 2).asInt();

    Json::Value extra(Json::objectValue);
    for (const auto &key : f.getMemberNames()) {
      if (key == "function_id" || key == "function_name" ||
          key == "function_code" || key == "action") {
        continue;
      }
      extra[key] = f[key];
    }
    tf.extraParam = extra;
    matchedFunctions.push_back(std::move(tf));
  }

  if (matchedFunctions.empty()) {
    LOG_INFO("[PeripheralManager] %s 串口模板命中但无有效功能: %s",
             serialType.c_str(), triggerLabel.c_str());
    return false;
  }

  executeTriggerFunctions(matchedFunctions, triggerLabel, serialType, payload, hex);
  return true;
}

void PeripheralManager::executeTriggerFunctions(
    const std::vector<TriggerFunction> &functions,
    const std::string &triggerLabel,
    const std::string &source,
    const std::string &payload,
    const std::string &hex) {
  if (!commandRouter_) {
    LOG_WARN("[PeripheralManager] dispatchNetworkTrigger: CommandRouter 未就绪，"
             "跳过 trigger=%s",
             triggerLabel.c_str());
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = source;
    event["status"] = "error";
    event["trigger"] = triggerLabel;
    event["message"] = "CommandRouter 未就绪";
    broadcastTriggerEvent(event);
    return;
  }

  LOG_INFO("[PeripheralManager] 触发命中: %s -> %zu functions",
           triggerLabel.c_str(), functions.size());

  for (const auto &fn : functions) {
    // 1) 起步参数：前端传来的 extraParam（layerId/时间/volume/...）
    Json::Value finalParam = fn.extraParam.isObject() ? fn.extraParam
                                                      : Json::Value(Json::objectValue);

    // 2) 回查 CommandList/playback/{function_id}.json 补全默认参数
    int finalCode = fn.code;
    if (!fn.functionId.empty()) {
      const std::string filePath =
          COMMAND_LIST_DIR + "playback/" + fn.functionId + ".json";
      if (FileUtils::exists(filePath)) {
        std::string content;
        if (FileUtils::readFile(filePath, content)) {
          Json::Value fileJson;
          std::string parseErr;
          if (JsonUtils::parseJson(content, fileJson, parseErr) &&
              fileJson.isObject()) {
            // 文件 code 仅在前端没指定时使用
            if (fileJson.isMember("code") && fn.code == 0) {
              finalCode = fileJson["code"].asInt();
            }
            if (fileJson.isMember("param") && fileJson["param"].isObject()) {
              const Json::Value &fileParam = fileJson["param"];
              for (const auto &k : fileParam.getMemberNames()) {
                // 前端 extraParam 已存在的字段不被覆盖
                if (!finalParam.isMember(k)) {
                  finalParam[k] = fileParam[k];
                }
              }
            }
          }
        }
      }
    }

    // 3) 兜底 action
    if (!finalParam.isMember("action") && !fn.action.empty()) {
      finalParam["action"] = fn.action;
    }

    // 播放列表触发应与网页“播放列表”按钮一致：不指定 index，
    // 让后端按 loop=3 等播放模式决定起点和自动续播。
    if (finalParam.get("isPlaylist", false).asBool() &&
        finalParam.isMember("playlistId") &&
        finalParam.isMember("index")) {
      finalParam.removeMember("index");
    }

    const bool isForwardFunction =
        fn.action == "forward_payload" ||
        finalParam.get("action", "").asString() == "forward_payload";
    if (isForwardFunction) {
      std::string forwardPayloadData = payload;
      if (forwardPayloadData.empty()) {
        forwardPayloadData = finalParam.get("payload", "").asString();
      }
      std::string forwardHex = hex;
      if (forwardHex.empty()) {
        forwardHex = finalParam.get("hex", "").asString();
      }
      if (forwardHex.empty()) {
        forwardHex = normalizeHexTrigger(forwardPayloadData);
      }
      std::string forwardError;
      if (!forwardPayload(finalParam, forwardPayloadData, forwardHex, forwardError)) {
        LOG_WARN("[PeripheralManager]   forward 执行失败: %s",
                 forwardError.c_str());
      }
      continue;
    }

    if (!source.empty()) finalParam["_source"] = source;

    // 4) 构建 CommandRequest 并派发
    CommandRequest req;
    req.type = 0;
    req.code = finalCode;
    req.requestId = "";
    req.timestamp = static_cast<int64_t>(std::time(nullptr));
    req.paramJson = JsonUtils::toString(finalParam);

    LOG_INFO("[PeripheralManager]   -> code=0x%02X action=%s param=%s",
             finalCode, finalParam.get("action", fn.action).asString().c_str(),
             req.paramJson.c_str());

    try {
      CommandResponse resp = commandRouter_->processCommand(req);
      if (!resp.ok) {
        LOG_WARN("[PeripheralManager]   function 执行失败: %s (err=0x%04X)",
                 resp.message.c_str(), resp.error);
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "failed";
        event["trigger"] = triggerLabel;
        event["action"] = finalParam.get("action", fn.action).asString();
        event["code"] = finalCode;
        event["message"] = resp.message;
        event["error"] = resp.error;
        event["param"] = req.paramJson;
        broadcastTriggerEvent(event);
      } else {
        LOG_INFO("[PeripheralManager]   function 执行成功: %s",
                 resp.message.c_str());
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "success";
        event["trigger"] = triggerLabel;
        event["action"] = finalParam.get("action", fn.action).asString();
        event["code"] = finalCode;
        event["message"] = resp.message;
        event["param"] = req.paramJson;
        broadcastTriggerEvent(event);
      }
    } catch (const std::exception &e) {
      LOG_ERROR("[PeripheralManager]   function 异常: %s", e.what());
    }
  }
}

// =====================================================================
//  配置持久化 / 查询
//    - peripherals.json：新独立文件，放在 CONFIG_DIR 下，不污染 config.json
//    - set_config 每次都会 saveToDisk()，重启由 loadFromDisk() 恢复
// =====================================================================

Json::Value
PeripheralManager::buildNetworkConfigJson(const NetworkConfig &cfg,
                                          bool includeMode) {
  Json::Value obj(Json::objectValue);
  obj["bind_address"] = cfg.bindAddress;
  obj["bind_port"] = cfg.bindPort;
  obj["target_address"] = cfg.targetAddress;
  obj["target_port"] = cfg.targetPort;
  obj["enabled"] = cfg.enabled;
  obj["multicast_group"] = cfg.multicastGroup;  // 组播地址，空字符串表示不启用
  if (includeMode) {
    obj["mode"] = cfg.mode;
  }
  if (!cfg.path.empty()) {
    obj["path"] = cfg.path;
  }

  Json::Value mappings(Json::arrayValue);
  for (const auto &entry : cfg.triggers) {
    Json::Value m(Json::objectValue);
    m["trigger"] = entry.trigger;
    m["name"] = entry.name;
    Json::Value fns(Json::arrayValue);
    for (const auto &fn : entry.functions) {
      Json::Value f(Json::objectValue);
      // 先拷贝 extraParam（layerId/volume/scene_name 等）作为基础
      if (fn.extraParam.isObject()) {
        for (const auto &k : fn.extraParam.getMemberNames()) {
          f[k] = fn.extraParam[k];
        }
      }
      // 再覆写元字段（保证顺序/稳定）
      f["function_id"] = fn.functionId;
      f["function_name"] = fn.functionName;
      f["function_code"] = fn.code;
      f["action"] = fn.action;
      fns.append(f);
    }
    m["functions"] = fns;
    mappings.append(m);
  }
  obj["mappings"] = mappings;
  return obj;
}

Json::Value PeripheralManager::buildGetConfigResponse(const std::string &type) {
  Json::Value result(Json::objectValue);
  if (type == "udp") {
    return buildNetworkConfigJson(udpConfig_, /*includeMode= 包含模式*/false);
  } else if (type == "tcp") {
    return buildNetworkConfigJson(tcpConfig_, /*includeMode= 包含模式*/true);
  } else if (type == "websocket") {
    return buildNetworkConfigJson(websocketConfig_, /*includeMode= 包含模式*/false);
  } else if (type == "function_config") {
    return functionConfig_.isObject() ? functionConfig_ : buildDefaultFunctionConfig();
  } else if (type == "rs232") {
    result["port"] = rs232Config_.port;
    result["baudrate"] = rs232Config_.baudrate;
    result["enabled"] = rs232Config_.enabled;
    result["learn_mode"] = rs232Config_.learnMode;
    Json::Value mappings(Json::arrayValue);
    for (const auto &kv : rs232Config_.cmdMap) {
      Json::Value m(Json::objectValue);
      m["trigger"] = kv.first;
      m["action"] = kv.second;
      mappings.append(m);
    }
    result["mappings"] = mappings;
  } else if (type == "rs485") {
    result["port"] = rs485Config_.port;
    result["baudrate"] = rs485Config_.baudrate;
    result["slave_id"] = rs485Config_.slaveId;
    result["enabled"] = rs485Config_.enabled;
    result["learn_mode"] = rs485Config_.learnMode;
  } else if (type == "dmx512") {
    result["mode"] = dmxInputModeToString(dmxConfig_.inputMode);
    result["port"] = dmxConfig_.port;
    result["external_port"] = dmxConfig_.externalPort;
    result["external_baudrate"] = dmxConfig_.externalBaudrate;
    result["external_data_bit"] = dmxConfig_.externalDataBits;
    result["external_stop_bit"] = dmxConfig_.externalStopBits;
    result["external_enable"] = dmxConfig_.externalEnabled;
    result["stop_handle"] = !dmxConfig_.externalHandleEnabled;
    result["stop_material"] = !dmxConfig_.externalMaterialEnabled;
    result["dmx_info_show"] = dmxConfig_.externalShowInfo;
    result["dmx_info_pos"] = dmxConfig_.externalInfoPos;
    result["electron_version"] = dmxConfig_.electronVersion;
    result["effect_interval"] = dmxConfig_.effectInterval;
    result["external_running"] = isDmxExternalInputRunning();
    result["address"] = dmxConfig_.startAddress;
    result["baudrate"] = dmxConfig_.baudrate;
    result["master"] = dmxConfig_.masterDimmer;
    result["enabled"] = dmxConfig_.enabled;
    Json::Value mappings(Json::arrayValue);
    for (const auto &kv : dmxConfig_.channelMap) {
      Json::Value m(Json::objectValue);
      m["offset"] = kv.first;
      m["function"] = kv.second;
      mappings.append(m);
    }
    result["mappings"] = mappings;
  }
  return result;
}

Json::Value
PeripheralManager::buildSerialTemplatesResponse(const std::string &type) const {
  const std::string wantedType =
      type.empty() ? "" : normalizeSerialTemplateType(type);
  Json::Value templates(Json::arrayValue);
  for (const auto &tpl : serialTemplates_) {
    const std::string tplType =
        tpl.type.empty() ? "rs232" : normalizeSerialTemplateType(tpl.type);
    if (!wantedType.empty() && !tpl.type.empty() && tplType != wantedType) {
      continue;
    }
    Json::Value item(Json::objectValue);
    item["type"] = tplType;
    item["has_type"] = !tpl.type.empty();
    item["code"] = tpl.code;
    item["template_code"] = tpl.code;
    item["name"] = tpl.name;
    item["template_name"] = tpl.name;
    item["data"] = tpl.data;
    item["functions"] = tpl.functions.isArray() ? tpl.functions
                                                : Json::Value(Json::arrayValue);
    item["forward_to_serial"] = tpl.forwardToSerial;
    templates.append(item);
  }
  return templates;
}

Json::Value PeripheralManager::setSerialLearnMode(const Json::Value &param) {
  Json::Value result;
  result["ok"] = true;

  const std::string type =
      normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
  const bool enabled = param.get("enabled", false).asBool();
  std::string port;
  int baudrate = 9600;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    SerialConfig &cfg = (type == "rs485") ? rs485Config_ : rs232Config_;
    cfg.learnMode = enabled;
    port = param.get("port", cfg.port).asString();
    baudrate = param.get("baudrate", cfg.baudrate > 0 ? cfg.baudrate : 9600).asInt();
    if (!port.empty()) cfg.port = port;
    cfg.baudrate = baudrate;
    cfg.enabled = true;
    saveToDisk();
  }

  if (port.empty()) {
    result["ok"] = false;
    result["message"] = "serial port is empty";
    return result;
  }

  std::string error;
  if (!startSerialLearnThread(type, port, baudrate, error)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      SerialConfig &cfg = (type == "rs485") ? rs485Config_ : rs232Config_;
      cfg.learnMode = false;
      saveToDisk();
    }
    result["ok"] = false;
    result["message"] = error;
    return result;
  }

  result["learn_mode"] = enabled;
  result["port"] = port;
  result["baudrate"] = baudrate;
  return result;
}

bool PeripheralManager::startSerialLearnThread(const std::string &type,
                                               const std::string &port,
                                               int baudrate,
                                               std::string &error) {
  const std::string serialType = normalizeSerialTemplateType(type);
  if (port.empty()) {
    error = "serial port is empty";
    return false;
  }
  if (serialType == "rs232") {
    stopDmxExternalInput();
  }
  auto onFrame = [this, serialType, port](const std::vector<uint8_t> &frame) {
    if (frame.empty()) return;
    const std::string hex = bytesToHexCompact(frame.data(), frame.size());
    const std::string preview = hexPreview(frame);
    bool learnMode = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const SerialConfig &cfg =
          (serialType == "rs485") ? rs485Config_ : rs232Config_;
      learnMode = cfg.learnMode;
    }
    if (learnMode) {
      rememberSerialTemplate(serialType, port, hex);
    }
    dispatchSerialTemplate(serialType, hex);
    broadcastSerialReceive(serialType, port, hex, preview, learnMode);
  };

  if (serialType == "rs485") {
    serialLearnRs485_.reset();
    auto server = std::make_unique<Rs485Server>(port, baudrate, 1);
    server->setRawFrameCallback(onFrame);
    server->setFrameGapMs(120);
    if (!server->start()) {
      error = "failed to start RS485 listener on " + port;
      return false;
    }
    serialLearnRs485_ = std::move(server);
  } else {
    serialLearnRs232_.reset();
    auto server = std::make_unique<Rs232Server>(port, baudrate);
    server->setRawFrameCallback(onFrame);
    server->setFrameGapMs(120);
    if (!server->start()) {
      error = "failed to start RS232 listener on " + port;
      return false;
    }
    serialLearnRs232_ = std::move(server);
  }

  LOG_INFO("[SerialListener] started on %s @ %d (%s)", port.c_str(), baudrate,
           type.c_str());
  return true;
}

void PeripheralManager::stopSerialLearnThread() {
  serialLearnRs232_.reset();
  serialLearnRs485_.reset();
  LOG_INFO("[SerialListener] stopped");
}

void PeripheralManager::stopSerialLearnThread(const std::string &type) {
  const std::string serialType = normalizeSerialTemplateType(type);
  if (serialType == "rs485") {
    serialLearnRs485_.reset();
  } else {
    serialLearnRs232_.reset();
  }
  LOG_INFO("[SerialListener] stopped (%s)", serialType.c_str());
}

void PeripheralManager::rememberSerialTemplate(const std::string &type,
                                               const std::string &port,
                                               const std::string &hex) {
  const std::string serialType = normalizeSerialTemplateType(type);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(serialTemplates_.begin(), serialTemplates_.end(),
                         [&](const SerialTemplate &tpl) {
                           const std::string tplType = tpl.type.empty()
                                                           ? "rs232"
                                                           : normalizeSerialTemplateType(tpl.type);
                           return tplType == serialType &&
                                  (tpl.code == hex || tpl.data == hex);
                         });
  if (it == serialTemplates_.end()) {
    SerialTemplate tpl;
    tpl.type = serialType;
    tpl.code = hex;
    tpl.name = "学习模板" + std::to_string(serialTemplates_.size() + 1);
    tpl.data = hex;
    tpl.functions = Json::Value(Json::arrayValue);
    serialTemplates_.push_back(std::move(tpl));
    LOG_INFO("[SerialLearn] learned %s from %s", hex.c_str(), port.c_str());
  }
  saveToDisk();
}

void PeripheralManager::broadcastSerialReceive(const std::string &type,
                                               const std::string &port,
                                               const std::string &hex,
                                               const std::string &preview,
                                               bool learned) {
  Json::Value event(Json::objectValue);
  event["type"] = "serial_recv";
  event["serial_type"] = normalizeSerialTemplateType(type);
  event["port"] = port;
  event["hex"] = hex;
  event["preview"] = preview;
  event["learned"] = learned;

  const std::string payload = JsonUtils::toString(event);
  if (auto *http = NetworkManager::getInstance().getHttpServer(); http) {
    http->broadcastSSE("serial_recv", payload);
  }
  if (auto *http = NetworkManager::getInstance().getVodHttpServer(); http) {
    http->broadcastSSE("serial_recv", payload);
  }
}

bool PeripheralManager::forwardPayload(const Json::Value &param,
                                       const std::string &payload,
                                       const std::string &hex,
                                       std::string &error) {
  const std::string target =
      param.get("forward_target", param.get("target", "")).asString();
  if (target.empty()) {
    error = "forward target is empty";
    return false;
  }

  struct ForwardPayloadItem {
    std::string payload;
    std::string hex;
  };

  auto payloadFromHex = [](const std::string &hexValue) {
    std::string parseError;
    const std::vector<uint8_t> bytes = hexToBytes(hexValue, parseError);
    if (bytes.empty()) return std::string();
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  };

  auto getMappingString = [](const Json::Value &item,
                             std::initializer_list<const char *> keys) {
    for (const char *key : keys) {
      if (item.isMember(key)) {
        return item[key].asString();
      }
    }
    return std::string();
  };

  auto buildForwardItems = [&]() {
    std::vector<ForwardPayloadItem> items;
    const Json::Value mappings =
        param.get("command_mappings",
                  param.get("forward_command_mappings", Json::Value(Json::arrayValue)));

    std::string incomingHex = hex;
    if (incomingHex.empty()) {
      incomingHex = normalizeHexTrigger(payload);
    }
    if (incomingHex.empty() && !payload.empty()) {
      incomingHex = bytesToHex(payload);
    }
    incomingHex = normalizeHexTrigger(incomingHex);

    if (mappings.isArray() && !incomingHex.empty()) {
      for (const auto &mapping : mappings) {
        if (!mapping.isObject()) continue;
        const std::string sourceHex =
            normalizeHexTrigger(getMappingString(
                mapping, {"source_hex", "source", "from", "input_hex", "trigger"}));
        const std::string targetHex =
            normalizeHexTrigger(getMappingString(
                mapping, {"target_hex", "target", "to", "output_hex", "data", "hex"}));
        if (sourceHex.empty() || targetHex.empty() || sourceHex != incomingHex) {
          continue;
        }
        items.push_back({payloadFromHex(targetHex), targetHex});
      }
    }

    if (items.empty()) {
      std::string itemPayload = payload;
      if (itemPayload.empty() && !hex.empty()) {
        itemPayload = payloadFromHex(hex);
      }
      items.push_back({itemPayload, hex});
    }
    return items;
  };

  const std::vector<ForwardPayloadItem> forwardItems = buildForwardItems();

  if (target == "rs232" || target == "rs485") {
    std::string port = param.get("forward_port", param.get("port", "")).asString();
    int baudrate =
        param.get("forward_baudrate", param.get("baudrate", 9600)).asInt();
    if (port.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      const SerialConfig &cfg = (target == "rs485") ? rs485Config_ : rs232Config_;
      port = cfg.port;
      if (baudrate <= 0) baudrate = cfg.baudrate;
    }
    if (port.empty()) {
      error = "serial forward port is empty";
      return false;
    }
    for (const auto &item : forwardItems) {
      std::string normalizedHex = item.hex;
      if (normalizedHex.empty()) {
        normalizedHex = normalizeHexTrigger(item.payload);
      }
      if (normalizedHex.empty() && !item.payload.empty()) {
        normalizedHex = bytesToHex(item.payload);
      }
      if (normalizedHex.empty()) {
        error = "serial forward requires hex payload";
        return false;
      }
      if (!writeHexToSerial(target, port, baudrate, normalizedHex, error)) {
        return false;
      }
    }
    return true;
  }

  if (target == "udp") {
    auto *udp = NetworkManager::getInstance().getUdpServer();
    if (!udp || !udp->isRunning()) {
      error = "udp server unavailable";
      return false;
    }
    std::string address =
        param.get("forward_address", param.get("address", "")).asString();
    int port = param.get("forward_port", param.get("port", 0)).asInt();
    bool useBroadcast = param.get("broadcast", false).asBool();
    bool useMulticast = param.get("multicast", false).asBool();
    std::string multicastGroup =
        param.get("multicast_group", "").asString();
    int ttl = param.get("ttl", 1).asInt();

    if (address.empty() || port <= 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (address.empty()) address = udpConfig_.targetAddress;
      if (port <= 0) port = udpConfig_.targetPort;
      if (multicastGroup.empty()) multicastGroup = udpConfig_.multicastGroup;
    }
    if (port <= 0) {
      port = udp->getPort();
    }
    if (useMulticast || (!multicastGroup.empty() && address.empty())) {
      if (multicastGroup.empty()) {
        error = "multicast group is empty";
        return false;
      }
      for (const auto &item : forwardItems) {
        if (!udp->sendToMulticastGroup(multicastGroup, port, item.payload, ttl)) {
          error = "udp multicast send failed";
          return false;
        }
      }
      return true;
    }
    if (useBroadcast) {
      for (const auto &item : forwardItems) {
        const size_t sent = udp->broadcastToClientIps(port, item.payload);
        if (sent == 0) {
          error = "no udp clients available";
          return false;
        }
      }
      return true;
    }
    if (address.empty()) {
      error = "udp address is empty";
      return false;
    }
    for (const auto &item : forwardItems) {
      if (!udp->sendTo(address, port, item.payload)) {
        error = "udp send failed";
        return false;
      }
    }
    return true;
  }

  if (target == "tcp") {
    auto *tcp = NetworkManager::getInstance().getTcpServer();
    if (!tcp || !tcp->isRunning()) {
      error = "tcp server unavailable";
      return false;
    }
    const std::string mode = param.get("forward_mode", "broadcast").asString();
    if (mode == "broadcast") {
      for (const auto &item : forwardItems) {
        tcp->broadcast(item.payload);
      }
      return true;
    }
    const int clientFd = param.get("client_fd", 0).asInt();
    if (clientFd <= 0) {
      error = "tcp client fd is invalid";
      return false;
    }
    for (const auto &item : forwardItems) {
      if (!tcp->sendToClient(clientFd, item.payload)) {
        error = "tcp send failed";
        return false;
      }
    }
    return true;
  }

  if (target == "websocket") {
    auto *ws = NetworkManager::getInstance().getWebSocketServer();
    if (!ws || !ws->isRunning()) {
      error = "websocket server unavailable";
      return false;
    }
    const std::string mode = param.get("forward_mode", "broadcast").asString();
    if (mode == "broadcast") {
      for (const auto &item : forwardItems) {
        std::string wsMessage = item.payload;
        const bool printable =
            std::all_of(wsMessage.begin(), wsMessage.end(), [](unsigned char c) {
              return (c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t';
            });
        if (!printable && !item.hex.empty()) {
          wsMessage = item.hex;
        }
        ws->broadcast(wsMessage);
      }
      return true;
    }
    const int clientId = param.get("client_id", 0).asInt();
    if (clientId <= 0) {
      error = "websocket client id is invalid";
      return false;
    }
    for (const auto &item : forwardItems) {
      std::string wsMessage = item.payload;
      const bool printable =
          std::all_of(wsMessage.begin(), wsMessage.end(), [](unsigned char c) {
            return (c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t';
          });
      if (!printable && !item.hex.empty()) {
        wsMessage = item.hex;
      }
      if (!ws->sendToClient(clientId, wsMessage)) {
        error = "websocket send failed";
        return false;
      }
    }
    return true;
  }

  error = "unsupported forward target: " + target;
  return false;
}

bool PeripheralManager::writeHexToSerial(const std::string &type,
                                         const std::string &port, int baudrate,
                                         const std::string &hex,
                                         std::string &error) {
  std::vector<uint8_t> bytes = hexToBytes(hex, error);
  if (bytes.empty()) return false;

  if (type == "rs485") {
    Rs485Server server(port, baudrate > 0 ? baudrate : 9600, 1);
    if (!server.start()) {
      error = "failed to start RS485 writer on " + port;
      return false;
    }
    if (!server.writeRaw(bytes)) {
      error = "RS485 write failed";
      return false;
    }
    server.stop();
  } else {
    Rs232Server server(port, baudrate > 0 ? baudrate : 9600);
    if (!server.start()) {
      error = "failed to start RS232 writer on " + port;
      return false;
    }
    if (!server.writeRaw(bytes)) {
      error = "RS232 write failed";
      return false;
    }
    server.stop();
  }
  return true;
}

bool PeripheralManager::startDmxExternalInput(const std::string &port,
                                              int baudrate,
                                              std::string &error) {
  if (port.empty()) {
    error = "DMX external RS232 port is empty";
    return false;
  }

  stopSerialLearnThread("rs232");

  auto server = std::make_unique<Rs232Server>(
      port, normalizeBaudrate(baudrate, 115200));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    server->setDataBits(dmxConfig_.externalDataBits);
    server->setStopBits(dmxConfig_.externalStopBits);
  }
  server->setRawFrameCallback(
      [this](const std::vector<uint8_t> &frame) { handleDmxExternalFrame(frame); });
  server->setFrameGapMs(40);
  if (!server->start()) {
    error = "failed to start DMX external RS232 listener on " + port;
    return false;
  }

  writeDmxExternalStartupCommands(*server);

  {
    std::lock_guard<std::mutex> lock(dmxExternalMutex_);
    dmxExternalRs232_ = std::move(server);
  }

  LOG_INFO("[DMX512 External] RS232 listener started on %s @ %d",
           port.c_str(), normalizeBaudrate(baudrate, 115200));
  return true;
}

void PeripheralManager::stopDmxExternalInput() {
  std::unique_ptr<Rs232Server> oldServer;
  {
    std::lock_guard<std::mutex> lock(dmxExternalMutex_);
    oldServer = std::move(dmxExternalRs232_);
  }
  if (oldServer) {
    oldServer.reset();
    LOG_INFO("[DMX512 External] RS232 listener stopped");
  }
}

void PeripheralManager::writeDmxExternalStartupCommands(Rs232Server &server) {
  int electronVersion = 1;
  bool enabled = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    electronVersion = dmxConfig_.electronVersion;
    enabled = dmxConfig_.externalHandleEnabled;
  }
  if (!enabled) {
    return;
  }
  if (electronVersion == 0) {
    server.writeRaw(std::vector<uint8_t>{0xAA, 0xBB, 0xDA, 0x05, 0x01});
  }
  server.writeRaw(std::vector<uint8_t>{0xAA, 0xBB, 0xDA, 0x05, 0x00});
  server.writeRaw(std::vector<uint8_t>{0xAA, 0xBB, 0xD2, 0x05, 0x01});
}

bool PeripheralManager::writeDmxExternalRaw(const std::vector<uint8_t> &data) {
  if (data.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(dmxExternalMutex_);
  if (!dmxExternalRs232_ || !dmxExternalRs232_->isRunning()) {
    return false;
  }
  return dmxExternalRs232_->writeRaw(data);
}

bool PeripheralManager::writeDmxExternalHex(const std::string &hex,
                                            std::string &error) {
  std::vector<uint8_t> bytes = hexToBytes(hex, error);
  if (bytes.empty()) {
    return false;
  }
  if (!writeDmxExternalRaw(bytes)) {
    error = "DMX external RS232 is not running or write failed";
    return false;
  }
  return true;
}

void PeripheralManager::writeDmxExternalAddressCommand(Rs232Server *server) {
  int light = 1;
  bool enabled = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    light = clampDmxAddress(dmxConfig_.startAddress);
    enabled = dmxConfig_.externalHandleEnabled;
  }
  if (!enabled) {
    return;
  }
  const uint8_t hi = static_cast<uint8_t>((light / 256) & 0xFF);
  const uint8_t lo = static_cast<uint8_t>(light & 0xFF);
  std::vector<uint8_t> cmd{0xAA, 0xBB, 0xF0, 0x06, hi, lo};
  if (server) {
    server->writeRaw(cmd);
  } else {
    writeDmxExternalRaw(cmd);
  }
}

bool PeripheralManager::handleDmxExternalReplyFrame(
    const std::vector<uint8_t> &frame) const {
  if (frame.size() < 2) {
    return false;
  }
  if (frame.size() >= 4 && frame[0] == 0xAA && frame[1] == 0x55 &&
      frame[2] == 0x00 && frame[3] == 0x08) {
    return true;
  }
  return (frame[0] == 0xCC && frame[1] == 0xDD) ||
         (frame[0] == 0xEE && frame[1] == 0xFF) ||
         (frame[0] == 0xC9 && frame[1] == 0xD9) ||
         (frame[0] == 0xE5 && frame[1] == 0xF5);
}

bool PeripheralManager::handleDmxExternalLedPacket(const uint8_t *packet,
                                                   size_t len) {
  if (!packet || len < 5 || packet[0] != 0xEF || packet[1] != 0xBC) {
    return false;
  }
  const size_t commandLen = std::min<size_t>(len, 5);
  if (commandLen == 5 && packet[2] == 0x05 && packet[3] == 0xAA &&
      packet[4] == 0x88) {
    writeDmxExternalAddressCommand();
    LOG_INFO("[DMX512 External] address request handled");
    return true;
  }
  LOG_DEBUG("[DMX512 External] ignored EFBC LED packet: %s",
            bytesToHexCompact(packet, commandLen).c_str());
  return true;
}

void PeripheralManager::restoreDmxExternalBright() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dmxConfig_.masterDimmer = 255;
    if (simulationChannels_.size() < 512) {
      simulationChannels_.assign(512, 0);
    }
    if (simulationChannels_.size() >= 5) {
      simulationChannels_[1] = 255;
      simulationChannels_[2] = 0;
      simulationChannels_[3] = 0;
      simulationChannels_[4] = 0;
    }
  }
  if (engine_) {
    if (auto *em = engine_->getEffectManagerPtr()) {
      em->clearDMX512MaterialColor();
      em->setDMX512OverlayEnabled(false);
    }
  }
  updateDmxOutput();
}

void PeripheralManager::restoreDmxExternalAllEffect() {
  std::array<uint8_t, 12> channels{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dmxConfig_.masterDimmer = 255;
    if (simulationChannels_.size() < 512) {
      simulationChannels_.assign(512, 0);
    }
    for (size_t i = 0; i < channels.size() &&
                       static_cast<int>(i) < static_cast<int>(simulationChannels_.size());
         ++i) {
      simulationChannels_[static_cast<int>(i)] = 0;
    }
  }
  if (engine_) {
    if (auto *receiver = engine_->getDmxReceiver()) {
      receiver->injectChannelData(0, channels.data(), channels.size());
    }
    if (auto *em = engine_->getEffectManagerPtr()) {
      em->clearDMX512MaterialColor();
      em->setDMX512OverlayEnabled(false);
      em->setDmxEffectSpeedMultiplier(1.0f);
    }
  }
  updateDmxOutput();
}

void PeripheralManager::showDmxExternalInfo(const std::vector<uint8_t> &frame) {
  bool showInfo = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    showInfo = dmxConfig_.externalShowInfo;
  }
  if (!showInfo || !engine_ || frame.empty()) {
    return;
  }
  std::vector<uint8_t> clipped(frame.begin(),
                              frame.begin() +
                                  std::min<size_t>(16, frame.size()));
  const std::string text = "512命令:" +
                           bytesToHexCompact(clipped.data(), clipped.size());
  if (commandRouter_) {
    commandRouter_->triggerLayer41Hint(static_cast<int>(HintType::CUSTOM),
                                       text);
  }
}

void PeripheralManager::showDmxChannelInfoLocked() const {
  if (!dmxConfig_.externalShowInfo || !engine_ || !commandRouter_) {
    return;
  }

  std::array<uint8_t, 15> packet{};
  packet[0] = 0xEF;
  packet[1] = 0xBB;
  packet[2] = 0x0F;
  for (size_t i = 0; i < 12 && i < simulationChannels_.size(); ++i) {
    packet[i + 3] = simulationChannels_[i];
  }

  const std::string text =
      "512命令:" + bytesToHexCompact(packet.data(), packet.size());
  commandRouter_->triggerLayer41Hint(static_cast<int>(HintType::CUSTOM), text);
}

bool PeripheralManager::handleLegacyDmxCommand(const Json::Value &param,
                                               Json::Value &result) {
  result["ok"] = true;

  if (param.isMember("serial_cmd")) {
    std::string error;
    const std::string cmd = param["serial_cmd"].asString();
    if (!writeDmxExternalHex(cmd, error)) {
      result["ok"] = false;
      result["message"] = error;
      return false;
    }
    result["sent_hex"] = cmd;
  }

  bool sendAddress = false;
  bool saveConfig = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (param.isMember("stop_handle")) {
      dmxConfig_.externalHandleEnabled = !param["stop_handle"].asBool();
      if (!dmxConfig_.externalHandleEnabled) {
        dmxConfig_.masterDimmer = 255;
      }
      saveConfig = true;
    }
    if (param.isMember("stop_material")) {
      dmxConfig_.externalMaterialEnabled = true;
      saveConfig = true;
    }
    if (param.isMember("dmx_info_show")) {
      dmxConfig_.externalShowInfo = param["dmx_info_show"].asBool();
      saveConfig = true;
    }
    if (param.isMember("dmx_info_pos")) {
      dmxConfig_.externalInfoPos = param["dmx_info_pos"].asString();
      saveConfig = true;
    }
    if (param.isMember("light")) {
      dmxConfig_.startAddress = clampDmxAddress(param["light"].asInt());
      sendAddress = true;
      saveConfig = true;
    }
    if (param.isMember("electron_version")) {
      dmxConfig_.electronVersion = param["electron_version"].asInt();
      saveConfig = true;
    }
    if (param.isMember("effect_interval")) {
      dmxConfig_.effectInterval = param["effect_interval"].asInt();
      saveConfig = true;
    }
  }

  if (param.isMember("stop_handle") &&
      param["stop_handle"].asBool()) {
    restoreDmxExternalAllEffect();
  }
  if (sendAddress) {
    writeDmxExternalAddressCommand();
  }
  if (saveConfig) {
    std::lock_guard<std::mutex> lock(mutex_);
    saveToDisk();
    updateDmxOutput();
  }

  result["external_handle_enabled"] = param.isMember("stop_handle")
                                          ? !param["stop_handle"].asBool()
                                          : true;
  result["external_running"] = isDmxExternalInputRunning();
  return true;
}

bool PeripheralManager::parseDmxExternalPacket(
    const uint8_t *packet, size_t len,
    std::array<uint8_t, 12> &channels) const {
  if (!packet || len < 15) {
    return false;
  }
  if (packet[0] != 0xEF || packet[1] != 0xBB || packet[2] != 0x0F) {
    return false;
  }
  channels[0] = packet[3];  // 模式
  channels[1] = packet[4];  // 总亮度
  channels[2] = packet[5];  // 技术标识：R
  channels[3] = packet[6];  // 技术标识：G
  channels[4] = packet[7];  // 技术标识：B
  channels[5] = packet[8];  // 素材目录
  channels[6] = packet[9];  // 素材/索引
  channels[7] = packet[10]; // 场景
  channels[8] = packet[11]; // 特效
  channels[9] = packet[12]; // 速度
  channels[10] = packet[13]; // 图层
  channels[11] = packet[14]; // 声控模式
  return true;
}

void PeripheralManager::handleDmxExternalFrame(
    const std::vector<uint8_t> &frame) {
  if (frame.empty()) {
    return;
  }

  const std::string hex = bytesToHexCompact(frame.data(), frame.size());
  const std::string preview = hexPreview(frame);
  std::string port;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    port = dmxConfig_.externalPort;
  }
  broadcastSerialReceive("rs232", port, hex, preview, false);

  if (handleDmxExternalReplyFrame(frame)) {
    LOG_DEBUG("[DMX512 External] reply frame from 512: %s", hex.c_str());
    return;
  }

  if (frame.size() > 8 && frame[0] != 0xEF) {
    const int declaredSize = readLe16(frame.data());
    const int code = readLe16(frame.data() + 2);
    size_t payloadLen = frame.size() > 4 ? frame.size() - 4 : 0;
    if (declaredSize >= 4 && declaredSize <= static_cast<int>(frame.size())) {
      payloadLen = static_cast<size_t>(declaredSize - 4);
    }
    if (code == 0x10 && frame.size() >= 4) {
      std::vector<uint8_t> payload(frame.begin() + 4,
                                   frame.begin() + 4 + payloadLen);
      if (writeDmxExternalRaw(payload)) {
        LOG_INFO("[DMX512 External] forwarded %zu byte(s) to 512 serial",
                 payload.size());
      } else {
        LOG_WARN("[DMX512 External] forward-to-512 failed, len=%zu",
                 payload.size());
      }
      return;
    }
    if (code == 0x11 && frame.size() >= 4) {
      const std::string upgradePath(frame.begin() + 4,
                                    frame.begin() + 4 + payloadLen);
      std::vector<char> fileBytes = FileUtils::readBinaryFile(upgradePath);
      if (!fileBytes.empty()) {
        std::vector<uint8_t> payload(fileBytes.begin(), fileBytes.end());
        writeDmxExternalRaw(payload);
        LOG_INFO("[DMX512 External] forwarded upgrade file to 512 serial: %s (%zu bytes)",
                 upgradePath.c_str(), payload.size());
      } else {
        LOG_WARN("[DMX512 External] upgrade file not found or empty: %s",
                 upgradePath.c_str());
      }
      return;
    }
  }

  bool enabled = true;
  bool handleEnabled = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled = dmxConfig_.externalEnabled;
    handleEnabled = dmxConfig_.externalHandleEnabled;
  }
  if (!enabled || !handleEnabled) {
    LOG_DEBUG("[DMX512 External] ignored frame while disabled: %s", hex.c_str());
    return;
  }

  if (frame.size() >= 3 && frame[0] == 0xEF && frame[1] == 0xBD) {
    const bool flag = frame[2] > 0;
    if (flag) {
      restoreDmxExternalBright();
      LOG_INFO("[DMX512 External] EFBD restore bright");
    } else {
      restoreDmxExternalAllEffect();
      LOG_INFO("[DMX512 External] EFBD restore all effect");
    }
    return;
  }

  if (frame.size() >= 5 && frame[0] == 0xEF && frame[1] == 0xBC) {
    handleDmxExternalLedPacket(frame.data(), frame.size());
    return;
  }

  size_t packetCount = 0;
  if (frame.size() >= 3 && frame[0] == 0xEF && frame[1] == 0xBB &&
      frame[2] == 0x0F) {
    const size_t loop = frame.size() / 15;
    for (size_t i = 0; i < loop; ++i) {
      const size_t offset = i * 15;
      if (frame[offset] != 0xEF) {
        continue;
      }

      std::array<uint8_t, 12> channels{};
      if (!parseDmxExternalPacket(frame.data() + offset, 15, channels)) {
        continue;
      }

      bool manualSimulation = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        manualSimulation = simulationMode_;
        if (!manualSimulation) {
          if (simulationChannels_.size() < 512) {
            simulationChannels_.assign(512, 0);
          }
          for (size_t ch = 0; ch < channels.size() &&
                              static_cast<int>(ch) <
                                  static_cast<int>(simulationChannels_.size());
               ++ch) {
            simulationChannels_[static_cast<int>(ch)] = channels[ch];
          }
        }
        std::copy(frame.begin() + static_cast<std::ptrdiff_t>(offset),
                  frame.begin() + static_cast<std::ptrdiff_t>(offset + 15),
                  dmxExternalPrevPacket_.begin());
        dmxExternalHasPrevPacket_ = true;
      }
      if (manualSimulation) {
        LOG_DEBUG("[DMX512 External] ignored live EFBB while simulation mode is active");
        std::vector<uint8_t> packet(
            frame.begin() + static_cast<std::ptrdiff_t>(offset),
            frame.begin() + static_cast<std::ptrdiff_t>(offset + 15));
        showDmxExternalInfo(packet);
        ++packetCount;
        continue;
      }

      if (engine_ && engine_->getDmxReceiver()) {
        engine_->getDmxReceiver()->injectChannelData(
            0, channels.data(), channels.size());
      }
      std::vector<uint8_t> packet(frame.begin() + static_cast<std::ptrdiff_t>(offset),
                                  frame.begin() + static_cast<std::ptrdiff_t>(offset + 15));
      showDmxExternalInfo(packet);
      ++packetCount;
    }
  }

  if (packetCount == 0) {
    LOG_DEBUG("[DMX512 External] ignored non-DMX RS232 frame: %s",
              hex.c_str());
    return;
  }

  updateDmxOutput();
  LOG_DEBUG("[DMX512 External] accepted %zu packet(s): %s",
            packetCount, hex.c_str());
}

bool PeripheralManager::saveToDisk() {
  if (CONFIG_DIR.empty()) {
    LOG_WARN("[PeripheralManager] saveToDisk: CONFIG_DIR 未初始化，跳过");
    return false;
  }
  if (!FileUtils::exists(CONFIG_DIR)) {
    FileUtils::createDirectory(CONFIG_DIR);
  }

  Json::Value root(Json::objectValue);
  // RS232 通道
  {
    Json::Value o(Json::objectValue);
    o["port"] = rs232Config_.port;
    o["baudrate"] = rs232Config_.baudrate;
    o["enabled"] = rs232Config_.enabled;
    o["learn_mode"] = rs232Config_.learnMode;
    Json::Value cmds(Json::arrayValue);
    for (const auto &kv : rs232Config_.cmdMap) {
      Json::Value c(Json::objectValue);
      c["hex"] = kv.first;
      c["action"] = kv.second;
      cmds.append(c);
    }
    o["commands"] = cmds;
    root["rs232"] = o;
  }
  // RS485 通道
  {
    Json::Value o(Json::objectValue);
    o["port"] = rs485Config_.port;
    o["baudrate"] = rs485Config_.baudrate;
    o["slave_id"] = rs485Config_.slaveId;
    o["enabled"] = rs485Config_.enabled;
    o["learn_mode"] = rs485Config_.learnMode;
    root["rs485"] = o;
  }
  // DMX512 通道
  {
    Json::Value o(Json::objectValue);
    o["mode"] = dmxInputModeToString(dmxConfig_.inputMode);
    o["port"] = dmxConfig_.port;
    o["external_port"] = dmxConfig_.externalPort;
    o["external_baudrate"] = dmxConfig_.externalBaudrate;
    o["external_data_bit"] = dmxConfig_.externalDataBits;
    o["external_stop_bit"] = dmxConfig_.externalStopBits;
    o["external_enable"] = dmxConfig_.externalEnabled;
    o["stop_handle"] = !dmxConfig_.externalHandleEnabled;
    o["stop_material"] = !dmxConfig_.externalMaterialEnabled;
    o["dmx_info_show"] = dmxConfig_.externalShowInfo;
    o["dmx_info_pos"] = dmxConfig_.externalInfoPos;
    o["electron_version"] = dmxConfig_.electronVersion;
    o["effect_interval"] = dmxConfig_.effectInterval;
    o["address"] = dmxConfig_.startAddress;
    o["baudrate"] = dmxConfig_.baudrate;
    o["master"] = dmxConfig_.masterDimmer;
    o["enabled"] = dmxConfig_.enabled;
    Json::Value mappings(Json::arrayValue);
    for (const auto &kv : dmxConfig_.channelMap) {
      Json::Value m(Json::objectValue);
      m["offset"] = kv.first;
      m["function"] = kv.second;
      mappings.append(m);
    }
    o["mappings"] = mappings;
    root["dmx512"] = o;
  }
  root["udp"] = buildNetworkConfigJson(udpConfig_, /*includeMode= 包含模式*/false);
  root["tcp"] = buildNetworkConfigJson(tcpConfig_, /*includeMode= 包含模式*/true);
  root["websocket"] = buildNetworkConfigJson(websocketConfig_, /*includeMode= 包含模式*/false);
  root["function_config"] =
      functionConfig_.isObject() ? functionConfig_ : buildDefaultFunctionConfig();
  root["serial_templates"] = buildSerialTemplatesResponse();

  const std::string path = CONFIG_DIR + "peripherals.json";
  const std::string content = JsonUtils::toFormattedString(root, 2);
  if (!FileUtils::writeTextFile(path, content)) {
    LOG_ERROR("[PeripheralManager] 保存外设配置失败: %s", path.c_str());
    return false;
  }
  LOG_INFO("[PeripheralManager] 外设配置已保存: %s (%zu bytes)", path.c_str(),
           content.size());
  return true;
}

bool PeripheralManager::loadFromDisk() {
  if (CONFIG_DIR.empty()) {
    LOG_WARN("[PeripheralManager] loadFromDisk: CONFIG_DIR 未初始化");
    return false;
  }
  const std::string path = CONFIG_DIR + "peripherals.json";
  if (!FileUtils::exists(path)) {
    LOG_INFO("[PeripheralManager] 无持久化外设配置文件，使用默认值: %s",
             path.c_str());
    return false;
  }

  std::string content;
  if (!FileUtils::readFile(path, content) || content.empty()) {
    LOG_WARN("[PeripheralManager] 读取外设配置失败或文件为空: %s", path.c_str());
    return false;
  }
  Json::Value root;
  std::string err;
  if (!JsonUtils::parseJson(content, root, err) || !root.isObject()) {
    LOG_ERROR("[PeripheralManager] 外设配置 JSON 解析失败: %s", err.c_str());
    return false;
  }

  struct RestoreSerialListener {
    std::string type;
    std::string port;
    int baudrate = 9600;
  };
  std::vector<RestoreSerialListener> restoreSerialListeners;
  bool restoreDmxExternal = false;
  std::string restoreDmxExternalPort;
  int restoreDmxExternalBaudrate = 115200;

  {
  std::lock_guard<std::mutex> lock(mutex_);

  auto loadNetwork = [&](const std::string &key, NetworkConfig &cfg,
                         bool includeMode) {
    if (!root.isMember(key) || !root[key].isObject()) return;
    const Json::Value &o = root[key];
    cfg.bindAddress = o.get("bind_address", cfg.bindAddress).asString();
    cfg.bindPort = o.get("bind_port", cfg.bindPort).asInt();
    cfg.targetAddress = o.get("target_address", cfg.targetAddress).asString();
    cfg.targetPort = o.get("target_port", cfg.targetPort).asInt();
    cfg.enabled = o.get("enabled", cfg.enabled).asBool();
    cfg.multicastGroup = o.get("multicast_group", cfg.multicastGroup).asString();
    if (includeMode) cfg.mode = o.get("mode", cfg.mode).asString();
    if (o.isMember("path")) cfg.path = o["path"].asString();
    parseNetworkMappings(cfg, o); // 复用 set_config 的解析逻辑
  };

  if (root.isMember("rs232") && root["rs232"].isObject()) {
    const Json::Value &o = root["rs232"];
    rs232Config_.port = o.get("port", "").asString();
    rs232Config_.baudrate = o.get("baudrate", 0).asInt();
    rs232Config_.enabled = o.get("enabled", false).asBool();
    rs232Config_.learnMode = o.get("learn_mode", false).asBool();
    rs232Config_.cmdMap.clear();
    if (o.isMember("commands") && o["commands"].isArray()) {
      for (const auto &c : o["commands"]) {
        rs232Config_.cmdMap[c.get("hex", "").asString()] =
            c.get("action", "").asString();
      }
    }
  }
  if (root.isMember("rs485") && root["rs485"].isObject()) {
    const Json::Value &o = root["rs485"];
    rs485Config_.port = o.get("port", rs485Config_.port).asString();
    rs485Config_.baudrate = o.get("baudrate", rs485Config_.baudrate).asInt();
    rs485Config_.slaveId = o.get("slave_id", rs485Config_.slaveId).asInt();
    rs485Config_.enabled = o.get("enabled", false).asBool();
    rs485Config_.learnMode = o.get("learn_mode", false).asBool();
  }
  if (root.isMember("dmx512") && root["dmx512"].isObject()) {
    const Json::Value &o = root["dmx512"];
    dmxConfig_.inputMode =
        parseDmxInputMode(o.get("mode", dmxInputModeToString(dmxConfig_.inputMode))
                              .asString());
    dmxConfig_.port = o.get("port", dmxConfig_.port).asString();
    dmxConfig_.externalPort =
        o.get("external_port", dmxConfig_.externalPort).asString();
    dmxConfig_.externalBaudrate = normalizeBaudrate(
        o.get("external_baudrate",
              o.get("baud_rate", dmxConfig_.externalBaudrate))
            .asInt(),
        115200);
    dmxConfig_.externalDataBits = normalizeDataBits(
        o.get("external_data_bit",
              o.get("data_bit", dmxConfig_.externalDataBits))
            .asInt());
    dmxConfig_.externalStopBits = normalizeStopBits(
        o.get("external_stop_bit",
              o.get("stop_bit", dmxConfig_.externalStopBits))
            .asInt());
    dmxConfig_.externalEnabled =
        o.get("external_enable",
              o.get("enable", dmxConfig_.externalEnabled))
            .asBool();
    dmxConfig_.externalHandleEnabled =
        !o.get("stop_handle", !dmxConfig_.externalHandleEnabled).asBool();
    // 旧配置中的 stop_material 已忽略；CH6/CH7 保持启用。
    dmxConfig_.externalMaterialEnabled = true;
    dmxConfig_.externalShowInfo =
        o.get("dmx_info_show", dmxConfig_.externalShowInfo).asBool();
    dmxConfig_.externalInfoPos =
        o.get("dmx_info_pos", dmxConfig_.externalInfoPos).asString();
    dmxConfig_.electronVersion =
        o.get("electron_version", dmxConfig_.electronVersion).asInt();
    dmxConfig_.effectInterval =
        o.get("effect_interval", dmxConfig_.effectInterval).asInt();
    dmxConfig_.startAddress =
        clampDmxAddress(o.get("address",
                              o.get("light", dmxConfig_.startAddress))
                            .asInt());
    dmxConfig_.baudrate = 250000;
    dmxConfig_.masterDimmer = o.get("master", dmxConfig_.masterDimmer).asInt();
    dmxConfig_.enabled = o.get("enabled", false).asBool();
    if (dmxConfig_.port == "artnet")
      dmxConfig_.protocol = DmxProtocol::ARTNET;
    else if (dmxConfig_.port == "sacn")
      dmxConfig_.protocol = DmxProtocol::SACN;
    else
      dmxConfig_.protocol = DmxProtocol::SERIAL;
    dmxConfig_.channelMap.clear();
    if (o.isMember("mappings") && o["mappings"].isArray()) {
      for (const auto &m : o["mappings"]) {
        dmxConfig_.channelMap[m.get("offset", 0).asInt()] =
            m.get("function", "").asString();
      }
    }
  }

  loadNetwork("udp", udpConfig_, /*includeMode= 包含模式*/false);
  loadNetwork("tcp", tcpConfig_, /*includeMode= 包含模式*/true);
  loadNetwork("websocket", websocketConfig_, /*includeMode= 包含模式*/false);
  if (root.isMember("function_config") && root["function_config"].isObject()) {
    functionConfig_ = root["function_config"];
  } else if (!functionConfig_.isObject()) {
    functionConfig_ = buildDefaultFunctionConfig();
  }

  if (rs232Config_.enabled && !rs232Config_.port.empty()) {
    restoreSerialListeners.push_back(
        {"rs232", rs232Config_.port,
         rs232Config_.baudrate > 0 ? rs232Config_.baudrate : 9600});
  }
  if (rs485Config_.enabled && !rs485Config_.port.empty()) {
    restoreSerialListeners.push_back(
        {"rs485", rs485Config_.port,
         rs485Config_.baudrate > 0 ? rs485Config_.baudrate : 9600});
  }
  if (dmxConfig_.enabled &&
      dmxConfig_.inputMode == DmxInputMode::EXTERNAL &&
      dmxConfig_.externalEnabled &&
      !dmxConfig_.externalPort.empty()) {
    restoreDmxExternal = true;
    restoreDmxExternalPort = dmxConfig_.externalPort;
    restoreDmxExternalBaudrate = dmxConfig_.externalBaudrate;
  }

  serialTemplates_.clear();
  if (root.isMember("serial_templates") && root["serial_templates"].isArray()) {
    for (const auto &item : root["serial_templates"]) {
      if (!item.isObject()) continue;
      SerialTemplate tpl;
      if (item.isMember("type")) {
        tpl.type = normalizeSerialTemplateType(item.get("type", "").asString());
      }
      tpl.code = item.get("code", "").asString();
      tpl.name = item.get("name", tpl.code).asString();
      tpl.data = item.get("data", tpl.code).asString();
      tpl.functions = item.get("functions", Json::Value(Json::arrayValue));
      tpl.forwardToSerial = item.get("forward_to_serial", false).asBool();
      if (!tpl.code.empty()) {
        serialTemplates_.push_back(std::move(tpl));
      }
    }
  }

  // 启动时恢复组播组（loadFromDisk 在服务器启动后调用，UdpServer 已就绪）
  if (!udpConfig_.multicastGroup.empty()) {
    if (auto *udp = NetworkManager::getInstance().getUdpServer()) {
      if (udp->joinMulticastGroup(udpConfig_.multicastGroup)) {
        LOG_INFO("[PeripheralManager] 已恢复组播组: %s", udpConfig_.multicastGroup.c_str());
      } else {
        LOG_WARN("[PeripheralManager] 恢复组播组 %s 失败", udpConfig_.multicastGroup.c_str());
      }
    }
  }

  LOG_INFO("[PeripheralManager] 外设配置已加载: udp=%zu triggers, tcp=%zu triggers",
           udpConfig_.triggers.size(), tcpConfig_.triggers.size());
  }

  for (const auto &listener : restoreSerialListeners) {
    if (restoreDmxExternal && listener.type == "rs232") {
      LOG_INFO("[PeripheralManager] 跳过 RS232 串口监听恢复，DMX512 外接模式占用 RS232: %s",
               listener.port.c_str());
      continue;
    }
    std::string error;
    if (!startSerialLearnThread(listener.type, listener.port,
                                listener.baudrate, error)) {
      LOG_WARN("[PeripheralManager] 恢复串口监听失败: %s %s @ %d, %s",
               listener.type.c_str(), listener.port.c_str(),
               listener.baudrate, error.c_str());
      {
        std::lock_guard<std::mutex> lock(mutex_);
        SerialConfig &cfg =
            (listener.type == "rs485") ? rs485Config_ : rs232Config_;
        cfg.learnMode = false;
        saveToDisk();
      }
    } else {
      LOG_INFO("[PeripheralManager] 已恢复串口监听: %s %s @ %d",
               listener.type.c_str(), listener.port.c_str(),
               listener.baudrate);
    }
  }

  if (restoreDmxExternal) {
    std::string error;
    if (!startDmxExternalInput(restoreDmxExternalPort,
                               restoreDmxExternalBaudrate, error)) {
      LOG_WARN("[PeripheralManager] 恢复 DMX512 外接 RS232 失败: %s %s @ %d, %s",
               "rs232", restoreDmxExternalPort.c_str(),
               restoreDmxExternalBaudrate, error.c_str());
      {
        std::lock_guard<std::mutex> lock(mutex_);
        dmxConfig_.inputMode = DmxInputMode::LOCAL;
        saveToDisk();
      }
    } else {
      LOG_INFO("[PeripheralManager] 已恢复 DMX512 外接 RS232: %s @ %d",
               restoreDmxExternalPort.c_str(), restoreDmxExternalBaudrate);
    }
  }

  return true;
}

uint8_t PeripheralManager::getChannelValue(int channel) const {
  if (channel < 0 || channel >= 512)
    return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  auto simulationValueAt = [this](int index) -> uint8_t {
    if (index >= 0 && index < static_cast<int>(simulationChannels_.size())) {
      return simulationChannels_[index];
    }
    return 0;
  };

  if (simulationMode_) {
    if (channel > 0 && channel < 12 && simulationValueAt(0) == 0) {
      return 0;
    }
    return simulationValueAt(channel);
  }
  if (dmxConfig_.inputMode == DmxInputMode::EXTERNAL) {
    if (channel > 0 && channel < 12 && simulationValueAt(0) == 0) {
      return 0;
    }
    return simulationValueAt(channel);
  }
  if (engine_ && engine_->getDmxReceiver()) {
    auto *receiver = engine_->getDmxReceiver();
    if (channel > 0 && channel < 12 && receiver->getChannelValue(0) == 0) {
      return 0;
    }
    return receiver->getChannelValue(channel);
  }
  if (channel > 0 && channel < 12 && simulationValueAt(0) == 0) {
    return 0;
  }
  return simulationValueAt(channel);
}

void PeripheralManager::setDmxMaster(int value) {
  std::lock_guard<std::mutex> lock(mutex_);
  dmxConfig_.masterDimmer = std::clamp(value, 0, 255);
  updateDmxOutput();
}

int PeripheralManager::getDmxMaster() {
  std::lock_guard<std::mutex> lock(mutex_);
  return dmxConfig_.masterDimmer;
}

void PeripheralManager::setDmxChannel(int channel, int value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel >= 0 && channel < static_cast<int>(simulationChannels_.size())) {
    simulationChannels_[channel] =
        static_cast<uint8_t>(std::clamp(value, 0, 255));
  }
  updateDmxOutput();
}

void PeripheralManager::resetDmxChannelsToDefault() {
  std::array<uint8_t, 12> zeroChannels{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dmxConfig_.masterDimmer = 255;
    if (simulationChannels_.size() < 512) {
      simulationChannels_.assign(512, 0);
    }
    for (int i = 0; i < 12 && i < static_cast<int>(simulationChannels_.size()); ++i) {
      simulationChannels_[i] = 0;
    }
    updateDmxOutput();
  }
  if (engine_ && engine_->getDmxReceiver()) {
    engine_->getDmxReceiver()->injectChannelData(
        0, zeroChannels.data(), zeroChannels.size());
  }
}

std::string PeripheralManager::sendTest(const std::string &target,
                                        const std::string &data) {
  (void)target;
  (void)data;
  return "SENT_OK";
}

void PeripheralManager::applyConfigs() {
  // Network protocol services managed by Network管理器 to avoid duplicate UDP/TCP/WS servers
}

void PeripheralManager::broadcastResult(const std::string &resultJson) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 在广播的 JSON 中注入 "_broadcast":true 标记，防止接收端把它当命令执行（环回）
  // 各通道（WebSocket/UDP/TCP）收到消息时检查此字段，有则跳过 processCommand
  std::string broadcastJson = resultJson;
  if (!resultJson.empty() && resultJson.back() == '}') {
    broadcastJson = resultJson.substr(0, resultJson.size() - 1) + ",\"_broadcast\":true}";
  }

  // UDP: 仅 send to registered clients
  if (udpConfig_.enabled) {
    if (auto *udp = NetworkManager::getInstance().getUdpServer();
        udp && udp->isRunning()) {
      size_t clientCount = udp->getClientCount();
      if (clientCount > 0) {
        const int targetPort =
            udpConfig_.targetPort > 0 ? udpConfig_.targetPort : udp->getPort();
        size_t sentCount = udp->broadcastToClientIps(targetPort, broadcastJson);
        LOG_DEBUG("PeripheralManager: Broadcasted result to %zu/%zu UDP clients "
                 "on port %d (%zu bytes)",
                 sentCount, clientCount, targetPort, broadcastJson.length());
      } else {
        LOG_DEBUG("PeripheralManager: No UDP clients registered");
      }
    }
  }

  // WebSocket：广播给所有已连接的 WS 客户端
  if (websocketConfig_.enabled) {
    if (auto *ws = NetworkManager::getInstance().getWebSocketServer();
        ws && ws->isRunning()) {
      ws->broadcast(broadcastJson);
      LOG_DEBUG("PeripheralManager: Broadcasted result to WebSocket clients "
               "(%zu bytes)",
               broadcastJson.length());
    }
  }

  // SSE: 广播到 HttpServer 的 SSE 客户端（用于web端显示接收数据）
  if (auto *http = NetworkManager::getInstance().getHttpServer();
      http) {
    http->broadcastSSE("tcp_recv", broadcastJson);
    http->broadcastSSE("udp_recv", broadcastJson);
    http->broadcastSSE("serial_recv", broadcastJson);
    LOG_DEBUG("PeripheralManager: Broadcasted result to SSE clients (%zu bytes)",
             broadcastJson.length());
  }
}

void PeripheralManager::broadcastTriggerEvent(const Json::Value &event) {
  Json::Value payload = event.isObject() ? event : Json::Value(Json::objectValue);
  payload["type"] = "peripheral_trigger";
  if (!payload.isMember("timestamp")) {
    payload["timestamp"] = static_cast<Json::Int64>(std::time(nullptr));
  }

  const std::string json = JsonUtils::toString(payload);
  if (auto *http = NetworkManager::getInstance().getHttpServer(); http) {
    http->broadcastSSE("peripheral_trigger", json);
  }
  if (auto *http = NetworkManager::getInstance().getVodHttpServer(); http) {
    http->broadcastSSE("peripheral_trigger", json);
  }
}

void PeripheralManager::updateDmxOutput() {
  if (auto *http = NetworkManager::getInstance().getHttpServer(); http) {
    Json::Value state;
    state["enabled"] = dmxConfig_.enabled;
    state["mode"] = dmxInputModeToString(dmxConfig_.inputMode);
    state["external_port"] = dmxConfig_.externalPort;
    state["external_baudrate"] = dmxConfig_.externalBaudrate;
    state["external_data_bit"] = dmxConfig_.externalDataBits;
    state["external_stop_bit"] = dmxConfig_.externalStopBits;
    state["external_enable"] = dmxConfig_.externalEnabled;
    state["stop_handle"] = !dmxConfig_.externalHandleEnabled;
    state["stop_material"] = !dmxConfig_.externalMaterialEnabled;
    state["dmx_info_show"] = dmxConfig_.externalShowInfo;
    state["electron_version"] = dmxConfig_.electronVersion;
    state["effect_interval"] = dmxConfig_.effectInterval;
    state["external_running"] = isDmxExternalInputRunning();
    state["master"] = dmxConfig_.masterDimmer;
    state["address"] = dmxConfig_.startAddress;
    state["protocol"] = dmxConfig_.protocol == DmxProtocol::ARTNET
                            ? "artnet"
                            : (dmxConfig_.protocol == DmxProtocol::SACN ? "sacn" : "serial");
    Json::Value channels(Json::arrayValue);
    const int count = std::min<int>(512, simulationChannels_.size());
    for (int i = 0; i < count; ++i) {
      channels.append(static_cast<int>(simulationChannels_[i]));
    }
    state["channels"] = channels;
    http->broadcastSSE("dmx_state", JsonUtils::toString(state));
  }
}

void PeripheralManager::logSerialPortStatus() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (rs232Config_.enabled && !rs232Config_.port.empty()) {
    LOG_DEBUG("[Step 1.4] RS232: %s (baudrate: %d, enabled)",
             rs232Config_.port.c_str(), rs232Config_.baudrate);
  } else {
    LOG_DEBUG("[Step 1.4] RS232: Not configured or disabled");
  }

  if (rs485Config_.enabled && !rs485Config_.port.empty()) {
    LOG_DEBUG("[Step 1.4] RS485: %s (baudrate: %d, slaveId: %d, enabled)",
             rs485Config_.port.c_str(), rs485Config_.baudrate,
             rs485Config_.slaveId);
  } else {
    LOG_DEBUG("[Step 1.4] RS485: %s (not enabled)",
             rs485Config_.port.empty() ? "Not configured" : rs485Config_.port.c_str());
  }

  if (dmxConfig_.enabled) {
    const char *protocolStr = "SERIAL";
    if (dmxConfig_.protocol == DmxProtocol::ARTNET) {
      protocolStr = "ARTNET";
    } else if (dmxConfig_.protocol == DmxProtocol::SACN) {
      protocolStr = "SACN";
    }
    LOG_DEBUG("[Step 1.4] DMX512: mode=%s local=%s protocol=%s external=%s@%d data=%d stop=%d extEnabled=%d handle=%d material=%d electron=%d startAddr=%d master=%d enabled",
             dmxInputModeToString(dmxConfig_.inputMode), dmxConfig_.port.c_str(),
             protocolStr,
             dmxConfig_.externalPort.empty() ? "-" : dmxConfig_.externalPort.c_str(),
             dmxConfig_.externalBaudrate, dmxConfig_.externalDataBits,
             dmxConfig_.externalStopBits, dmxConfig_.externalEnabled,
             dmxConfig_.externalHandleEnabled, dmxConfig_.externalMaterialEnabled,
             dmxConfig_.electronVersion, dmxConfig_.startAddress,
             dmxConfig_.masterDimmer);
  } else {
    LOG_DEBUG("[Step 1.4] DMX512: %s (not enabled)",
             dmxConfig_.port.empty() ? "Not configured" : dmxConfig_.port.c_str());
  }
}

} // 命名空间 hsvj
