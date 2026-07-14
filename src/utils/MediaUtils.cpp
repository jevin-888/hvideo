/**
 * @file MediaUtils.cpp（文件名）
 * @brief 媒体工具类实现
 */

#include "MediaUtils.h"
#include "core/PathConfig.h"
#include "utils/Logger.h"
#include "utils/JsonUtils.h"
#include "utils/FileUtils.h"
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <future>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/dict.h>
}

#include <android/log.h>

namespace fs = std::filesystem;

namespace hsvj {

// 静态成员初始化
std::vector<std::future<void>> MediaUtils::thumbnailTasks_;
std::mutex MediaUtils::thumbnailTasksMutex_;

bool MediaUtils::probeVideoInfo(const std::string &videoPath,
                                MediaVideoInfo &outInfo,
                                std::string *outError) {
    outInfo = MediaVideoInfo{};

    AVFormatContext *fmt = nullptr;
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    av_dict_set(&opts, "probesize", "1048576", 0);

    int ret = avformat_open_input(&fmt, videoPath.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        if (outError) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            *outError = errbuf;
        }
        return false;
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        if (outError) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            *outError = errbuf;
        }
        avformat_close_input(&fmt);
        return false;
    }

    outInfo.valid = true;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream *stream = fmt->streams[i];
        AVCodecParameters *params = stream ? stream->codecpar : nullptr;
        if (!params || params->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }

        outInfo.hasVideo = true;
        outInfo.width = params->width;
        outInfo.height = params->height;
        outInfo.bitRate = params->bit_rate > 0 ? params->bit_rate : fmt->bit_rate;
        const char *codecName = avcodec_get_name(params->codec_id);
        outInfo.codecName = codecName ? codecName : "unknown";

        AVRational fps = av_guess_frame_rate(fmt, stream, nullptr);
        if (fps.num > 0 && fps.den > 0) {
            outInfo.frameRate = av_q2d(fps);
        } else if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
            outInfo.frameRate = av_q2d(stream->avg_frame_rate);
        } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
            outInfo.frameRate = av_q2d(stream->r_frame_rate);
        }
        if (!std::isfinite(outInfo.frameRate) || outInfo.frameRate < 0.0) {
            outInfo.frameRate = 0.0;
        }
        break;
    }

    avformat_close_input(&fmt);
    return true;
}

void MediaUtils::cleanupCompletedThumbnailTasks() {
    thumbnailTasks_.erase(
        std::remove_if(thumbnailTasks_.begin(), thumbnailTasks_.end(),
            [](std::future<void>& f) {
                return f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            }),
        thumbnailTasks_.end()
    );
}

void MediaUtils::waitAllThumbnailTasks() {
    std::lock_guard<std::mutex> lock(thumbnailTasksMutex_);
    for (auto& task : thumbnailTasks_) {
        if (task.valid()) task.wait_for(std::chrono::seconds(5));
    }
    thumbnailTasks_.clear();
}

std::vector<uint8_t> MediaUtils::encodeFrameToJPEG(AVFrame *frame, int quality, int maxWidth, int maxHeight) {
    std::vector<uint8_t> jpegData;
    if (!frame || !frame->data[0]) return jpegData;

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return jpegData;

    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) return jpegData;

    int dstWidth = frame->width;
    int dstHeight = frame->height;
    if (frame->width > maxWidth || frame->height > maxHeight) {
        double scale = std::min((double)maxWidth / frame->width, (double)maxHeight / frame->height);
        dstWidth = ((int)(frame->width * scale) / 2) * 2;
        dstHeight = ((int)(frame->height * scale) / 2) * 2;
    }

    codecCtx->width = dstWidth;
    codecCtx->height = dstHeight;
    codecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    codecCtx->time_base = {1, 25};
    codecCtx->framerate = {25, 1};
    codecCtx->global_quality = FF_QP2LAMBDA * quality;
    codecCtx->flags |= AV_CODEC_FLAG_QSCALE;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        return jpegData;
    }

    AVFrame *yuvFrame = av_frame_alloc();
    yuvFrame->format = AV_PIX_FMT_YUVJ420P;
    yuvFrame->width = dstWidth;
    yuvFrame->height = dstHeight;
    av_frame_get_buffer(yuvFrame, 32);

    SwsContext *swsCtx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                      dstWidth, dstHeight, AV_PIX_FMT_YUVJ420P,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (swsCtx) {
        sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height, yuvFrame->data, yuvFrame->linesize);
        sws_freeContext(swsCtx);
    }

    AVPacket *pkt = av_packet_alloc();
    if (avcodec_send_frame(codecCtx, yuvFrame) >= 0) {
        if (avcodec_receive_packet(codecCtx, pkt) == 0) {
            jpegData.assign(pkt->data, pkt->data + pkt->size);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&yuvFrame);
    avcodec_free_context(&codecCtx);
    return jpegData;
}

bool MediaUtils::isBlackFrame(AVFrame *frame, int threshold, int sampleRatio) {
    if (!frame || !frame->data[0]) return true;
    long long total = 0, count = 0;
    for (int y = 0; y < frame->height; y += sampleRatio) {
        for (int x = 0; x < frame->width; x += sampleRatio) {
            total += frame->data[0][y * frame->linesize[0] + x];
            count++;
        }
    }
    return count == 0 || (double)total / count < threshold;
}

std::string MediaUtils::getThumbnailCacheDir(const std::string &videoPath) {
    return hsvj::ROOT_PATH + ".cache/thumbnails/";
}

bool MediaUtils::generateThumbnailFromFrame(AVFrame *frame, const std::string &outputPath, 
                                          const std::string &videoPath,
                                          std::function<void(const std::string &, const std::string &)> broadcastCallback) {
    // 关键修复：确保缓存目录存在，否则缩略图永远存不下来
    FileUtils::createDirectory(FileUtils::getDirectory(outputPath));
    
    std::vector<uint8_t> jpeg = encodeFrameToJPEG(frame, 70, 320, 180);
    if (jpeg.empty()) return false;
    std::ofstream out(outputPath, std::ios::binary);
    out.write((char*)jpeg.data(), jpeg.size());
    out.close();
    if (broadcastCallback) {
        Json::Value n; n["type"] = "thumbnail_ready"; n["path"] = videoPath;
        broadcastCallback("thumbnail", JsonUtils::toString(n));
    }
    return true;
}

void MediaUtils::generateThumbnailAsync(const std::string &videoPath,
                                      std::function<void(const std::string &, const std::string &)> broadcastCallback) {
    std::lock_guard<std::mutex> lock(thumbnailTasksMutex_);
    cleanupCompletedThumbnailTasks();
    thumbnailTasks_.push_back(std::async(std::launch::async, [videoPath, broadcastCallback]() {
        AVFormatContext *f = nullptr;
        if (avformat_open_input(&f, videoPath.c_str(), nullptr, nullptr) < 0) return;
        avformat_find_stream_info(f, nullptr);
        int vIdx = -1;
        for (unsigned i=0; i<f->nb_streams; i++) if (f->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vIdx=i; break; }
        if (vIdx < 0) { avformat_close_input(&f); return; }

        AVCodecParameters *p = f->streams[vIdx]->codecpar;

        // 关键修复1：强制使用软件解码器生成缩略图
        // 硬件解码器（如 h264_rkmpp）在 seek 后可能返回黑帧或缓存帧
        const AVCodec *c = nullptr;
        if (p->codec_id == AV_CODEC_ID_H264 || p->codec_id == AV_CODEC_ID_HEVC) {
            // 对于H264/HEVC，强制使用软件解码
            c = avcodec_find_decoder_by_name(p->codec_id == AV_CODEC_ID_H264 ? "h264" : "hevc");
        }
        if (!c) c = avcodec_find_decoder(p->codec_id);
        if (!c) { avformat_close_input(&f); return; }

        AVCodecContext *ctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ctx, p);

        // 关键修复2：禁用硬件加速
        ctx->thread_count = 1;  // 单线程，减少开销

        if (avcodec_open2(ctx, c, nullptr) < 0) {
            avcodec_free_context(&ctx);
            avformat_close_input(&f);
            return;
        }

        AVFrame *frame = av_frame_alloc();
        AVPacket *pkt = av_packet_alloc();

        // 关键修复3：seek到视频10%位置（更可能有有效画面）
        int64_t seekPos = 0;
        if (f->duration > 10000000) {  // 如果视频大于10秒
            seekPos = f->duration / 10;  // seek到10%位置
        } else if (f->duration > 2000000) {  // 如果视频大于2秒
            seekPos = 1 * AV_TIME_BASE;  // seek到1秒
        }

        if (seekPos > 0) {
            av_seek_frame(f, -1, seekPos, AVSEEK_FLAG_BACKWARD);
            // 关键修复4：seek后必须flush解码器
            avcodec_flush_buffers(ctx);
        }

        int got = 0;
        int validFrames = 0;
        while (av_read_frame(f, pkt) >= 0 && got < 100) {  // 增加尝试次数到100
            if (pkt->stream_index == vIdx) {
                if (avcodec_send_packet(ctx, pkt) >= 0) {
                    while (avcodec_receive_frame(ctx, frame) == 0) {
                        validFrames++;
                        // 关键修复5：跳过前几帧（可能是seek后的不完整帧）
                        if (validFrames > 2 && !isBlackFrame(frame)) {
                            size_t h = std::hash<std::string>{}(videoPath);
                            generateThumbnailFromFrame(frame, getThumbnailCacheDir(videoPath) + std::to_string(h) + ".jpg", videoPath, broadcastCallback);
                            av_packet_unref(pkt);
                            goto cleanup;
                        }
                    }
                }
            }
            av_packet_unref(pkt);
            got++;
        }

        // 如果没找到非黑帧，使用第一个有效帧
        if (validFrames == 0) {
            av_seek_frame(f, -1, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(ctx);
            while (av_read_frame(f, pkt) >= 0 && validFrames < 10) {
                if (pkt->stream_index == vIdx && avcodec_send_packet(ctx, pkt) >= 0) {
                    if (avcodec_receive_frame(ctx, frame) == 0) {
                        size_t h = std::hash<std::string>{}(videoPath);
                        generateThumbnailFromFrame(frame, getThumbnailCacheDir(videoPath) + std::to_string(h) + ".jpg", videoPath, broadcastCallback);
                        av_packet_unref(pkt);
                        break;
                    }
                }
                av_packet_unref(pkt);
                validFrames++;
            }
        }

cleanup:
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        avformat_close_input(&f);
    }));
}

bool MediaUtils::checkVideoFormatSupport(const std::string &path, std::string &outFmt, std::string &outErr) {
    AVFormatContext *f = nullptr;
    bool bad = false;
    if (avformat_open_input(&f, path.c_str(), nullptr, nullptr) < 0) return true;
    if (avformat_find_stream_info(f, nullptr) >= 0) {
        for (unsigned i=0; i<f->nb_streams; i++) {
            AVCodecParameters *p = f->streams[i]->codecpar;
            if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
                const char* cname = avcodec_get_name(p->codec_id);
                std::string cstr = cname ? cname : "unknown";
                
                // 1. Pro Codecs 检查
                if (p->codec_id == AV_CODEC_ID_DXV || p->codec_id == AV_CODEC_ID_HAP) {
                    bad = true; outErr = "UNSUPPORTED_PRO_CODEC"; outFmt = cstr; break;
                }

                // 2. Bit depth / Pixel 格式
                const AVPixFmtDescriptor *d = av_pix_fmt_desc_get((AVPixelFormat)p->format);
                if (d) {
                    int b = d->comp[0].depth;
                    outFmt = cstr + " " + d->name + " (" + std::to_string(b) + "-bit)";
                    if (b >= 10) { bad = true; outErr = "10BIT"; }
                } else {
                    AVPixelFormat pf = (AVPixelFormat)p->format;
                    outFmt = cstr + " pix_" + std::to_string(pf);
                    if (pf >= AV_PIX_FMT_YUV420P10LE && pf <= AV_PIX_FMT_YUV444P12BE) { bad = true; outErr = "10BIT"; }
                }

                // 3. 说明：高色度 / BT2020
                if (!bad) {
                    AVPixelFormat pf = (AVPixelFormat)p->format;
                    bool is2020 = (p->color_space == AVCOL_SPC_BT2020_NCL || p->color_space == AVCOL_SPC_BT2020_CL);
                    bool isHigh = (pf == AV_PIX_FMT_YUV422P || pf == AV_PIX_FMT_YUV444P);
                    if (is2020 || isHigh) { bad = true; outErr = "COLOR_CONFIG"; if(is2020) outFmt += " [BT2020]"; if(isHigh) outFmt += " [HighChroma]"; }
                }

                // 4. Codec Tag 检查
                char tagBuf[5];
                uint32_t tag = p->codec_tag;
                tagBuf[0] = (char)(tag & 0xFF);
                tagBuf[1] = (char)((tag >> 8) & 0xFF);
                tagBuf[2] = (char)((tag >> 16) & 0xFF);
                tagBuf[3] = (char)((tag >> 24) & 0xFF);
                tagBuf[4] = '\0';
                
                std::string tagStr = tagBuf; std::transform(tagStr.begin(), tagStr.end(), tagStr.begin(), ::tolower);
                outFmt += " [Tag: " + tagStr + "]";

                if (tagStr.find("dxv") != std::string::npos || tagStr.find("hap") != std::string::npos) {
                    bad = true; outErr = "CODEC_TAG_MATCH"; break;
                }

                if (bad) break;
            }
        }
    }
    avformat_close_input(&f);
    return !bad;
}

} // 命名空间 hsvj
