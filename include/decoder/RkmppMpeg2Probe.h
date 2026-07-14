#ifndef HSVJ_RKMPP_MPEG2_PROBE_H
#define HSVJ_RKMPP_MPEG2_PROBE_H

#include <string>
#include <cstdint>

namespace hsvj {

struct RkmppMpeg2ProbeResult {
  bool opened = false;
  bool initialized = false;
  bool infoChanged = false;
  bool gotFrame = false;
  int packetsSent = 0;
  int framesReceived = 0;
  int width = 0;
  int height = 0;
  int hStride = 0;
  int vStride = 0;
  int dmaBufFd = -1;
  std::string message;
};

class RkmppMpeg2Probe {
public:
  static RkmppMpeg2ProbeResult probeFile(const std::string &path, int maxPackets = 120, int timeoutMs = 5000);
};

struct RkmppMpeg2DirectFrame {
  void *mppFrame = nullptr;
  int dmaBufFd = -1;
  int width = 0;
  int height = 0;
  int hStride = 0;
  int vStride = 0;
  uint32_t v4l2Fourcc = 0;
  double pts = 0.0;
  bool keyFrame = false;
};

class RkmppMpeg2DirectDecoder {
public:
  RkmppMpeg2DirectDecoder();
  ~RkmppMpeg2DirectDecoder();
  RkmppMpeg2DirectDecoder(const RkmppMpeg2DirectDecoder&) = delete;
  RkmppMpeg2DirectDecoder& operator=(const RkmppMpeg2DirectDecoder&) = delete;

  bool initializeStream(int width, int height, double frameRate, double duration);
  bool sendPacket(const uint8_t *data, int size, double pts, bool keyFrame);
  bool receiveFrame(RkmppMpeg2DirectFrame &out);
  void close();
  bool isOpen() const { return opened_; }
  int getWidth() const { return width_; }
  int getHeight() const { return height_; }
  double getFrameRate() const { return frameRate_; }
  double getDuration() const { return duration_; }

private:
  bool drainFrame(RkmppMpeg2DirectFrame &out);
  double pendingPts_ = 0.0;
  bool pendingKeyFrame_ = false;
  void *mppCtx_ = nullptr;
  void *mppApi_ = nullptr;
  void *frameGroup_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  double frameRate_ = 0.0;
  double duration_ = 0.0;
  bool opened_ = false;
  int64_t frameNumber_ = 0;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_RKMPP_MPEG2_PROBE_H
