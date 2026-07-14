#ifndef HSVJ_USB_H264_RKMPP_DECODER_H
#define HSVJ_USB_H264_RKMPP_DECODER_H

#include <cstdint>
#include <mutex>
#include <vector>

namespace hsvj {

class DecodedFrame;

struct UsbH264RkmppFrame {
  DecodedFrame* frame = nullptr;
  int originalWidth = 0;
  int originalHeight = 0;
  int cropOffsetY = 0;
};

class UsbH264RkmppDecoder {
public:
  UsbH264RkmppDecoder();
  ~UsbH264RkmppDecoder();

  bool open(int width, int height);
  void close();
  void flush();

  std::vector<UsbH264RkmppFrame> pushPacket(const uint8_t* data, int size,
                                            int64_t ptsUs, bool keyFrame);

  int width() const { return visibleWidth_; }
  int height() const { return visibleHeight_; }

private:
  bool sendPacketLocked(const uint8_t* data, int size, int64_t ptsUs,
                        bool keyFrame, std::vector<UsbH264RkmppFrame>& out);
  void drainFrames(std::vector<UsbH264RkmppFrame>& out);
  void closeLocked();

  mutable std::mutex mutex_;
  void* codecCtx_ = nullptr;
  int visibleWidth_ = 0;
  int visibleHeight_ = 0;
  int64_t frameNumber_ = 0;
};

} // namespace hsvj

#endif // HSVJ_USB_H264_RKMPP_DECODER_H
