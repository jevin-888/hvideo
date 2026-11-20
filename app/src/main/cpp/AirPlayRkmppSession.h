#ifndef HSVJ_AIRPLAY_RKMPP_SESSION_H
#define HSVJ_AIRPLAY_RKMPP_SESSION_H

#include "decoder/H264RkmppDecoder.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hsvj {

class AirPlayRkmppSession {
public:
  using VideoFrameCallback = std::function<void(int, H264RkmppFrame&)>;

  AirPlayRkmppSession();
  ~AirPlayRkmppSession();

  bool start(int layerId, VideoFrameCallback callback);
  void stop();

  bool enqueue(std::vector<uint8_t>&& data, int nalType, int64_t ptsUs,
               int width, int height, int codec);
  bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
  struct Packet {
    std::vector<uint8_t> data;
    int nalType = 0;
    int64_t ptsUs = 0;
    int width = 0;
    int height = 0;
    int codec = 0;
    int64_t sequence = 0;
  };

  void decoderLoop();

  static constexpr size_t kQueueLimit = 30;

  int layerId_ = 0;
  VideoFrameCallback callback_;
  std::atomic<bool> running_{false};
  std::thread decoderThread_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<Packet> queue_;
  bool discontinuityPending_ = false;
  int64_t nextSequence_ = 0;
  int64_t droppedPackets_ = 0;
  int64_t syntheticPtsUs_ = 0;
  int lastWidth_ = 0;
  int lastHeight_ = 0;
  bool unsupportedCodecLogged_ = false;
};

} // namespace hsvj

#endif // HSVJ_AIRPLAY_RKMPP_SESSION_H
