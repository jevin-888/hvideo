#include "lyric/ASSRenderer.h"
#include "lyric/SharedLibassHolder.h"
#include "utils/Logger.h"

namespace hsvj {

ASSRenderer::ASSRenderer()
    : library_(nullptr), renderer_(nullptr), track_(nullptr),
      initialized_(false), videoWidth_(0), videoHeight_(0) {}

ASSRenderer::~ASSRenderer() { cleanup(); }

void ASSRenderer::freeTrackLocked() {
  if (!track_) {
    return;
  }
  ass_free_track(track_);
  track_ = nullptr;
}

bool ASSRenderer::initialize(int width, int height,
                             const std::string &fontsDir) {
  if (initialized_)
    return true;

  videoWidth_ = width;
  videoHeight_ = height;
  fontsDir_ = fontsDir;

  if (!sharedLibassHolder_ || !sharedLibassHolder_->isInitialized()) {
    LOG_ERROR("[Lyric] ASS 初始化失败：SharedLibassHolder 未注入或未就绪");
    return false;
  }

  library_ = reinterpret_cast<ASS_Library *>(sharedLibassHolder_->getLibrary());
  renderer_ = reinterpret_cast<ASS_Renderer *>(sharedLibassHolder_->getRenderer());
  if (!library_ || !renderer_) {
    LOG_ERROR("[Lyric] ASS 初始化失败：library=%d renderer=%d", library_ ? 1 : 0, renderer_ ? 1 : 0);
    library_ = nullptr;
    renderer_ = nullptr;
    return false;
  }

  if (!sharedLibassHolder_->ensureAppFontsLoaded(fontsDir_, appFontsDir_)) {
    LOG_ERROR("[Lyric] ASS 初始化失败未加载到任何字体，请确保 ttf 目录存在且包.ttf 文件");
    library_ = nullptr;
    renderer_ = nullptr;
    return false;
  }

  track_ = ass_new_track(library_);
  if (!track_) {
    LOG_ERROR("[Lyric] ASS 初始化失败ass_new_track");
    library_ = nullptr;
    renderer_ = nullptr;
    return false;
  }

  initialized_ = true;
  return true;
}

void ASSRenderer::cleanup() {
  if (track_) {
    if (sharedLibassHolder_) {
      std::lock_guard<std::mutex> lock(sharedLibassHolder_->getRenderMutex());
      freeTrackLocked();
    } else {
      freeTrackLocked();
    }
  }
  renderer_ = nullptr;
  library_ = nullptr;
  initialized_ = false;
}

void ASSRenderer::setSharedLibassHolder(SharedLibassHolder *holder) {
  sharedLibassHolder_ = holder;
}

bool ASSRenderer::loadSubtitleFile(const std::string &filename) {
  if (!initialized_ || !library_ || !sharedLibassHolder_) {
    LOG_ERROR("[Lyric] ASS 加载文件失败 未初始化");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(sharedLibassHolder_->getRenderMutex());
    freeTrackLocked();
    track_ = ass_read_file(library_, const_cast<char *>(filename.c_str()), nullptr);

    if (!track_) {
      LOG_ERROR("[Lyric] ASS 加载文件失败 ass_read_file path=%s", filename.c_str());
      track_ = ass_new_track(library_);
      return false;
    }
  }

  if (track_->n_events == 0) {
    LOG_ERROR("[Lyric] ASS 加载文件失败 无事path=%s", filename.c_str());
    return false;
  }
  return true;
}

bool ASSRenderer::resetTrack() {
  if (!initialized_ || !library_ || !sharedLibassHolder_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(sharedLibassHolder_->getRenderMutex());
  freeTrackLocked();
  track_ = ass_new_track(library_);
  return track_ != nullptr;
}

ASS_Image *ASSRenderer::renderFrame(int64_t timeMs, int *detectChange) {
  if (!initialized_ || !track_) {
    if (detectChange)
      *detectChange = 0;
    return nullptr;
  }

  if (!sharedLibassHolder_ || !renderer_) {
    if (detectChange)
      *detectChange = 0;
    return nullptr;
  }

  int change = 0;
  ASS_Image *image = nullptr;
  {
    std::lock_guard<std::mutex> lock(sharedLibassHolder_->getRenderMutex());
    sharedLibassHolder_->setFrameSizeForLyrics(videoWidth_, videoHeight_);
    image = ass_render_frame(
        reinterpret_cast<ASS_Renderer *>(sharedLibassHolder_->getRenderer()),
        track_, timeMs, &change);
  }

  if (detectChange)
    *detectChange = change;
  return image;
}

void ASSRenderer::setVideoSize(int width, int height) {
  videoWidth_ = width;
  videoHeight_ = height;
}

void ASSRenderer::setAppFontsDir(const std::string &appFontsDir) {
  appFontsDir_ = appFontsDir;
}

int ASSRenderer::getEventCount() const {
  if (!track_) {
    return 0;
  }
  return track_->n_events;
}

void ASSRenderer::updateDefaultFont(const std::string &fontPath) {
  if (!renderer_ || !sharedLibassHolder_ || fontPath.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(sharedLibassHolder_->getRenderMutex());
  // 重置 默认 font
  // Second parameter is 默认 font 路径, third is 默认 font family
  // We 设置 默认 family to "lyric" for compatibility
  // 说明：关键参数是第一个 fontPath
  ass_set_fonts(renderer_, fontPath.c_str(), "lyric",
                ASS_FONTPROVIDER_NONE, nullptr, 1);
}

} // 命名空间 hsvj
