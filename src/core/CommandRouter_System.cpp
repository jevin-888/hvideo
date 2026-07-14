#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/SystemConfig.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <chrono>
#include <sstream>
#include <thread>
#if defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#endif

extern "C" void callJavaRestartMethod();

// 应用版本号（由 jni_interface.cpp 在 initialize 时设置）
extern std::string g_appVersion;

namespace hsvj {

CommandResponse CommandRouter::handleSystemConfig(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x00;
  response.timestamp = std::time(nullptr);

  if (!systemConfig_) {
    setParamError(response, "SystemConfig not initialized");
    return response;
  }

  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  // 检查是否是查询操作
  if (param.isMember("action") && param["action"].asString() == "status") {
    // 查询系统状- 直接返回内存中的配置
    // 注意：不再每次查询都重新加载config.json，因为这可能导致多线程竞

    Json::Value data;
    Resolution res = systemConfig_->getResolution();
    data["resolution"] = res.toString();
    data["audio_route_mode"] = "auto";
    data["audio_route_authority"] = "Engine::syncAudioOutputLayer";
    data["device_type"] = systemConfig_->getDeviceType();
    data["screen_rotate"] = systemConfig_->getScreenRotate();
    data["dmx_baudrate"] = systemConfig_->getDmxBaudRate();
    data["dmx_start_address"] = systemConfig_->getDmxStartAddress();
    data["networkIpMode"] = systemConfig_->getNetworkIpMode();
    data["networkStaticIp"] = systemConfig_->getNetworkStaticIp();
    data["networkGateway"] = systemConfig_->getNetworkGateway();
    data["networkDns"] = systemConfig_->getNetworkDns();
    data["debugHotspotEnabled"] = systemConfig_->isDebugHotspotEnabled();
    data["renderFrameRateMode"] = systemConfig_->getRenderFrameRateMode();
    data["renderQuality"] = systemConfig_->getRenderQuality();
    data["powerScheduleEnabled"] = systemConfig_->isPowerScheduleEnabled();
    data["powerOnScheduleEnabled"] = systemConfig_->isPowerOnScheduleEnabled();
    data["powerOnDate"] = systemConfig_->getPowerOnDate();
    data["powerOnTime"] = systemConfig_->getPowerOnTime();
    data["powerOffScheduleEnabled"] = systemConfig_->isPowerOffScheduleEnabled();
    data["powerOffDate"] = systemConfig_->getPowerOffDate();
    data["powerOffTime"] = systemConfig_->getPowerOffTime();
    data["app_version"] = g_appVersion;

    // 返回矩阵配置参数（统一字段）
    const int canvasInWidth = systemConfig_->getInputWidth() > 0
        ? systemConfig_->getInputWidth()
        : systemConfig_->getResolution().width;
    const int canvasInHeight = systemConfig_->getInputHeight() > 0
        ? systemConfig_->getInputHeight()
        : systemConfig_->getResolution().height;
    const int layoutInCols = systemConfig_->getInputLayoutCols();
    const int layoutInRows = systemConfig_->getInputLayoutRows();
    const int tileInWidth = systemConfig_->getRegionWidth();
    const int tileInHeight = systemConfig_->getRegionHeight();
    // 遮罩/ZheZhao 使用输入合成总幕布尺寸；输出矩阵尺寸不能参与这里的比例。
    const int inputTotalWidth =
        (tileInWidth > 0 && layoutInCols > 0) ? tileInWidth * layoutInCols
                                             : canvasInWidth;
    const int inputTotalHeight =
        (tileInHeight > 0 && layoutInRows > 0) ? tileInHeight * layoutInRows
                                               : canvasInHeight;
    const int canvasOutWidth = systemConfig_->getOutputWidth();
    const int canvasOutHeight = systemConfig_->getOutputHeight();
    const int layoutOutCols = systemConfig_->getOutputLayoutCols();
    const int layoutOutRows = systemConfig_->getOutputLayoutRows();

    data["canvas_in_width"] = canvasInWidth;
    data["canvas_in_height"] = canvasInHeight;
    data["input_total_width"] = inputTotalWidth;
    data["input_total_height"] = inputTotalHeight;
    data["canvas_out_width"] = canvasOutWidth;
    data["canvas_out_height"] = canvasOutHeight;
    data["tile_in_width"] = tileInWidth;
    data["tile_in_height"] = tileInHeight;
    // API 字段名显式区分 rows/cols；前端显示时必须按行×列(rows×cols)组合。
    data["layout_in_cols"] = layoutInCols;
    data["layout_in_rows"] = layoutInRows;
    data["tile_out_width"] = layoutOutCols > 0 ? canvasOutWidth / layoutOutCols : 0;
    data["tile_out_height"] = layoutOutRows > 0 ? canvasOutHeight / layoutOutRows : 0;
    data["layout_out_cols"] = layoutOutCols;
    data["layout_out_rows"] = layoutOutRows;
    data["split_direction"] = systemConfig_->getSplitDirection();
    data["rotation_angle"] = roundFloat2(systemConfig_->getRotationAngle());
    data["rotation"] = static_cast<int>(systemConfig_->getRotationAngle());

    LOG_DEBUG("config.json 读取系统状态 输入分辨率%dx%d 输出分辨率%dx%d", canvasInWidth,
             canvasInHeight, canvasOutWidth, canvasOutHeight);

    response.ok = true;
    response.error = 0x0000;
    response.message = "系统状态查询成功";
    response.dataJson = hsvj::JsonUtils::toString(data);
    return response;
  }

  // 设置操作
  Json::Value data;
  bool updated = false;

  if (param.isMember("resolution") && param["resolution"].isString()) {
    std::string resStr = param["resolution"].asString();
    Resolution res = Resolution::fromString(resStr);
    if (res.width > 0 && res.height > 0) {
      systemConfig_->setResolution(res);
      data["resolution"] = resStr;
      updated = true;

      // 分辨率已更新，无需重新初始化区域渲染器
    }
  }

  if (param.isMember("audio_type") && param["audio_type"].isInt()) {
    int audioType = param["audio_type"].asInt();
    if (audioType >= 0 && audioType <= 3) {
      data["audio_type"] = audioType;
      data["audio_type_deprecated"] = true;
      data["audio_route_mode"] = "auto";
      updated = true;
      LOG_WARN("audio_type=%d 已废弃并忽略；运行时音频路由由 Engine::syncAudioOutputLayer 统一控制", audioType);
    } else {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Invalid audio_type: must be 0-3";
      return response;
    }
  }

  if (param.isMember("device_type") && param["device_type"].isInt()) {
    systemConfig_->setDeviceType(param["device_type"].asInt());
    data["device_type"] = param["device_type"].asInt();
    updated = true;
  }

  if (param.isMember("screen_rotate") && param["screen_rotate"].isInt()) {
    int rotate = param["screen_rotate"].asInt();
    if (rotate == 0 || rotate == 90 || rotate == 180 || rotate == 270) {
      systemConfig_->setScreenRotate(rotate);
      data["screen_rotate"] = rotate;
      updated = true;
    } else {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Invalid screen_rotate: must be 0, 90, 180, or 270";
      return response;
    }
  }

  if (param.isMember("dmx_baudrate") && param["dmx_baudrate"].isInt()) {
    int baudrate = 250000;
    if (baudrate >= 0) {
      systemConfig_->setDmxBaudRate(baudrate);
      data["dmx_baudrate"] = baudrate;
      updated = true;
    } else {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Invalid dmx_baudrate: must be >= 0";
      return response;
    }
  }

  if (param.isMember("dmx_start_address") && param["dmx_start_address"].isInt()) {
    int address = param["dmx_start_address"].asInt();
    if (address >= 1 && address <= 512) {
      systemConfig_->setDmxStartAddress(address);
      data["dmx_start_address"] = address;
      updated = true;
    } else {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Invalid dmx_start_address: must be 1-512";
      return response;
    }
  }

  if (updated) {
    // 不再自动保存配置，用户需点击"保存"按钮
    // 示例/字段：systemConfig_->save(CONFIG_PATH);

    response.ok = true;
    response.error = 0x0000;
    response.message = "系统参数设置成功";
    response.dataJson = hsvj::JsonUtils::toString(data);
  } else {
    setParamError(response, "No valid parameters provided");
  }

  return response;
}

CommandResponse CommandRouter::handleSystemControl(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x10;
  response.timestamp = std::time(nullptr);

  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  std::string action =
      param.isMember("action") ? param["action"].asString() : "";

  if (action == "restart_app") {
    // 重启应用程序
    LOG_DEBUG("收到重启应用请求");
    response.ok = true;
    response.error = 0x0000;
    response.message = "正在重启应用...";

    // 调用 Java 层的重启方法
    LOG_DEBUG("调用 Java 层的重启方法");
    callJavaRestartMethod();

  } else if (action == "reboot") {
    // 重启系统
    LOG_DEBUG("收到重启系统请求");
    response.ok = true;
    response.error = 0x0000;
    response.message = "正在重启系统...";

    // 使用异步方式执行重启，避免阻塞响
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
#ifdef __ANDROID__
      // Android: 使用 reboot 命令重启系统
      system("reboot");
#endif
    }).detach();

  } else {
    setParamError(response, "Unknown action: " + action);
    return response;
  }

  return response;
}

} // 命名空间 hsvj
