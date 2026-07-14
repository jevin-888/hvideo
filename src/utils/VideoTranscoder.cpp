/**
 * @file VideoTranscoder.cpp
 * @brief 视频重新编码实现（Rockchip MPP H.264）
 *
 * Android 平台只使用 Rockchip MPP H.264 硬件编码。
 */

#include "utils/VideoTranscoder.h"
#include "utils/Logger.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}
#endif

namespace fs = std::filesystem;

namespace hsvj {

namespace {

static constexpr int OPTIMIZED_MARKER_VERSION = 11;
static constexpr const char* TRANSCODE_ENCODER_NAME = "h264_rkmpp";
static constexpr int PR_COMPATIBLE_MP4_TIMESCALE = 25000;

static std::string getOptimizedMarkerPath(const std::string& inputPath) {
    return inputPath + ".optimized";
}

static int64_t getFileSizeForMarker(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return -1;
    return static_cast<int64_t>(size);
}

static int64_t getFileTimeForMarker(const std::string& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(time.time_since_epoch().count());
}

static std::unordered_map<std::string, std::string> readOptimizedMarker(const std::string& markerPath) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream marker(markerPath);
    std::string line;
    while (std::getline(marker, line)) {
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        values[line.substr(0, split)] = line.substr(split + 1);
    }
    return values;
}

static bool markerInt64Equals(const std::unordered_map<std::string, std::string>& values,
                              const std::string& key,
                              int64_t expected) {
    auto it = values.find(key);
    if (it == values.end()) return false;
    try {
        return std::stoll(it->second) == expected;
    } catch (...) {
        return false;
    }
}

static bool markerIntEquals(const std::unordered_map<std::string, std::string>& values,
                            const std::string& key,
                            int expected) {
    auto it = values.find(key);
    if (it == values.end()) return false;
    try {
        return std::stoi(it->second) == expected;
    } catch (...) {
        return false;
    }
}

static bool markerStringEquals(const std::unordered_map<std::string, std::string>& values,
                               const std::string& key,
                               const std::string& expected) {
    auto it = values.find(key);
    return it != values.end() && it->second == expected;
}

static bool writeOptimizedMarker(const std::string& inputPath,
                                 const VideoTranscoder::TranscodeOptions& opts) {
    const std::string markerPath = getOptimizedMarkerPath(inputPath);
    const std::string tmpPath = markerPath + ".tmp";

    std::ofstream marker(tmpPath, std::ios::trunc);
    if (!marker.is_open()) {
        LOG_WARN("[VideoTranscoder] Failed to write optimized marker: %s", inputPath.c_str());
        return false;
    }

    marker << "marker_version=" << OPTIMIZED_MARKER_VERSION << "\n";
    marker << "optimized=1\n";
    marker << "file_size=" << getFileSizeForMarker(inputPath) << "\n";
    marker << "file_mtime=" << getFileTimeForMarker(inputPath) << "\n";
    marker << "copy_audio=" << (opts.copyAudio ? 1 : 0) << "\n";
    marker << "preserve_resolution=1\n";
    marker << "encoder=" << TRANSCODE_ENCODER_NAME << "\n";
    marker.close();

    if (!marker.good()) {
        LOG_WARN("[VideoTranscoder] Failed to flush optimized marker: %s", inputPath.c_str());
        std::error_code removeEc;
        fs::remove(tmpPath, removeEc);
        return false;
    }

    std::error_code ec;
    fs::rename(tmpPath, markerPath, ec);
    if (ec) {
        fs::remove(markerPath, ec);
        ec.clear();
        fs::rename(tmpPath, markerPath, ec);
    }
    if (ec) {
        LOG_WARN("[VideoTranscoder] Failed to publish optimized marker (ec=%d): %s",
                 ec.value(), inputPath.c_str());
        std::error_code removeEc;
        fs::remove(tmpPath, removeEc);
        return false;
    }

    return true;
}

} // namespace

bool VideoTranscoder::isOptimizedForOptions(const std::string& inputPath,
                                            const TranscodeOptions& opts) {
    std::error_code ec;
    if (inputPath.empty() || !fs::exists(inputPath, ec) || !fs::is_regular_file(inputPath, ec)) {
        return false;
    }

    const std::string markerPath = getOptimizedMarkerPath(inputPath);
    if (!fs::exists(markerPath, ec)) {
        return false;
    }

    const auto values = readOptimizedMarker(markerPath);
    if (!markerIntEquals(values, "marker_version", OPTIMIZED_MARKER_VERSION) ||
        !markerIntEquals(values, "optimized", 1) ||
        !markerIntEquals(values, "preserve_resolution", 1) ||
        !markerStringEquals(values, "encoder", TRANSCODE_ENCODER_NAME)) {
        return false;
    }

    const bool sameFile =
        markerInt64Equals(values, "file_size", getFileSizeForMarker(inputPath)) &&
        markerInt64Equals(values, "file_mtime", getFileTimeForMarker(inputPath)) &&
        markerIntEquals(values, "copy_audio", opts.copyAudio ? 1 : 0);
    return sameFile;
}

#ifdef __ANDROID__

static std::string simpleHashPath(const std::string& path) {
    std::hash<std::string> hasher;
    size_t h = hasher(path);
    std::ostringstream oss;
    oss << std::hex << h;
    return oss.str().substr(0, 12);
}

std::string VideoTranscoder::getTranscodeOutputPath(const std::string& inputPath) {
    fs::path p(inputPath);
    std::string parent = p.parent_path().string();
    std::string stem = p.stem().string();
    std::string hash = simpleHashPath(inputPath);
    std::string cacheDir = parent + "/.cache/transcoded";
    return cacheDir + "/" + stem + "_" + hash + ".mp4";
}

static const AVCodec* findH264Encoder() {
    return avcodec_find_encoder_by_name(TRANSCODE_ENCODER_NAME);
}

class H264BitReader {
public:
    explicit H264BitReader(const std::vector<uint8_t>& data) : data_(data) {}

    bool readBit(uint32_t& out) {
        if (bitPos_ >= data_.size() * 8) return false;
        out = (data_[bitPos_ / 8] >> (7 - (bitPos_ % 8))) & 1;
        ++bitPos_;
        return true;
    }

    bool readBits(int count, uint32_t& out) {
        out = 0;
        for (int i = 0; i < count; ++i) {
            uint32_t bit = 0;
            if (!readBit(bit)) return false;
            out = (out << 1) | bit;
        }
        return true;
    }

    bool readUe(uint32_t& out) {
        int zeros = 0;
        uint32_t bit = 0;
        while (true) {
            if (!readBit(bit)) return false;
            if (bit) break;
            if (++zeros > 31) return false;
        }
        uint32_t suffix = 0;
        if (zeros > 0 && !readBits(zeros, suffix)) return false;
        out = ((1u << zeros) - 1u) + suffix;
        return true;
    }

    bool readSe(int32_t& out) {
        uint32_t codeNum = 0;
        if (!readUe(codeNum)) return false;
        out = (codeNum & 1) ? static_cast<int32_t>((codeNum + 1) / 2)
                            : -static_cast<int32_t>(codeNum / 2);
        return true;
    }

    size_t bitPos() const { return bitPos_; }

private:
    const std::vector<uint8_t>& data_;
    size_t bitPos_ = 0;
};

static bool isHighProfileWithExtraSpsFields(uint32_t profileIdc) {
    switch (profileIdc) {
        case 100: case 110: case 122: case 244: case 44: case 83:
        case 86: case 118: case 128: case 138: case 139: case 134: case 135:
            return true;
        default:
            return false;
    }
}

static bool setSpsMappedBit(uint8_t* nal,
                            const std::vector<std::pair<int, int>>& bitMap,
                            size_t rbspBitPos,
                            bool value) {
    if (!nal || rbspBitPos >= bitMap.size()) return false;
    const auto [byteIndex, bitIndex] = bitMap[rbspBitPos];
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - bitIndex));
    const bool oldValue = (nal[byteIndex] & mask) != 0;
    if (oldValue == value) return false;
    if (value) {
        nal[byteIndex] |= mask;
    } else {
        nal[byteIndex] &= static_cast<uint8_t>(~mask);
    }
    return true;
}

static bool patchSpsForPrCompatiblePlayback(uint8_t* nal, int size) {
    if (!nal || size < 2 || (nal[0] & 0x1f) != 7) return false;

    std::vector<uint8_t> rbsp;
    std::vector<std::pair<int, int>> bitMap;
    rbsp.reserve(size - 1);
    int zeros = 0;
    for (int i = 1; i < size; ++i) {
        if (zeros >= 2 && nal[i] == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(nal[i]);
        for (int bit = 0; bit < 8; ++bit) {
            bitMap.emplace_back(i, bit);
        }
        zeros = (nal[i] == 0) ? zeros + 1 : 0;
    }

    H264BitReader br(rbsp);
    uint32_t profileIdc = 0;
    uint32_t tmp = 0;
    if (!br.readBits(8, profileIdc) || !br.readBits(8, tmp) || !br.readBits(8, tmp)) return false;
    if (!br.readUe(tmp)) return false;  // seq_parameter_set_id

    if (isHighProfileWithExtraSpsFields(profileIdc)) {
        uint32_t chromaFormatIdc = 0;
        if (!br.readUe(chromaFormatIdc)) return false;
        if (chromaFormatIdc == 3 && !br.readBits(1, tmp)) return false;
        if (!br.readUe(tmp) || !br.readUe(tmp) || !br.readBits(1, tmp)) return false;
        uint32_t scalingMatrixPresent = 0;
        if (!br.readBits(1, scalingMatrixPresent)) return false;
        if (scalingMatrixPresent) {
            const int count = chromaFormatIdc == 3 ? 12 : 8;
            for (int i = 0; i < count; ++i) {
                uint32_t listPresent = 0;
                if (!br.readBits(1, listPresent)) return false;
                if (!listPresent) continue;
                const int entries = i < 6 ? 16 : 64;
                int lastScale = 8;
                int nextScale = 8;
                for (int j = 0; j < entries; ++j) {
                    if (nextScale != 0) {
                        int32_t deltaScale = 0;
                        if (!br.readSe(deltaScale)) return false;
                        nextScale = (lastScale + deltaScale + 256) % 256;
                    }
                    lastScale = nextScale == 0 ? lastScale : nextScale;
                }
            }
        }
    }

    if (!br.readUe(tmp)) return false;  // log2_max_frame_num_minus4
    uint32_t picOrderCntType = 0;
    if (!br.readUe(picOrderCntType)) return false;
    if (picOrderCntType == 0) {
        if (!br.readUe(tmp)) return false;
    } else if (picOrderCntType == 1) {
        if (!br.readBits(1, tmp)) return false;
        int32_t signedTmp = 0;
        if (!br.readSe(signedTmp) || !br.readSe(signedTmp)) return false;
        uint32_t cycle = 0;
        if (!br.readUe(cycle)) return false;
        for (uint32_t i = 0; i < cycle; ++i) {
            if (!br.readSe(signedTmp)) return false;
        }
    }
    if (!br.readUe(tmp) || !br.readBits(1, tmp) || !br.readUe(tmp) || !br.readUe(tmp)) return false;

    uint32_t frameMbsOnly = 0;
    if (!br.readBits(1, frameMbsOnly)) return false;
    if (!frameMbsOnly && !br.readBits(1, tmp)) return false;

    const size_t directBitPos = br.bitPos();
    uint32_t directFlag = 0;
    if (!br.readBits(1, directFlag)) return false;
    bool patched = setSpsMappedBit(nal, bitMap, directBitPos, true);

    uint32_t frameCropping = 0;
    if (!br.readBits(1, frameCropping)) return patched;
    if (frameCropping) {
        if (!br.readUe(tmp) || !br.readUe(tmp) || !br.readUe(tmp) || !br.readUe(tmp)) return patched;
    }

    uint32_t vuiPresent = 0;
    if (!br.readBits(1, vuiPresent) || !vuiPresent) return patched;

    uint32_t flag = 0;
    if (!br.readBits(1, flag)) return patched;  // aspect_ratio_info_present_flag
    if (flag) {
        uint32_t aspectRatioIdc = 0;
        if (!br.readBits(8, aspectRatioIdc)) return patched;
        if (aspectRatioIdc == 255 && (!br.readBits(16, tmp) || !br.readBits(16, tmp))) return patched;
    }

    if (!br.readBits(1, flag)) return patched;  // overscan_info_present_flag
    if (flag && !br.readBits(1, tmp)) return patched;

    if (!br.readBits(1, flag) || !flag) return patched;  // video_signal_type_present_flag
    for (int i = 0; i < 3; ++i) {
        patched = setSpsMappedBit(nal, bitMap, br.bitPos() + i, false) || patched;
    }

    return patched;
}

static bool patchH264SpsForPrCompatiblePlayback(uint8_t* data, int size) {
    if (!data || size <= 0) return false;
    bool patched = false;

    if (size > 6 && data[0] == 1) {
        int offset = 5;
        const int spsCount = data[offset++] & 0x1f;
        for (int i = 0; i < spsCount && offset + 2 <= size; ++i) {
            const int spsSize = (data[offset] << 8) | data[offset + 1];
            offset += 2;
            if (spsSize <= 0 || offset + spsSize > size) return patched;
            patched = patchSpsForPrCompatiblePlayback(data + offset, spsSize) || patched;
            offset += spsSize;
        }
        return patched;
    }

    auto startCodeLengthAt = [&](int pos) -> int {
        if (pos + 3 <= size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) return 3;
        if (pos + 4 <= size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) return 4;
        return 0;
    };

    int pos = 0;
    while (pos < size) {
        int sc = startCodeLengthAt(pos);
        if (!sc) {
            ++pos;
            continue;
        }
        const int nalStart = pos + sc;
        int next = nalStart;
        while (next < size && !startCodeLengthAt(next)) ++next;
        patched = patchSpsForPrCompatiblePlayback(data + nalStart, next - nalStart) || patched;
        pos = next;
    }

    if (!patched && size > 1 && (data[0] & 0x1f) == 7) {
        patched = patchSpsForPrCompatiblePlayback(data, size);
    }
    return patched;
}

static AVRational getStreamSampleAspectRatio(const AVStream* stream) {
    if (!stream || !stream->codecpar) {
        return AVRational{0, 1};
    }

    AVRational sar = av_guess_sample_aspect_ratio(nullptr, const_cast<AVStream*>(stream), nullptr);
    if (sar.num <= 0 || sar.den <= 0) {
        sar = stream->codecpar->sample_aspect_ratio.num > 0 &&
              stream->codecpar->sample_aspect_ratio.den > 0
            ? stream->codecpar->sample_aspect_ratio
            : stream->sample_aspect_ratio;
    }
    if (sar.num <= 0 || sar.den <= 0) {
        sar = AVRational{1, 1};
    }
    return sar;
}

static void applyPrCompatibleVideoPropertiesToEncoder(const AVStream* inStream,
                                                      AVCodecContext* encCtx) {
    if (!inStream || !encCtx || !inStream->codecpar) {
        return;
    }

    const AVRational sar = getStreamSampleAspectRatio(inStream);
    if (sar.num > 0 && sar.den > 0) {
        encCtx->sample_aspect_ratio = sar;
    }

    encCtx->color_range = AVCOL_RANGE_MPEG;
    encCtx->color_primaries = AVCOL_PRI_UNSPECIFIED;
    encCtx->color_trc = AVCOL_TRC_UNSPECIFIED;
    encCtx->colorspace = AVCOL_SPC_UNSPECIFIED;
    encCtx->chroma_sample_location = AVCHROMA_LOC_UNSPECIFIED;
    encCtx->field_order = AV_FIELD_PROGRESSIVE;
}

static bool shouldCopyVideoSideData(enum AVPacketSideDataType type) {
    switch (type) {
        case AV_PKT_DATA_DISPLAYMATRIX:
        case AV_PKT_DATA_STEREO3D:
        case AV_PKT_DATA_SPHERICAL:
        case AV_PKT_DATA_CONTENT_LIGHT_LEVEL:
        case AV_PKT_DATA_MASTERING_DISPLAY_METADATA:
        case AV_PKT_DATA_ICC_PROFILE:
            return true;
        default:
            return false;
    }
}

static void copyOutputVideoStreamProperties(const AVStream* inStream,
                                            AVStream* outStream) {
    if (!inStream || !outStream || !inStream->codecpar || !outStream->codecpar) {
        return;
    }

    const AVCodecParameters* inPar = inStream->codecpar;
    const AVRational sar = getStreamSampleAspectRatio(inStream);
    if (sar.num > 0 && sar.den > 0) {
        outStream->sample_aspect_ratio = sar;
        outStream->codecpar->sample_aspect_ratio = sar;
    }

    outStream->codecpar->color_range = AVCOL_RANGE_MPEG;
    outStream->codecpar->color_primaries = AVCOL_PRI_UNSPECIFIED;
    outStream->codecpar->color_trc = AVCOL_TRC_UNSPECIFIED;
    outStream->codecpar->color_space = AVCOL_SPC_UNSPECIFIED;
    outStream->codecpar->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    outStream->codecpar->field_order = AV_FIELD_PROGRESSIVE;
    outStream->avg_frame_rate = inStream->avg_frame_rate;
    outStream->disposition = inStream->disposition;
    av_dict_copy(&outStream->metadata, inStream->metadata, 0);

    for (int i = 0; i < inPar->nb_coded_side_data; ++i) {
        const AVPacketSideData& src = inPar->coded_side_data[i];
        if (!src.data || src.size == 0 || !shouldCopyVideoSideData(src.type)) continue;
        AVPacketSideData* dst = av_packet_side_data_new(&outStream->codecpar->coded_side_data,
                                                        &outStream->codecpar->nb_coded_side_data,
                                                        src.type, src.size, 0);
        if (dst && dst->data) {
            memcpy(dst->data, src.data, src.size);
        }
    }
}

static AVPixelFormat getEncoderPixelFormat(const AVCodec* enc) {
    if (!enc || !enc->pix_fmts) return AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* p = enc->pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    }
    for (const AVPixelFormat* p = enc->pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    }
    return enc->pix_fmts[0];
}

static int64_t estimatePrCompatibleBitRate(int width, int height, AVRational frameRate) {
    double fps = 25.0;
    if (frameRate.num > 0 && frameRate.den > 0) {
        fps = static_cast<double>(frameRate.num) / static_cast<double>(frameRate.den);
    }
    const double bitsPerPixel = 0.225;
    const double target = static_cast<double>(width) * static_cast<double>(height) * fps * bitsPerPixel;
    return std::max<int64_t>(2LL * 1000 * 1000, static_cast<int64_t>(target + 0.5));
}

bool VideoTranscoder::transcode(const std::string& inputPath,
                                TranscodeOptions& opts,
                                ProgressCallback progress,
                                std::string* outError) {
    auto setErr = [outError](const std::string& msg) {
        if (outError) *outError = msg;
    };
    auto setErrFromAv = [outError](int ret) {
        if (outError) {
            char buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, buf, sizeof(buf));
            *outError = std::string(buf);
        }
    };

    if (inputPath.empty()) {
        LOG_ERROR("[VideoTranscoder] Empty input path");
        setErr("输入路径为空");
        return false;
    }

    if (!fs::exists(inputPath)) {
        LOG_ERROR("[VideoTranscoder] Input file not found: %s", inputPath.c_str());
        setErr("文件不存在: " + inputPath);
        return false;
    }

    if (opts.outputPath.empty()) {
        opts.outputPath = getTranscodeOutputPath(inputPath);
    }

    std::string outDir = fs::path(opts.outputPath).parent_path().string();
    if (!fs::exists(outDir)) {
        if (!fs::create_directories(outDir)) {
            LOG_ERROR("[VideoTranscoder] Failed to create output dir: %s", outDir.c_str());
            setErr("无法创建输出目录: " + outDir);
            return false;
        }
    }

    AVFormatContext* fmtCtx = nullptr;
    int openRet = avformat_open_input(&fmtCtx, inputPath.c_str(), nullptr, nullptr);
    if (openRet < 0) {
        LOG_ERROR("[VideoTranscoder] Failed to open input: %s", inputPath.c_str());
        setErrFromAv(openRet);
        return false;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        LOG_ERROR("[VideoTranscoder] Failed to find stream info");
        setErr("无法解析流信息");
        avformat_close_input(&fmtCtx);
        return false;
    }

    int videoStreamIdx = -1;
    std::vector<int> audioStreamIdxs;  // 支持多条音轨
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        auto type = fmtCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoStreamIdx < 0) {
            videoStreamIdx = i;
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIdxs.push_back(i);
        }
    }

    if (videoStreamIdx < 0) {
        LOG_ERROR("[VideoTranscoder] No video stream found");
        setErr("未找到视频流");
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVCodecParameters* codecpar = fmtCtx->streams[videoStreamIdx]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(codecpar->codec_id);
    if (!dec) {
        setErr("不支持的视频编码格式");
        avformat_close_input(&fmtCtx);
        return false;
    }
    AVCodecContext* decCtx = avcodec_alloc_context3(dec);
    if (!decCtx) {
        setErr("内存分配失败");
        avformat_close_input(&fmtCtx);
        return false;
    }
    avcodec_parameters_to_context(decCtx, codecpar);
    decCtx->pkt_timebase = fmtCtx->streams[videoStreamIdx]->time_base;
    int decOpenRet = avcodec_open2(decCtx, dec, nullptr);
    if (decOpenRet < 0) {
        LOG_ERROR("[VideoTranscoder] Failed to open decoder");
        setErrFromAv(decOpenRet);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    LOG_INFO("[VideoTranscoder] Using decoder: %s (pix_fmt=%d, size=%dx%d)",
             dec->name ? dec->name : "unknown", (int)decCtx->pix_fmt, decCtx->width, decCtx->height);

    // 保持源视频显示分辨率不变。若硬件编码器不支持该尺寸，直接失败提示，
    // 不再自动拉伸/缩放到 16 倍数，避免画面比例变化。
    int width = decCtx->width;
    int height = decCtx->height;
    if (width <= 0 || height <= 0) {
        setErr("视频分辨率无效");
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }
    const int sourceWidth = width;
    const int sourceHeight = height;

    AVRational frameRate = av_guess_frame_rate(fmtCtx, fmtCtx->streams[videoStreamIdx], nullptr);
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = AVRational{25, 1};
    }

    AVFormatContext* outFmtCtx = nullptr;
    avformat_alloc_output_context2(&outFmtCtx, nullptr, "mp4", opts.outputPath.c_str());
    if (!outFmtCtx) {
        setErr("无法创建 MP4 输出上下文");
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    const AVCodec* enc = nullptr;
    AVCodecContext* encCtx = nullptr;
    std::string encoderName;  // 记录使用的编码器名称
    {
        enc = findH264Encoder();
        if (!enc) {
            setErr(std::string(TRANSCODE_ENCODER_NAME) + " 编码器不可用");
            avformat_free_context(outFmtCtx);
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            return false;
        }

        encCtx = avcodec_alloc_context3(enc);
        if (!encCtx) {
            setErr("编码器内存分配失败");
            avformat_free_context(outFmtCtx);
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            return false;
        }

        int targetWidth = sourceWidth;
        int targetHeight = sourceHeight;

        encCtx->width = targetWidth;
        encCtx->height = targetHeight;
        encCtx->pix_fmt = getEncoderPixelFormat(enc);
        encCtx->time_base = AVRational{frameRate.den, frameRate.num};
        encCtx->framerate = frameRate;
        applyPrCompatibleVideoPropertiesToEncoder(fmtCtx->streams[videoStreamIdx], encCtx);
        encCtx->gop_size = 30;
        encCtx->max_b_frames = 0;

        encCtx->bit_rate = estimatePrCompatibleBitRate(targetWidth, targetHeight, frameRate);
        
        // 某些硬编要求设置 max_rate 和 buffer_size 才会严格遵循码率
        encCtx->rc_max_rate = encCtx->bit_rate;
        encCtx->rc_buffer_size = static_cast<int>(
            std::min<int64_t>(encCtx->bit_rate * 2, std::numeric_limits<int>::max()));

        // 设置 profile/level，优先使用 Main 以提升画质（Baseline 不支持针对画质的某些特性）
        encCtx->profile = FF_PROFILE_H264_MAIN;
        av_opt_set(encCtx->priv_data, "profile", "main", 0);

        // codec_tag=0：避免 MP4 muxer 因 codec_tag 不匹配报错
        encCtx->codec_tag = 0;

        if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AVDictionary* encOpts = nullptr;
        int encOpenRet = avcodec_open2(encCtx, enc, &encOpts);
        av_dict_free(&encOpts);
        if (encOpenRet >= 0) {
            encoderName = enc->name;  // 记录成功的编码器
            LOG_INFO("[VideoTranscoder] Using encoder: %s (pix_fmt=%d)", enc->name, (int)encCtx->pix_fmt);
            if (patchH264SpsForPrCompatiblePlayback(encCtx->extradata, encCtx->extradata_size)) {
                LOG_INFO("[VideoTranscoder] Patched H.264 SPS for PR-compatible playback in encoder extradata");
            }
        } else {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(encOpenRet, errBuf, sizeof(errBuf));
            setErr(std::string(TRANSCODE_ENCODER_NAME) +
                   " 不支持当前原始分辨率，已停止以避免生成异常视频: " + std::string(errBuf));
            LOG_WARN("[VideoTranscoder] Encoder %s failed (ret=%d)", enc->name, encOpenRet);
            avcodec_free_context(&encCtx);
            encCtx = nullptr;
            enc = nullptr;
        }
    }
    if (!enc || !encCtx) {
        if (outError && outError->empty()) setErr("无可用 H264 编码器");
        else if (outError && outError->find("Generic error") != std::string::npos)
            *outError = std::string(TRANSCODE_ENCODER_NAME) + " 编码器不可用，请确认设备支持或尝试其他视频";
        avformat_free_context(outFmtCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    // outFmtCtx 已在前面分配

    AVStream* outVideoStream = avformat_new_stream(outFmtCtx, nullptr);
    if (!outVideoStream) {
        setErr("无法创建输出流");
        avformat_free_context(outFmtCtx);
        avcodec_free_context(&encCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }
    outVideoStream->time_base = AVRational{1, PR_COMPATIBLE_MP4_TIMESCALE};
    avcodec_parameters_from_context(outVideoStream->codecpar, encCtx);
    copyOutputVideoStreamProperties(fmtCtx->streams[videoStreamIdx], outVideoStream);

    // 音频流：所有音轨全部 stream copy（不重新编码）
    // key=输入流index, value=对应的输出流指针
    std::unordered_map<int, AVStream*> audioStreamMap;
    if (opts.copyAudio) {
        for (int asi : audioStreamIdxs) {
            AVStream* outAs = avformat_new_stream(outFmtCtx, nullptr);
            if (outAs) {
                avcodec_parameters_copy(outAs->codecpar, fmtCtx->streams[asi]->codecpar);
                // 确保声道信息被复制（虽然 params_copy 已经做了，但显式重置 tag 避免某些容器问题）
                outAs->codecpar->codec_tag = 0;
                outAs->time_base = fmtCtx->streams[asi]->time_base;
                // 复制流元数据（如语言、轨道名等，对多音轨识别很重要）
                av_dict_copy(&outAs->metadata, fmtCtx->streams[asi]->metadata, 0);
                audioStreamMap[asi] = outAs;
            }
        }
        LOG_INFO("[VideoTranscoder] Audio tracks: %d copied", (int)audioStreamMap.size());
    }

    if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        int avioRet = avio_open(&outFmtCtx->pb, opts.outputPath.c_str(), AVIO_FLAG_WRITE);
        if (avioRet < 0) {
            LOG_ERROR("[VideoTranscoder] Failed to open output file");
            setErrFromAv(avioRet);
            avformat_free_context(outFmtCtx);
            avcodec_free_context(&encCtx);
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            return false;
        }
    }

    AVDictionary* muxOpts = nullptr;
    av_dict_set(&muxOpts, "video_track_timescale", "25000", 0);
    int ret = avformat_write_header(outFmtCtx, &muxOpts);
    av_dict_free(&muxOpts);
    if (ret < 0) {
        LOG_ERROR("[VideoTranscoder] Failed to write output header: %d", ret);
        setErrFromAv(ret);
        if (outFmtCtx->pb && !(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&outFmtCtx->pb);
        }
        avformat_free_context(outFmtCtx);
        avcodec_free_context(&encCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    SwsContext* swsCtx = nullptr;
    AVPixelFormat dstFmt = encCtx->pix_fmt;
    int64_t streamDuration = (fmtCtx->duration > 0)
        ? av_rescale_q(fmtCtx->duration, AVRational{1, AV_TIME_BASE},
                       fmtCtx->streams[videoStreamIdx]->time_base)
        : 1;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* encFrame = av_frame_alloc();
    if (!pkt || !frame || !encFrame) {
        setErr("内存分配失败");
        if (encFrame) av_frame_free(&encFrame);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        if (swsCtx) sws_freeContext(swsCtx);
        if (outFmtCtx->pb && !(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&outFmtCtx->pb);
        }
        avformat_free_context(outFmtCtx);
        avcodec_free_context(&encCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }
    encFrame->format = dstFmt;
    encFrame->width = encCtx->width;
    encFrame->height = encCtx->height;
    encFrame->sample_aspect_ratio = encCtx->sample_aspect_ratio;
    encFrame->color_range = encCtx->color_range;
    encFrame->color_primaries = encCtx->color_primaries;
    encFrame->color_trc = encCtx->color_trc;
    encFrame->colorspace = encCtx->colorspace;
    encFrame->chroma_location = encCtx->chroma_sample_location;
    if (av_frame_get_buffer(encFrame, 32) < 0) {
        av_frame_free(&encFrame);
        encFrame = nullptr;
    }

    bool directFrameCompatible = decCtx->pix_fmt == dstFmt &&
                                 decCtx->width == encCtx->width &&
                                 decCtx->height == encCtx->height;

    LOG_INFO("[VideoTranscoder] Frame path: decoderFmt=%d encoderFmt=%d decoderSize=%dx%d encoderSize=%dx%d cpuConvert=%d",
             (int)decCtx->pix_fmt, (int)dstFmt, decCtx->width, decCtx->height, encCtx->width, encCtx->height,
             directFrameCompatible ? 0 : 1);

    int swsSrcWidth = 0;
    int swsSrcHeight = 0;
    AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;

    bool success = true;
    bool fatalWriteError = false;
    int64_t lastPts = 0;
    AVRational streamTimeBase = fmtCtx->streams[videoStreamIdx]->time_base;

    auto failWrite = [&](const char* where, int writeRet) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(writeRet, errBuf, sizeof(errBuf));
        LOG_ERROR("[VideoTranscoder] %s failed: ret=%d (%s)", where, writeRet, errBuf);
        if (outError && outError->empty()) {
            *outError = std::string(where) + " failed: " + errBuf;
        }
        success = false;
        fatalWriteError = true;
    };

    auto writeEncodedPacket = [&](AVPacket* packet, const char* where) -> bool {
        if (!packet || !packet->buf || packet->size <= 0 || fatalWriteError) {
            return !fatalWriteError;
        }
        if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE) && !outFmtCtx->pb) {
            failWrite(where, AVERROR(EIO));
            return false;
        }
        packet->stream_index = outVideoStream->index;
        patchH264SpsForPrCompatiblePlayback(packet->data, packet->size);
        av_packet_rescale_ts(packet, encCtx->time_base, outVideoStream->time_base);
        int writeRet = av_interleaved_write_frame(outFmtCtx, packet);
        if (writeRet < 0) {
            failWrite(where, writeRet);
            return false;
        }
        return true;
    };

    auto drainEncoder = [&](const char* where) -> bool {
        AVPacket* encPkt = av_packet_alloc();
        if (!encPkt) {
            setErr("内存分配失败");
            success = false;
            fatalWriteError = true;
            return false;
        }
        bool ok = true;
        while (!fatalWriteError) {
            int recvRet = avcodec_receive_packet(encCtx, encPkt);
            if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF) {
                break;
            }
            if (recvRet < 0) {
                LOG_WARN("[VideoTranscoder] Encoder receive_packet failed: %d", recvRet);
                success = false;
                ok = false;
                break;
            }
            if (!writeEncodedPacket(encPkt, where)) {
                ok = false;
                av_packet_unref(encPkt);
                break;
            }
            av_packet_unref(encPkt);
        }
        av_packet_free(&encPkt);
        return ok;
    };

    auto processDecodedFrame = [&]() -> bool {
        AVFrame* srcFrame = frame;
        AVPixelFormat srcFmt = static_cast<AVPixelFormat>(srcFrame->format);
        bool frameCompatible = directFrameCompatible &&
                               srcFmt == dstFmt &&
                               srcFrame->width == encCtx->width &&
                               srcFrame->height == encCtx->height;
        if (!frameCompatible && !encFrame) {
            LOG_ERROR("[VideoTranscoder] Encoder frame conversion buffer unavailable");
            setErr("视频帧格式不兼容且无法分配转换缓冲");
            success = false;
            return false;
        }
        bool needNewSws = !swsCtx ||
                          swsSrcWidth != srcFrame->width ||
                          swsSrcHeight != srcFrame->height ||
                          swsSrcFmt != srcFmt;
        if (!frameCompatible && needNewSws) {
            if (swsCtx) {
                sws_freeContext(swsCtx);
                swsCtx = nullptr;
            }
            swsCtx = sws_getContext(srcFrame->width, srcFrame->height, srcFmt,
                                    encCtx->width, encCtx->height, dstFmt,
                                    SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!swsCtx) {
                LOG_ERROR("[VideoTranscoder] Failed to create SwsContext");
                setErr("视频像素格式转换初始化失败");
                success = false;
                return false;
            }
            // 设置色彩空间和范围，避免 YUV420P→NV12 转换时色彩偏移
            // srcRange=0 表示 limited range (TV)，dstRange=0 同样
            const int *srcCoeffs = sws_getCoefficients(SWS_CS_ITU709);
            const int *dstCoeffs = sws_getCoefficients(SWS_CS_ITU709);
            int srcRange = (srcFrame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
            int dstRange = srcRange;  // 保持与源一致
            sws_setColorspaceDetails(swsCtx, srcCoeffs, srcRange,
                                     dstCoeffs, dstRange, 0, 1 << 16, 1 << 16);
            swsSrcWidth = srcFrame->width;
            swsSrcHeight = srcFrame->height;
            swsSrcFmt = srcFmt;
        }
        if (progress && srcFrame->pts != AV_NOPTS_VALUE && srcFrame->pts > lastPts) {
            lastPts = srcFrame->pts;
            float prog = 100.0f * (float)srcFrame->pts / (float)streamDuration;
            if (prog > 100.0f) prog = 100.0f;
            // 根据编码器名称生成友好的显示文本
            std::string statusText;
            statusText = "硬件编码中 (RK MPP)...";
            progress(prog, statusText, encoderName);
        }
        int64_t encPts = (srcFrame->pts != AV_NOPTS_VALUE)
            ? av_rescale_q(srcFrame->pts, streamTimeBase, encCtx->time_base)
            : AV_NOPTS_VALUE;
        AVFrame* toEncode = srcFrame;
        if (!frameCompatible && encFrame) {
            encFrame->sample_aspect_ratio = srcFrame->sample_aspect_ratio.num > 0 && srcFrame->sample_aspect_ratio.den > 0
                ? srcFrame->sample_aspect_ratio
                : encCtx->sample_aspect_ratio;
            encFrame->color_range = srcFrame->color_range;
            encFrame->color_primaries = srcFrame->color_primaries;
            encFrame->color_trc = srcFrame->color_trc;
            encFrame->colorspace = srcFrame->colorspace;
            encFrame->chroma_location = srcFrame->chroma_location;
            int scaleRet = sws_scale(swsCtx, srcFrame->data, srcFrame->linesize, 0, srcFrame->height,
                                     encFrame->data, encFrame->linesize);
            if (scaleRet <= 0) {
                LOG_ERROR("[VideoTranscoder] Failed to convert video frame: ret=%d", scaleRet);
                setErr("视频帧格式转换失败");
                success = false;
                return false;
            }
            encFrame->pts = encPts;
            toEncode = encFrame;
        } else {
            toEncode->sample_aspect_ratio = srcFrame->sample_aspect_ratio.num > 0 && srcFrame->sample_aspect_ratio.den > 0
                ? srcFrame->sample_aspect_ratio
                : encCtx->sample_aspect_ratio;
            toEncode->color_range = srcFrame->color_range;
            toEncode->color_primaries = srcFrame->color_primaries;
            toEncode->color_trc = srcFrame->color_trc;
            toEncode->colorspace = srcFrame->colorspace;
            toEncode->chroma_location = srcFrame->chroma_location;
            toEncode->pts = encPts;
        }
        int encSendRet = avcodec_send_frame(encCtx, toEncode);
        if (encSendRet == AVERROR(EAGAIN)) {
            if (!drainEncoder("write video frame")) {
                return false;
            }
            encSendRet = avcodec_send_frame(encCtx, toEncode);
        }
        if (encSendRet >= 0) {
            return drainEncoder("write video frame");
        } else if (encSendRet != AVERROR_EOF) {
            LOG_WARN("[VideoTranscoder] Encoder send_frame failed: %d", encSendRet);
            if (outError && outError->empty()) {
                *outError = "H.264硬件编码器不支持当前原始分辨率，已停止以避免改变画面";
            }
            success = false;
            return false;
        }
        return true;
    };

    while (!fatalWriteError && av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIdx) {
            int sendRet = avcodec_send_packet(decCtx, pkt);
            if (sendRet == AVERROR(EAGAIN)) {
                while (avcodec_receive_frame(decCtx, frame) == 0) {
                    if (!processDecodedFrame()) break;
                }
                if (!fatalWriteError) {
                    sendRet = avcodec_send_packet(decCtx, pkt);
                }
            }
            if (!fatalWriteError && sendRet == 0) {
                while (avcodec_receive_frame(decCtx, frame) == 0) {
                    if (!processDecodedFrame()) break;
                }
            } else if (sendRet != AVERROR_EOF && !fatalWriteError) {
                LOG_WARN("[VideoTranscoder] Decoder send_packet failed: %d", sendRet);
                success = false;
            }
        } else if (!fatalWriteError) {
            auto it = audioStreamMap.find(pkt->stream_index);
            if (it != audioStreamMap.end()) {
                AVStream* outAs = it->second;
                pkt->stream_index = outAs->index;
                av_packet_rescale_ts(pkt,
                                      fmtCtx->streams[it->first]->time_base,
                                      outAs->time_base);
                int writeRet = av_interleaved_write_frame(outFmtCtx, pkt);
                if (writeRet < 0) {
                    failWrite("write audio packet", writeRet);
                }
            }
        }
        av_packet_unref(pkt);
    }
    if (!fatalWriteError) {
        avcodec_send_packet(decCtx, nullptr);
        while (avcodec_receive_frame(decCtx, frame) == 0) {
            if (!processDecodedFrame()) break;
        }
    }

    if (!fatalWriteError) {
        int flushRet = avcodec_send_frame(encCtx, nullptr);
        if (flushRet >= 0 || flushRet == AVERROR_EOF) {
            drainEncoder("write video frame during final flush");
        } else {
            LOG_WARN("[VideoTranscoder] Encoder final flush failed: %d", flushRet);
            success = false;
        }
    }

    if (success && !fatalWriteError) {
        int trailerRet = av_write_trailer(outFmtCtx);
        if (trailerRet < 0) {
            failWrite("write trailer", trailerRet);
        }
    }
    if (success && !fatalWriteError && progress) progress(100.0f, "完成", encoderName);

    if (encFrame) av_frame_free(&encFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (swsCtx) sws_freeContext(swsCtx);
    if (outFmtCtx->pb && !(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&outFmtCtx->pb);
    }
    avformat_free_context(outFmtCtx);
    avcodec_free_context(&encCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    if (!success) {
        // 编码失败时清理残留的不完整输出文件，避免留下损坏文件
        if (fs::exists(opts.outputPath)) {
            std::error_code ec;
            fs::remove(opts.outputPath, ec);
            LOG_WARN("[VideoTranscoder] Removed incomplete output: %s", opts.outputPath.c_str());
        }
        return false;
    }
    LOG_INFO("[VideoTranscoder] Transcode complete: %s -> %s", inputPath.c_str(), opts.outputPath.c_str());

    // 编码成功：用转码后的文件替换原文件
    // 步骤：1) 原文件改名为 .bak  2) 新文件重命名为原路径  3) 删除 .bak
    std::string bakPath = inputPath + ".bak";
    std::error_code ec;
    fs::rename(inputPath, bakPath, ec);
    if (ec) {
        LOG_WARN("[VideoTranscoder] Cannot backup original (ec=%d), keeping output at: %s", ec.value(), opts.outputPath.c_str());
        setErr("编码完成但无法备份原文件，未替换原文件");
        return false;
    }
    fs::rename(opts.outputPath, inputPath, ec);
    if (ec) {
        // 重命名失败，尝试还原备份
        LOG_ERROR("[VideoTranscoder] Failed to replace original (ec=%d), restoring backup", ec.value());
        fs::rename(bakPath, inputPath, ec);
        setErr("编码完成但无法替换原文件，已还原原文件");
        return false;
    }
    // 删除备份
    fs::remove(bakPath, ec);
    // 告知调用方最终路径就是 inputPath
    opts.outputPath = inputPath;
    
    // 创建 .optimized 标记文件，防止重复处理；只有写入可靠记录后才算完整成功。
    if (!writeOptimizedMarker(inputPath, opts)) {
        setErr("编码完成但无法写入优化记录");
        return false;
    }
    
    LOG_INFO("[VideoTranscoder] Original replaced and marked as optimized: %s", inputPath.c_str());
    return true;
}

std::future<bool> VideoTranscoder::transcodeAsync(const std::string& inputPath,
                                                  TranscodeOptions opts,
                                                  ProgressCallback progress) {
    return std::async(std::launch::async, [inputPath, opts, progress]() {
        TranscodeOptions mutableOpts = opts;
        return transcode(inputPath, mutableOpts, progress);
    });
}

#else

std::string VideoTranscoder::getTranscodeOutputPath(const std::string& inputPath) {
    (void)inputPath;
    return "";
}

bool VideoTranscoder::transcode(const std::string& inputPath,
                                TranscodeOptions& opts,
                                ProgressCallback progress,
                                std::string* outError) {
    (void)inputPath;
    (void)opts;
    (void)progress;
    LOG_ERROR("[VideoTranscoder] Transcode only supported on Android");
    if (outError) *outError = "转码仅支持 Android 平台";
    return false;
}

std::future<bool> VideoTranscoder::transcodeAsync(const std::string& inputPath,
                                                  TranscodeOptions opts,
                                                  ProgressCallback progress) {
    (void)inputPath;
    (void)opts;
    (void)progress;
    return std::async(std::launch::deferred, []() { return false; });
}

#endif

} // namespace hsvj
