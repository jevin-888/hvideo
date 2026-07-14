/**
 * @file SharedLibassHolder.h（文件名）
 * @brief 全局共享 libass 实例（ASS_Library + ASS_渲染器），仅 Layer 21 歌词使用
 *
 * 由 Engine 创建并注入 Mubu，Mubu 在创建 Layer 21 时传给 LayerText。
 * 字体仅加载 lyric.ttf、lyric-Pinyin.ttf，降低内存占用。
 */

#ifndef HSVJ_SHARED_LIBASS_HOLDER_H
#define HSVJ_SHARED_LIBASS_HOLDER_H

#include <mutex>
#include <string>
#include <vector>

extern "C" {
struct ass_library;
struct ass_renderer;
}

namespace hsvj {

class SharedLibassHolder {
public:
  SharedLibassHolder();
  ~SharedLibassHolder();

  SharedLibassHolder(const SharedLibassHolder &) = delete;
  SharedLibassHolder &operator=(const SharedLibassHolder &) = delete;

  struct ass_library *getLibrary() const { return library_; }
  struct ass_renderer *getRenderer() const { return renderer_; }

  /** 渲染前设置歌词渲染尺寸（有缓存，尺寸不变时跳过 ass_set_frame_size） */
  void setFrameSizeForLyrics(int width, int height);

  /** 确保应用字体已加载（lyric.ttf、lyric-Pinyin.ttf），只加载一次 */
  bool ensureAppFontsLoaded(const std::string &fontDir,
                           const std::string &appFontsDir  = "");

  /** 获取渲染互斥锁，ass_render_frame 前后必须加锁 */
  std::mutex &getRenderMutex() { return renderMutex_; }

  bool isInitialized() const {
    return library_ != nullptr && renderer_ != nullptr;
  }

private:
  struct ass_library *library_ = nullptr;
  struct ass_renderer *renderer_ = nullptr;
  std::mutex renderMutex_;
  bool fontsLoaded_ = false;
  std::vector<std::vector<char>> appFontDataBuffers_;
  int lastFrameWidth_ = 0;
  int lastFrameHeight_ = 0;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_SHARED_LIBASS_HOLDER_H
