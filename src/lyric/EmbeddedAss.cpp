#include "lyric/EmbeddedAss.h"
#include "utils/Logger.h"
#include "utils/EncryptedMedia.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace hsvj {
namespace {
struct Extraction {
  const std::atomic<bool>& cancelled;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  AVFormatContext* input = nullptr;
  AVFormatContext* output = nullptr;
  AVPacket* packet = nullptr;
  bool muxStarted = false;

  void madeProgress() {
    // This is an inactivity limit, not a limit on the whole movie scan.
    // HTTP Matroska reads can take longer than 30 seconds even while packets
    // keep arriving. Keep cancellation and the per-read network timeout.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  }

  static int interrupted(void* opaque) {
    const auto& self = *static_cast<Extraction*>(opaque);
    return self.cancelled.load() || std::chrono::steady_clock::now() >= self.deadline;
  }

  ~Extraction() {
    av_packet_free(&packet);
    avformat_close_input(&input);
    if (output) {
      if (muxStarted) av_write_trailer(output);
      if (output->pb) {
        uint8_t* buffer = nullptr;
        avio_close_dyn_buf(output->pb, &buffer);
        av_free(buffer);
      }
      avformat_free_context(output);
    }
  }
};
}

std::string readEmbeddedAss(const std::string& videoPath, const std::atomic<bool>& cancelled,
                           const std::function<void(std::string)>& onProgress) {
  std::string path = videoPath;
  if (path.find("://") != std::string::npos) {
    const size_t query = path.find_first_of("?#");
    if (query != std::string::npos) path.resize(query);
  }
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (path.size() < 4 || cancelled.load()) return {};
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  const std::string extension = path.substr(dot);
  if (extension != ".mkv" && extension != ".hvideo" && extension != ".mp4" && extension != ".m4v" && extension != ".mov") return {};

  Extraction task{cancelled};
  task.input = avformat_alloc_context();
  if (!task.input) return {};
  task.input->interrupt_callback = {Extraction::interrupted, &task};
  AVDictionary* options = nullptr;
  av_dict_set(&options, "rw_timeout", "5000000", 0);
  int rc = encrypted_media::openInput(&task.input, videoPath.c_str(), nullptr, &options);
  av_dict_free(&options);
  if (rc < 0) return {};
  task.madeProgress();

  // The encryption tool retains the complete ASS document in MP4 mdta.
  // Read the faststart header for both local and HTTP media without scanning A/V.
  if (extension != ".mkv") {
    const AVDictionaryEntry* lyrics = av_dict_get(task.input->metadata, "hvideo_ass_v1", nullptr, 0);
    if (!lyrics || !lyrics->value || Extraction::interrupted(&task)) return {};
    const size_t length = strnlen(lyrics->value, 8 * 1024 * 1024 + 1);
    if (length == 0 || length > 8 * 1024 * 1024) return {};
    std::string ass(lyrics->value, length);
    if (ass.find("[Events]") == std::string::npos || ass.find("Dialogue:") == std::string::npos) return {};
    if (onProgress) onProgress(ass);
    return Extraction::interrupted(&task) ? std::string{} : ass;
  }

  // Matroska exposes subtitle tracks and CodecPrivate when opening its header.
  int selected = -1;
  for (unsigned i = 0; i < task.input->nb_streams; ++i) {
    AVStream* stream = task.input->streams[i];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE ||
        stream->codecpar->codec_id != AV_CODEC_ID_ASS) continue;
    if (selected < 0) selected = static_cast<int>(i);
    if (stream->disposition & AV_DISPOSITION_DEFAULT) {
      selected = static_cast<int>(i);
      break;
    }
  }
  if (selected < 0) return {};
  AVStream* source = task.input->streams[selected];
  if (source->codecpar->extradata_size <= 0 || source->codecpar->extradata_size > 1024 * 1024) return {};

  if (avformat_alloc_output_context2(&task.output, nullptr, "ass", nullptr) < 0 || !task.output) return {};
  AVStream* target = avformat_new_stream(task.output, nullptr);
  if (!target || avcodec_parameters_copy(target->codecpar, source->codecpar) < 0) return {};
  target->codecpar->codec_tag = 0;
  target->time_base = source->time_base;
  if (avio_open_dyn_buf(&task.output->pb) < 0 || avformat_write_header(task.output, nullptr) < 0) return {};
  task.muxStarted = true;
  task.packet = av_packet_alloc();
  if (!task.packet) return {};

  size_t bytes = 0;
  size_t events = 0;
  size_t publishedEvents = 0;
  int publishedSize = 0;
  const auto started = std::chrono::steady_clock::now();
  auto nextPublish = started;
  while (!Extraction::interrupted(&task) && (rc = av_read_frame(task.input, task.packet)) >= 0) {
    task.madeProgress();
    if (task.packet->stream_index == selected && task.packet->size > 0) {
      // Subtitle packets are allowed to carry an unknown/zero duration in
      // Matroska; the ASS event itself contains the timing. Rejecting those
      // packets aborts extraction and leaves the player with no lyrics.
      if (task.packet->pts == AV_NOPTS_VALUE) return {};
      bytes += static_cast<size_t>(task.packet->size);
      if (bytes > 8 * 1024 * 1024 || ++events > 100000) return {};
      av_packet_rescale_ts(task.packet, source->time_base, target->time_base);
      task.packet->stream_index = 0;
      task.packet->pos = -1;
      if (av_write_frame(task.output, task.packet) < 0) return {};
    }
    av_packet_unref(task.packet);
    const auto now = std::chrono::steady_clock::now();
    if (onProgress && events > publishedEvents && now >= nextPublish &&
        !Extraction::interrupted(&task)) {
      // Only expose complete lines already emitted by the ASS muxer. Keep its
      // ReadOrder buffering intact so later snapshots retain the same order.
      avio_flush(task.output->pb);
      uint8_t* buffer = nullptr;
      const int size = avio_get_dyn_buf(task.output->pb, &buffer);
      if (size > publishedSize && buffer) {
        onProgress(std::string(reinterpret_cast<const char*>(buffer), static_cast<size_t>(size)));
        if (publishedSize == 0) {
          LOG_INFO("[Lyric] Embedded ASS first snapshot: events=%zu elapsedMs=%lld",
                   events, static_cast<long long>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()));
        }
        publishedSize = size;
      }
      publishedEvents = events;
      nextPublish = now + std::chrono::seconds(1);
    }
  }
  // A failed read must not be reported as a completed timeline. The caller
  // tries directory fallback without discarding an already valid snapshot.
  if (Extraction::interrupted(&task) || rc != AVERROR_EOF || events == 0) {
    LOG_WARN("[Lyric] Embedded ASS incomplete: events=%zu readResult=%d cancelled=%d idleTimeout=%d",
             events, rc, cancelled.load(),
             std::chrono::steady_clock::now() >= task.deadline);
    return {};
  }
  if (av_write_trailer(task.output) < 0) return {};
  task.muxStarted = false;
  uint8_t* buffer = nullptr;
  const int size = avio_close_dyn_buf(task.output->pb, &buffer);
  task.output->pb = nullptr;
  std::string result;
  if (size > 0) result.assign(reinterpret_cast<const char*>(buffer), static_cast<size_t>(size));
  av_free(buffer);
  LOG_INFO("[Lyric] Embedded ASS extracted: stream=%d events=%zu bytes=%d", selected, events, size);
  return result;
}
}
