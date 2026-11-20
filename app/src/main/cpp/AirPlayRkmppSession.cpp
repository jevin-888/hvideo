#include "AirPlayRkmppSession.h"

#include "decoder/H264NalUtils.h"
#include "decoder/frame/DecodedFrame.h"
#include "utils/Logger.h"

#include <chrono>
#include <utility>

namespace hsvj {

AirPlayRkmppSession::AirPlayRkmppSession() = default;

AirPlayRkmppSession::~AirPlayRkmppSession() {
  stop();
}

bool AirPlayRkmppSession::start(int layerId, VideoFrameCallback callback) {
  if (layerId < 0 || !callback || running_.load(std::memory_order_acquire)) {
    return false;
  }

  layerId_ = layerId;
  callback_ = std::move(callback);
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.clear();
    discontinuityPending_ = false;
    nextSequence_ = 0;
    droppedPackets_ = 0;
    syntheticPtsUs_ = 0;
    lastWidth_ = 0;
    lastHeight_ = 0;
    unsupportedCodecLogged_ = false;
  }
  running_.store(true, std::memory_order_release);
  decoderThread_ = std::thread(&AirPlayRkmppSession::decoderLoop, this);
  LOG_INFO("[AirPlayRkmpp] session started layer=%d", layerId_);
  return true;
}

void AirPlayRkmppSession::stop() {
  const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.clear();
    discontinuityPending_ = false;
  }
  queueCv_.notify_all();
  if (decoderThread_.joinable()) {
    decoderThread_.join();
  }
  callback_ = nullptr;
  if (wasRunning) {
    LOG_INFO("[AirPlayRkmpp] session stopped layer=%d dropped=%lld", layerId_,
             static_cast<long long>(droppedPackets_));
  }
}

bool AirPlayRkmppSession::enqueue(std::vector<uint8_t>&& data, int nalType,
                                  int64_t ptsUs, int width, int height,
                                  int codec) {
  if (!running_.load(std::memory_order_acquire) || data.empty()) {
    return false;
  }
  if (codec != 264) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (!unsupportedCodecLogged_) {
      unsupportedCodecLogged_ = true;
      LOG_ERROR("[AirPlayRkmpp] unsupported codec=%d; only H.264/264 is enabled",
                codec);
    }
    return false;
  }

  std::vector<uint8_t> normalized = normalizeH264AnnexB(data.data(), data.size());
  if (normalized.empty()) {
    return false;
  }

  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (!running_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (queue_.size() >= kQueueLimit) {
      droppedPackets_ += static_cast<int64_t>(queue_.size());
      queue_.clear();
      discontinuityPending_ = true;
      LOG_WARN("[AirPlayRkmpp] queue overflow; resetting at next IDR dropped=%lld",
               static_cast<long long>(droppedPackets_));
    }
    if (width > 0 && height > 0) {
      lastWidth_ = width;
      lastHeight_ = height;
    } else if (lastWidth_ > 0 && lastHeight_ > 0) {
      width = lastWidth_;
      height = lastHeight_;
    }
    if (ptsUs <= 0) {
      syntheticPtsUs_ += 33333;
      ptsUs = syntheticPtsUs_;
    } else {
      syntheticPtsUs_ = ptsUs;
    }
    Packet packet;
    packet.data = std::move(normalized);
    packet.nalType = nalType;
    packet.ptsUs = ptsUs;
    packet.width = width;
    packet.height = height;
    packet.codec = codec;
    packet.sequence = ++nextSequence_;
    queue_.push_back(std::move(packet));
    notify = true;
  }
  if (notify) {
    queueCv_.notify_one();
  }
  return true;
}

void AirPlayRkmppSession::decoderLoop() {
  std::unique_ptr<H264RkmppDecoder> decoder;
  int decoderWidth = 0;
  int decoderHeight = 0;
  int decoderCodec = 0;
  bool waitingForIdr = true;
  bool firstFrameDelivered = false;
  std::vector<uint8_t> cachedSps;
  std::vector<uint8_t> cachedPps;

  while (running_.load(std::memory_order_acquire)) {
    Packet packet;
    bool discontinuity = false;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait(lock, [&]() {
        return !running_.load(std::memory_order_relaxed) ||
               discontinuityPending_ || !queue_.empty();
      });
      if (!running_.load(std::memory_order_relaxed)) {
        break;
      }
      discontinuity = discontinuityPending_;
      discontinuityPending_ = false;
      if (!queue_.empty()) {
        packet = std::move(queue_.front());
        queue_.pop_front();
      }
    }

    if (discontinuity) {
      decoder.reset();
      decoderWidth = 0;
      decoderHeight = 0;
      decoderCodec = 0;
      waitingForIdr = true;
    }
    if (packet.data.empty()) {
      continue;
    }

    std::vector<uint8_t> parameterSet;
    if (extractH264NalUnit(packet.data.data(), packet.data.size(), 7,
                           parameterSet)) {
      cachedSps = parameterSet;
    }
    if (extractH264NalUnit(packet.data.data(), packet.data.size(), 8,
                           parameterSet)) {
      cachedPps = parameterSet;
    }

    if (packet.width <= 0 || packet.height <= 0) {
      // Some SDK callbacks report SPS/PPS before the first valid geometry.
      // Keep the parameter sets and defer decoder creation until dimensions arrive.
      continue;
    }

    const bool geometryChanged = packet.width != decoderWidth ||
                                 packet.height != decoderHeight ||
                                 packet.codec != decoderCodec;
    if (geometryChanged) {
      decoder.reset();
      decoderWidth = packet.width;
      decoderHeight = packet.height;
      decoderCodec = packet.codec;
      waitingForIdr = true;
      firstFrameDelivered = false;
      LOG_INFO("[AirPlayRkmpp] stream geometry=%dx%d codec=%d",
               decoderWidth, decoderHeight, decoderCodec);
    }
    const bool isIdr = packet.nalType == 5 ||
                       containsH264Idr(packet.data.data(), packet.data.size());
    if (waitingForIdr && !isIdr) {
      continue;
    }

    if (!decoder) {
      decoder = std::make_unique<H264RkmppDecoder>();
      if (!decoder->open(decoderWidth, decoderHeight)) {
        LOG_ERROR("[AirPlayRkmpp] failed to open h264_rkmpp for %dx%d",
                  decoderWidth, decoderHeight);
        decoder.reset();
        waitingForIdr = true;
        continue;
      }
    }

    std::vector<uint8_t> accessUnit;
    const uint8_t* decodeData = packet.data.data();
    size_t decodeSize = packet.data.size();
    if (isIdr) {
      const bool packetHasSps = containsH264NalType(
          packet.data.data(), packet.data.size(), 7);
      const bool packetHasPps = containsH264NalType(
          packet.data.data(), packet.data.size(), 8);
      if ((!packetHasSps && !cachedSps.empty()) ||
          (!packetHasPps && !cachedPps.empty())) {
        accessUnit.reserve(cachedSps.size() + cachedPps.size() +
                           packet.data.size());
        if (!packetHasSps) {
          accessUnit.insert(accessUnit.end(), cachedSps.begin(), cachedSps.end());
        }
        if (!packetHasPps) {
          accessUnit.insert(accessUnit.end(), cachedPps.begin(), cachedPps.end());
        }
        accessUnit.insert(accessUnit.end(), packet.data.begin(), packet.data.end());
        decodeData = accessUnit.data();
        decodeSize = accessUnit.size();
      }
      waitingForIdr = false;
    }

    const auto decodeStart = std::chrono::steady_clock::now();
    auto frames = decoder->pushPacket(decodeData, static_cast<int>(decodeSize),
                                      packet.ptsUs, isIdr);
    const auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - decodeStart).count();
    if (decodeMs >= 33) {
      LOG_WARN("[AirPlayRkmpp] slow decode seq=%lld bytes=%zu frames=%zu cost=%lldms",
               static_cast<long long>(packet.sequence), decodeSize, frames.size(),
               static_cast<long long>(decodeMs));
    }

    for (auto& frame : frames) {
      if (!running_.load(std::memory_order_acquire) || !callback_) {
        if (frame.frame) {
          frame.frame->release();
          frame.frame = nullptr;
        }
        continue;
      }
      if (!firstFrameDelivered) {
        firstFrameDelivered = true;
        LOG_INFO("[AirPlayRkmpp] first output frame original=%dx%d",
                 frame.originalWidth, frame.originalHeight);
      }
      callback_(layerId_, frame);
    }
  }
}

} // namespace hsvj
