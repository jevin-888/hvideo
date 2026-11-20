#include "core/Engine.h"
#include "core/CommandRouter.h"
#include "core/Mubu.h"
#include "core/QrCodeCard.h"
#include "foundation/PathConfig.h"
#include "layer/LayerImage.h"
#include "network/OnlineVodWsClient.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <cstdio>

namespace hsvj {
namespace {
std::string cardKey(const LayerConfigData& cfg) {
  Json::Value key(Json::arrayValue);
  key.append(cfg.qrContent); key.append(cfg.qrSize); key.append(cfg.qrText);
  key.append(cfg.qrTextColor); key.append(cfg.qrFgColor); key.append(cfg.qrBgColor);
  key.append(cfg.qrErrorCorrection); key.append(cfg.fontPath);
  return JsonUtils::toString(key);
}
}

void Engine::resetConfiguredQrCode() {
  if (configuredQrFuture_.valid()) configuredQrFuture_.get();
  if (!configuredQrPendingPath_.empty()) std::remove(configuredQrPendingPath_.c_str());
  configuredQrPendingPath_.clear();
  configuredQrPendingKey_.clear();
  configuredQrAppliedKey_.clear();
  configuredQrNextCheck_ = {};
}

void Engine::refreshConfiguredQrCode() {
  if (shuttingDown_.load() || !systemConfig_ || !mubu_) return;
  const auto now = std::chrono::steady_clock::now();
  const bool finished = configuredQrFuture_.valid() &&
      configuredQrFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  if (!finished && now < configuredQrNextCheck_) return;
  configuredQrNextCheck_ = now + std::chrono::seconds(2);

  const auto* saved = systemConfig_->getLayerConfig(71);
  auto* layer = mubu_->getLayer(71);
  const bool valid = saved && layer &&
      (layer->getType() == LayerType::QRCODE || layer->getType() == LayerType::IMAGE);
  LayerConfigData desired;
  std::string key;
  if (valid) {
    desired = *saved;
    const bool online = systemConfig_->isNetworkVodEnabled();
    const std::string host = systemConfig_->getOnlineVodHost();
    const std::string ip = onlineVodWsClient_ && host == lastOnlineVodWsHost_
        ? onlineVodWsClient_->localAddress() : "";
    desired.qrContent = resolveVodMobileQr(saved->qrContent, online,
        host.empty() ? "" : "http://" + host + ":9898", ip);
    if (!desired.qrContent.empty()) key = cardKey(desired);
  }
  const std::string path = QR_CODE_DIR + "qrcode_71.png";
  auto* image = valid ? static_cast<LayerImage*>(layer) : nullptr;
  if (finished) {
    const bool generated = configuredQrFuture_.get();
    bool committed = false;
    // Scene/config/network may change while CPU rasterization is running.
    if (generated && !key.empty() && key == configuredQrPendingKey_ &&
        std::rename(configuredQrPendingPath_.c_str(), path.c_str()) == 0) {
      image->invalidateCache();
      if (image->loadImage(path)) {
        if (desired.size.width > 0 && desired.size.height > 0) image->setSize(desired.size);
        image->setPosition(desired.position);
        image->setVisible(desired.visible);
        image->setContentReady(true);
        configuredQrAppliedKey_ = key;
        committed = true;
        if (saved->qrContent != desired.qrContent) {
          systemConfig_->setLayerConfig(71, desired);
          if (commandRouter_) commandRouter_->requestSystemConfigSave("device QR address");
        }
        LOG_INFO("[QRCode] Restored configured card: %s", desired.qrContent.c_str());
      }
    } else if (!generated) {
      LOG_WARN("[QRCode] Card generation failed; will retry");
    }
    std::remove(configuredQrPendingPath_.c_str());
    configuredQrPendingPath_.clear();
    if (key == configuredQrPendingKey_ && !committed) return;
  }
  if (!valid) return;
  if (key.empty()) {
    // Never display a template's foreign room while waiting for network identity.
    image->setContentReady(false);
    return;
  }
  if (key == configuredQrAppliedKey_ && FileUtils::exists(path) &&
      image->getImagePath() == path && image->getWidth() > 0 && image->isContentReady()) {
    return;
  }
  image->setContentReady(false);
  if (configuredQrFuture_.valid()) return;
  FileUtils::createDirectory(QR_CODE_DIR);
  configuredQrPendingPath_ = path + ".pending.png";
  configuredQrPendingKey_ = key;
  const std::string font = FONT_DIR + (desired.fontPath.empty() ? "默认字体.ttf" : desired.fontPath);
  const std::string pendingPath = configuredQrPendingPath_;
  configuredQrFuture_ = std::async(std::launch::async, [desired, font, pendingPath]() {
    return writeQrCodeCard(desired, font, pendingPath);
  });
}
} // namespace hsvj
