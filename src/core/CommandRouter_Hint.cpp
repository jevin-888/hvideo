#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "utils/Logger.h"

namespace hsvj {

void CommandRouter::triggerLayer41Hint(int type, const std::string &customText) {
  showLayer41Hint(type, customText);
}

void CommandRouter::showLayer41Hint(int type, const std::string &customText) {
  if (!mubu_) {
    LOG_WARN("CommandRouter::showLayer41Hint: mubu_ is null");
    return;
  }

  // 获取 Layer 41
  Layer *layer41 = mubu_->getLayer(41);
  if (!layer41) {
    LOG_WARN("CommandRouter::showLayer41Hint: Layer 41 does not exist");
    return;
  }

  if (layer41->getType() != LayerType::TEXT) {
    LOG_WARN("CommandRouter::showLayer41Hint: Layer 41 is not TEXT type");
    return;
  }

  LayerText *textLayer = static_cast<LayerText *>(layer41);

  // 确保 Layer 41 可见
  if (!textLayer->isVisible()) {
    LOG_DEBUG("CommandRouter::showLayer41Hint: Making Layer 41 visible");
    textLayer->setVisible(true);
  }

  LOG_DEBUG("CommandRouter::showLayer41Hint: Calling showOperationHint, type=%d, text='%s'", type, customText.c_str());
  textLayer->showOperationHint(static_cast<HintType>(type), customText);
  LOG_DEBUG("CommandRouter::showLayer41Hint: showOperationHint called successfully, type=%d, visible=%d", type, textLayer->isVisible());
}

bool CommandRouter::tryLoadLyricForVideo(int layerId, LayerVideo *videoLayer,
                                         const std::string & /*视频路径*/) {
  if (!systemConfig_ || !systemConfig_->isLyricEnabled() ||
      !systemConfig_->hasLayerConfig(21)) {
    return false; // 歌词功能未启用或未配置 Layer21
  }

  Layer *lyricLayer = mubu_->getLayer(21);
  if (!lyricLayer || lyricLayer->getType() != LayerType::TEXT) {
    return false; // 歌词图层不存在或类型不匹配
  }

  LayerText *lyricTextLayer = static_cast<LayerText *>(lyricLayer);

  // 只有当当前播放的图层是歌词层绑定的图层时，才关联回调
  // 注意：实际的歌词加载统一由 Engine::update() 根据视频路径触发，
  // 这里不再直接调用 autoLoadLyric，避免与 Engine 双线程并发访问 Lyric渲染器/ASS渲染器。
  if (layerId == lyricTextLayer->getBindLayerId()) {
    lyricTextLayer->setCurrentTimeCallback(
        [videoLayer]() { return videoLayer->getCurrentPosition(); });

    // 这里仅完成时间回调绑定，返回当前歌词层是否已处于已加载状态。
    // 实际加载将在 Engine::update() 中，根据当前 videoLayer->getCurrentPath() 统一处理。
    return lyricTextLayer->isLyricLoaded();
  }

  return false;
}

void CommandRouter::updateLayer41PlaylistHint(const std::string &playlistId,
                                              int /*图层 ID*/) {
  if (!mubu_ || playlistId.empty())
    return;

  // 获取 Layer 41
  Layer *layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT) {
    return;
  }

  LayerText *textLayer = static_cast<LayerText *>(layer41);

  // 同步播放列表ID
  textLayer->setPlaylistId(playlistId);
  textLayer->setPlaylistHintSuppressAfterSwitch(false);  // 用户主动请求播放列表，解除抑制

  // 切换视频时重置时序状态，让 Engine::update播放列表HintLayer 按 endHintTime 配置延迟显示
  textLayer->setLastCurrentPos(-1.0);
  textLayer->setLastRemainingTime(-1.0);
  textLayer->setPlaylistHintState(0);
  // 不调用 record播放列表HintStartTime()，计时由 Engine 在 currentPos >= endHintTime 时触发
}

void CommandRouter::suppressLayer41PlaylistHintForNextVideo() {
  if (!mubu_)
    return;

  Layer *layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT)
    return;

  LayerText *textLayer = static_cast<LayerText *>(layer41);
  float endHintTime = textLayer->getEndHintTime();
  if (endHintTime <= 0)
    endHintTime = 3.0f;

  // 将 lastCurrentPos 设为 >= endHintTime，使 Engine::update播放列表HintLayer 的
  // enteredFromStartWindow 为 false，从而不触发「已选列表」显示
  textLayer->setLastCurrentPos(static_cast<double>(endHintTime) + 1.0);
  textLayer->setLastRemainingTime(0.0);
  textLayer->setPlaylistHintState(0);
  textLayer->setPlaylistHintSuppressAfterSwitch(true);
  textLayer->clearPlaylistHintTimeRecord();
  // 立即记录切歌时刻，确保 elapsed 从 0 开始，按 endHintTime 配置延迟显示
  textLayer->recordPlaylistHintStartTime();
  textLayer->getMessageHintRenderer()->setPlaylistHintVisible(false);
}

} // 命名空间 hsvj
