// Runs the production PCM callback without opening an audio output.
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <memory>
#include <chrono>
#define private public
#include "audio/AudioPlayer.h"
#undef private
#include "utils/Logger.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

// Health tests model the legacy stream stuck in STARTING without callbacks.
// No real output is opened by this executable.
static aaudio_stream_state_t fakeState = AAUDIO_STREAM_STATE_STARTING;
static bool fakePauseStalls = false;
static int fakeStartRequests = 0;
extern "C" aaudio_stream_state_t AAudioStream_getState(AAudioStream*) {
  return fakeState;
}
extern "C" aaudio_result_t AAudioStream_requestPause(AAudioStream*) {
  fakeState = fakePauseStalls ? AAUDIO_STREAM_STATE_PAUSING : AAUDIO_STREAM_STATE_PAUSED;
  return AAUDIO_OK;
}
extern "C" aaudio_result_t AAudioStream_requestStart(AAudioStream*) {
  ++fakeStartRequests;
  fakeState = AAUDIO_STREAM_STATE_STARTING;
  return AAUDIO_OK;
}
extern "C" aaudio_result_t AAudioStream_waitForStateChange(
    AAudioStream*, aaudio_stream_state_t, aaudio_stream_state_t* next, int64_t) {
  *next = fakeState;
  return fakePauseStalls ? AAUDIO_ERROR_TIMEOUT : AAUDIO_OK;
}

namespace hsvj {
Logger::Level Logger::getLevel() { return Logger::ERROR_LEVEL; }
void Logger::log(Level, const char*, const char*, ...) {}
}

static void enqueue(hsvj::AudioPlayer& p, int frames, bool split) {
  while (frames > 0) {
    const int count = split ? std::min(frames, 37) : frames;
    auto* pcm = new int16_t[count * 2];
    std::fill(pcm, pcm + count * 2, 20000);
    p.audioQueue_.emplace(pcm, count, 0);
    frames -= count;
  }
}

static void ramp(int rate, int burst, bool split) {
  hsvj::AudioPlayer p;
  p.sampleRate_ = rate;
  p.channelCount_ = 2;
  p.bytesPerFrame_ = 4;
  p.playing_ = true;
  p.setVolume(1.0f);
  enqueue(p, rate * 2, split);
  p.fadeOut(8);
  std::vector<int16_t> samples(burst * 2);
  int previous = 20000, frameIndex = 0;
  // Includes many callbacks after zero; intentional fade-out must stay silent.
  for (int n = 0; n < 12; ++n) {
    p.onAudioReady(samples.data(), burst);
    for (int f = 0; f < burst; ++f, ++frameIndex) {
      const int value = samples[f * 2];
      assert(value == samples[f * 2 + 1]);
      assert(value >= 0 && value <= previous);
      assert(previous - value <= 60);
      const double expected = 20000 * std::max(0.0, 1.0 - frameIndex / (rate * .008));
      assert(std::abs(value - expected) <= 3);
      previous = value;
    }
  }
  assert(p.isFadeComplete() && previous == 0);
  p.fadeToVolume(1.0f, 30);
  previous = 0;
  for (int n = 0; n < 12; ++n) {
    p.onAudioReady(samples.data(), burst);
    for (int f = 0; f < burst; ++f) {
      assert(samples[f * 2] == samples[f * 2 + 1]);
      assert(samples[f * 2] >= previous && samples[f * 2] <= 20000);
      assert(samples[f * 2] - previous <= 17);
      previous = samples[f * 2];
    }
  }
  assert(previous == 20000);
  p.setVolume(0.0f);
  p.onAudioReady(samples.data(), burst);
  assert(std::all_of(samples.begin(), samples.end(), [](int16_t v) { return v == 0; }));
  p.flush();
  p.onAudioReady(samples.data(), burst);
  assert(std::all_of(samples.begin(), samples.end(), [](int16_t v) { return v == 0; }));
}

static void outputGain(int rate) {
  hsvj::AudioPlayer p;
  p.sampleRate_ = rate;
  p.channelCount_ = 2;
  p.bytesPerFrame_ = 4;
  p.playing_ = true;
  p.setVolume(1.0f);
  const int frames = rate / 10;
  std::vector<int16_t> samples(frames * 2);
  auto render = [&](int16_t left, int16_t right) {
    auto* pcm = new int16_t[frames * 2];
    for (int f = 0; f < frames; ++f) {
      pcm[f * 2] = left;
      pcm[f * 2 + 1] = right;
    }
    p.audioQueue_.emplace(pcm, frames, 0);
    p.onAudioReady(samples.data(), frames);
  };
  for (int db = 0; db <= 10; ++db) {
    p.setOutputGainDb(db);
    render(4000, -2000);
    const double expected = 4000 * std::pow(10.0, db / 20.0);
    assert(std::abs(samples[samples.size() - 2] - expected) <= 2);
    assert(std::abs(samples.back() + expected / 2) <= 2);
  }
  // Full-scale transients cannot wrap sign, clip the stereo balance, or mute.
  render(30000, -15000);
  for (int f = 0; f < frames; ++f) {
    assert(samples[f * 2] >= 29999);
    assert(std::abs(samples[f * 2] + 2 * samples[f * 2 + 1]) <= 2);
  }
  render(-32768, 16384);
  for (int f = 0; f < frames; ++f) {
    assert(samples[f * 2] < -30000);
    assert(std::abs(samples[f * 2] + 2 * samples[f * 2 + 1]) <= 2);
  }
  // Limiter recovers after peaks, without staying attenuated across buffers.
  for (int n = 0; n < 15; ++n) render(4000, -2000);
  assert(std::abs(samples[samples.size() - 2] - 4000 * std::pow(10.0, .5)) <= 3);
  p.setVolume(0.0f);
  render(30000, -30000);
  assert(std::all_of(samples.begin(), samples.end(), [](int16_t v) { return v == 0; }));
  p.setVolume(1.0f);
  p.setOutputGainDb(0);
  render(-32768, 32767);
  for (int f = 0; f < frames; ++f) {
    assert(samples[f * 2] == -32768 && samples[f * 2 + 1] == 32767);
  }
  p.setOutputGainDb(99);
  assert(p.outputGainDb_ == 10);
  p.setOutputGainDb(-1);
  assert(p.outputGainDb_ == 0);
}

static void outputHealth() {
  hsvj::AudioPlayer p;
  p.sampleRate_ = 44100;
  p.channelCount_ = 2;
  p.bytesPerFrame_ = 4;
  p.stream_ = reinterpret_cast<AAudioStream*>(1);
  p.playing_ = true;
  auto now = [] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  p.outputStartedAtNs_ = now();
  assert(!p.isOutputProgressing()); // Queued PCM is not output progress.
  assert(!p.needsReinit()); // Allow startup latency.
  enqueue(p, 2565, false);
  std::vector<int16_t> samples(2565 * 2);
  p.onAudioReady(samples.data(), 2565);
  assert(p.isOutputProgressing());
  assert(!p.needsReinit());
  p.outputStartedAtNs_ = now() - 2000000000LL;
  p.lastOutputCallbackNs_ = now() - 800000000LL;
  assert(!p.isOutputProgressing()); // Video hands off before recovery.
  assert(!p.needsReinit());
  p.lastOutputCallbackNs_ = now() - 1600000000LL;
  assert(p.needsReinit());
  assert(!p.isOutputProgressing());
  p.needsReinit_ = false;
  p.lastOutputCallbackNs_ = 0;
  p.playbackPaused_ = true;
  assert(!p.needsReinit()); // Intentional pause never triggers recovery.
  p.playbackPaused_ = false;
  p.outputStartedAtNs_ = 0;
  assert(!p.needsReinit()); // Stopped / preroll is not a broken output.
  p.outputStartedAtNs_ = now() - 2000000000LL;
  assert(p.needsReinit()); // Reproduces STARTING forever, no error callback.
  p.stream_ = nullptr;
  std::puts("PASS: real callback progress, stale-clock handoff, missing-callback recovery, startup/pause/stop exclusions.");
}

static void pauseRecovery() {
  hsvj::AudioPlayer p;
  p.stream_ = reinterpret_cast<AAudioStream*>(1);
  p.playing_ = true;
  fakeState = AAUDIO_STREAM_STATE_STARTED;
  fakePauseStalls = false;
  p.pause();
  assert(!p.needsReinit() && !p.playing_ && p.outputStartedAtNs_ == 0);
  fakeState = AAUDIO_STREAM_STATE_STARTING;
  assert(p.start());
  assert(p.outputStartedAtNs_ > 0 && p.playing_); // Retained STARTING is watched.
  p.outputStartedAtNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() - 2000000000LL;
  assert(!p.start() && p.needsReinit()); // Retry must not extend the deadline.
  p.needsReinit_ = false;
  fakeState = AAUDIO_STREAM_STATE_STARTED;
  fakePauseStalls = true;
  p.pause();
  assert(p.needsReinit() && !p.playing_);
  const int requests = fakeStartRequests;
  assert(!p.start() && fakeStartRequests == requests); // Never reuse half-paused output.
  p.stream_ = nullptr;
  fakePauseStalls = false;
  std::puts("PASS: pause completion, incomplete-pause recovery, retained STARTING watchdog and bounded retry deadline.");
}

int main() {
  outputHealth();
  pauseRecovery();
  for (int rate : {44100, 48000}) {
    for (int burst : {192, 256, 960, 2565}) {
      for (bool split : {false, true}) ramp(rate, burst, split);
    }
    outputGain(rate);
  }
  std::puts("PASS: 16 production-callback cases: frame-accurate fade, packet boundaries, stereo gain, intentional silence, fade-in and mute.");
  std::puts("PASS: VOD gain 0..10 dB at 44.1/48kHz, stereo peak limiting/release, mute, bit-exact bypass, invalid gain bounds.");
}
