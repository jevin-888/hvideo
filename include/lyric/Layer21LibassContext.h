/**
 * @file Layer21LibassContext.h（文件名）
 * @brief Layer 21 内部独占的 libass 单实例上下文（1 ASS_Library + 1 ASS_渲染器）
 *
 * 仅 Layer 21（歌词）内部使用此上下文；不会对其他图层暴露或共享。
 * 调用 ass_render_frame 前必须加 getRenderMutex()，用完后 ass_frame_unref。
 */

#ifndef HSVJ_LAYER21_LIBASS_CONTEXT_H
#define HSVJ_LAYER21_LIBASS_CONTEXT_H

#include <mutex>
#include <string>
#include <vector>

extern "C" {
struct ass_library;
struct ass_renderer;
}

namespace hsvj {

/**
 * @brief 持有一套独占的 ASS_Library 和 ASS_渲染器，仅供单个 Layer 21 使用
 */
class Layer21LibassContext {
public:
  /** 全局单例，避免多套 libass 上下文导致 200MB+ 重复内存 */
  static Layer21LibassContext &getInstance();

  Layer21LibassContext();
  ~Layer21LibassContext();

  Layer21LibassContext(const Layer21LibassContext &) = delete;
  Layer21LibassContext &operator=(const Layer21LibassContext &) = delete;

  struct ass_library *getLibrary() const { return library_; }
  struct ass_renderer *getRenderer() const { return renderer_; }

  /** 渲染前设置当前 Layer 21 的歌词渲染尺寸 */
  void setFrameSize(int width, int height);

  /** 确保应用字体已加载（lyric.ttf 等），只加载一次 */
  bool ensureAppFontsLoaded(const std::string &fontDir,
                            const std::string &appFontsDir  = "");

  /** 获取渲染互斥锁，ass_render_frame 前后必须加锁保证串行 */
  std::mutex &getRenderMutex() { return renderMutex_; }

  bool isInitialized() const {
    return library_ != nullptr && renderer_ != nullptr;
  }

private:
  struct ass_library *library_ = nullptr;
  struct ass_renderer *renderer_ = nullptr;
  std::mutex renderMutex_;
  bool fontsLoaded_ = false;
  std::vector<std::vector<char>> fontDataBuffers_;
  int lastFrameWidth_ = 0;
  int lastFrameHeight_ = 0;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER21_LIBASS_CONTEXT_H

