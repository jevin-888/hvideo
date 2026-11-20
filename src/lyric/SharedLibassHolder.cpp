/**
 * @file SharedLibassHolder.cpp（文件名）
 * @brief 全局共享 libass 实例实现（ASS_Library + ASS_渲染器），仅 Layer 21 歌词使用
 */

#include "lyric/SharedLibassHolder.h"
#include "foundation/PathConfig.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>

extern "C" {
#include "ass.h"
}

namespace hsvj {

SharedLibassHolder::SharedLibassHolder() {
  library_ = ass_library_init();
  if (!library_) {
    LOG_ERROR("[SharedLibassHolder] ass_library_init failed");
    return;
  }
  // 禁用 ASS 内嵌字体提取，避免 [Fonts] 段多款字体导致 100MB+ 内存
  // 使用 ensureAppFontsLoaded 的 lyric.ttf/lyric-Pinyin.ttf 作为 fallback
  ass_set_extract_fonts(library_, 0);

  renderer_ = ass_renderer_init(library_);
  if (!renderer_) {
    LOG_ERROR("[SharedLibassHolder] ass_renderer_init failed");
    ass_library_done(library_);
    library_ = nullptr;
    return;
  }

  // 缓存限制使用 libass 修改后的默认值（ass_render.h：字形 2000、位图 8MB）
  ass_set_cache_limits(renderer_, 2000, 32);
  ass_set_pixel_aspect(renderer_, 1.0);
  ass_set_hinting(renderer_, ASS_HINTING_NONE);
  ass_set_shaper(renderer_, ASS_SHAPING_SIMPLE);
  ass_set_fonts(renderer_, nullptr, "sans-serif", ASS_FONTPROVIDER_NONE, nullptr,
                1);
}

SharedLibassHolder::~SharedLibassHolder() {
  appFontDataBuffers_.clear();
  if (renderer_) {
    ass_renderer_done(renderer_);
    renderer_ = nullptr;
  }
  if (library_) {
    ass_library_done(library_);
    library_ = nullptr;
  }
}

void SharedLibassHolder::setFrameSizeForLyrics(int width, int height) {
  if (!renderer_) {
    return;
  }
  if (width == lastFrameWidth_ && height == lastFrameHeight_) {
    return;
  }
  lastFrameWidth_ = width;
  lastFrameHeight_ = height;
  ass_set_storage_size(renderer_, width, height);
  ass_set_frame_size(renderer_, width, height);
}

bool SharedLibassHolder::ensureAppFontsLoaded(const std::string &fontDir,
                                              const std::string &appFontsDir) {
  if (fontsLoaded_ || !library_ || !renderer_) {
    return fontsLoaded_;
  }

  std::string baseDir = appFontsDir.empty() ? fontDir : appFontsDir;
  if (!baseDir.empty() && baseDir.back() == '/') {
    baseDir.pop_back();
  }

  std::vector<std::string> paths = FileUtils::listFiles(baseDir);
  paths.erase(std::remove_if(paths.begin(), paths.end(), [](const std::string &path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return true;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext != ".ttf" && ext != ".otf" && ext != ".ttc";
  }), paths.end());
  std::sort(paths.begin(), paths.end(), [&baseDir](const std::string &a, const std::string &b) {
    auto rank = [&baseDir](const std::string &p) {
      if (p == baseDir + "/默认字体.ttf") return 0;
      if (p == baseDir + "/lyric.ttf") return 1;
      return 2;
    };
    return rank(a) == rank(b) ? a < b : rank(a) < rank(b);
  });

  std::string firstLoadedPath;
  for (const std::string &path : paths) {
    std::vector<char> data = FileUtils::readBinaryFile(path);
    if (data.empty()) {
      continue;
    }
    size_t size = data.size();
    if (size >= 100 * 1024 * 1024u) {
      continue;
    }
    appFontDataBuffers_.push_back(std::move(data));
    char *ptr = appFontDataBuffers_.back().data();
    const std::string name = path.substr(path.find_last_of('/') + 1);
    ass_add_font(library_, name.c_str(), ptr, static_cast<int>(size));
    if (firstLoadedPath.empty()) {
      firstLoadedPath = path;
    }
  }

  if (!firstLoadedPath.empty()) {
    defaultFontPath_ = firstLoadedPath;
    ass_set_fonts(renderer_, firstLoadedPath.c_str(), "lyric",
                  ASS_FONTPROVIDER_NONE, nullptr, 1);
    fontsLoaded_ = true;
    LOG_INFO("[Lyric] 字体加载 path=%s", firstLoadedPath.c_str());
  }
  return fontsLoaded_;
}

} // 命名空间 hsvj
