/**
 * @file ApngLoader.cpp（文件名）
 * @brief APNG 动画图像加载器实
 * 
 * 使用 libpng 解析 APNG 格式，提取所有帧数据
 * 基于 APNG Disassembler 2.8 (Max Stepin) 的代码适配
 */

#include "utils/ApngLoader.h"
#include "utils/Logger.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>
#include <png.h>
#include <zlib.h>
#include "stb_image.h"

#ifdef __ANDROID__
#include "rk_mpi.h"
#include "mpp_buffer.h"
#endif

namespace hsvj {

// PNG 数据块 ID
#define id_IHDR 0x52444849
#define id_acTL 0x4C546361
#define id_fcTL 0x4C546366
#define id_IDAT 0x54414449
#define id_fdAT 0x54416466
#define id_IEND 0x444E4549

#define notabc(c) ((c) < 65 || (c) > 122 || ((c) > 90 && (c) < 97))

// 最PNG 尺寸
const unsigned long MAX_PNG_SIZE = 1000000UL;

struct CHUNK_LOCAL {
    uint8_t* p;
    uint32_t size;
    
    CHUNK_LOCAL() : p(nullptr), size(0) {}
    ~CHUNK_LOCAL() {} // 本移植版中内存由外部或手动管理
};

// libpng 回调
static void info_fn(png_structp png_ptr, png_infop info_ptr)
{
  png_set_expand(png_ptr);
  png_set_strip_16(png_ptr);
  png_set_gray_to_rgb(png_ptr);
  png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);
  (void)png_set_interlace_handling(png_ptr);
  png_read_update_info(png_ptr, info_ptr);
}

static void row_fn(png_structp png_ptr, png_bytep new_row, png_uint_32 row_num, int pass)
{
  (void)pass;
  uint8_t** rows = (uint8_t**)png_get_progressive_ptr(png_ptr);
  png_progressive_combine_row(png_ptr, rows[row_num], new_row);
}

static void compose_frame(uint8_t** rows_dst, uint8_t** rows_src, uint8_t bop, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
  uint32_t i, j;
  int u, v, al;

  for (j=0; j<h; j++)
  {
    uint8_t* sp = rows_src[j];
    uint8_t* dp = rows_dst[j+y] + x*4;

    if (bop == 0)
      memcpy(dp, sp, w*4);
    else
    for (i=0; i<w; i++, sp+=4, dp+=4)
    {
        if (sp[3] == 255) {
          memcpy(dp, sp, 4);
        } else if (sp[3] != 0) {
          if (dp[3] != 0) {
            u = sp[3] * 255;
            v = (255 - sp[3]) * dp[3];
            al = u + v;
            if (al != 0) { // 防止除零
                dp[0] = (uint8_t)((sp[0] * u + dp[0] * v) / al);
                dp[1] = (uint8_t)((sp[1] * u + dp[1] * v) / al);
                dp[2] = (uint8_t)((sp[2] * u + dp[2] * v) / al);
                dp[3] = (uint8_t)(al / 255);
            }
          } else {
            memcpy(dp, sp, 4);
          }
        }
    }
  }
}

static inline uint32_t read_chunk_internal(FILE * f, CHUNK_LOCAL * pChunk)
{
  unsigned char len[4];
  pChunk->size = 0;
  pChunk->p = nullptr;
  if (fread(&len, 4, 1, f) == 1)
  {
    pChunk->size = png_get_uint_32(len) + 12;
    pChunk->p = new uint8_t[pChunk->size];
    memcpy(pChunk->p, len, 4);
    if (fread(pChunk->p + 4, pChunk->size - 4, 1, f) == 1)
      return *(uint32_t *)(pChunk->p + 4);
    
    // 如果读取内容失败，必须释放刚申请的内存
    delete[] pChunk->p;
    pChunk->p = nullptr;
    pChunk->size = 0;
  }
  return 0;
}

static int processing_start(png_structp & png_ptr, png_infop & info_ptr, void * frame_ptr, bool hasInfo, CHUNK_LOCAL & chunkIHDR, std::vector<CHUNK_LOCAL>& chunksInfo)
{
  unsigned char header[8] = {137, 80, 78, 71, 13, 10, 26, 10};

  png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  info_ptr = png_create_info_struct(png_ptr);
  if (!png_ptr || !info_ptr)
    return 1;

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return 1;
  }

  png_set_crc_action(png_ptr, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
  png_set_progressive_read_fn(png_ptr, frame_ptr, info_fn, row_fn, NULL);

  png_process_data(png_ptr, info_ptr, header, 8);
  png_process_data(png_ptr, info_ptr, chunkIHDR.p, chunkIHDR.size);

  if (hasInfo)
    for (unsigned int i=0; i<chunksInfo.size(); i++)
      png_process_data(png_ptr, info_ptr, chunksInfo[i].p, chunksInfo[i].size);
  return 0;
}

static int processing_data(png_structp png_ptr, png_infop info_ptr, uint8_t * p, uint32_t size)
{
  if (!png_ptr || !info_ptr)
    return 1;

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    return 1;
  }

  png_process_data(png_ptr, info_ptr, p, size);
  return 0;
}

static int processing_finish(png_structp &png_ptr, png_infop &info_ptr)
{
  unsigned char footer[12] = {0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

  if (!png_ptr || !info_ptr)
    return 1;

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return 1;
  }

  png_process_data(png_ptr, info_ptr, footer, 12);
  png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

  return 0;
}

ApngLoader::ApngLoader() 
    : totalDurationMs_(0), loaded_(false), mppBufferGroup_(nullptr) {
}

ApngLoader::~ApngLoader() {
    free();
}

void ApngLoader::freeAllFrameData() {
    for (auto& frame : frames_) {
        frame.freeData();
    }
}

void ApngLoader::free() {
    // 先释MPP buffer，再释放普通内
    // 这样可以避免freeAllFrameData() 中误操作 MPP buffer 管理data 指针
#ifdef __ANDROID__
    for (auto& frame : frames_) {
        if (frame.mppBuffer) {
            // 确保 buffer 有效后再释放
            MppBuffer buffer = (MppBuffer)frame.mppBuffer;
            frame.mppBuffer = nullptr;  // 先清空指针，避免重复释放
            frame.dmaBufFd = -1;
            frame.data = nullptr;  // MPP buffer 管理的内存，不需delete
            mpp_buffer_put(buffer);
        }
    }
    if (mppBufferGroup_) {
        MppBufferGroup group = (MppBufferGroup)mppBufferGroup_;
        mppBufferGroup_ = nullptr;  // 先清空指
        mpp_buffer_group_put(group);
    }
#endif

    // 释放普通内存（MPP buffer 的内存已经在上面的循环中处理
    freeAllFrameData();

    frames_.clear();
    totalDurationMs_ = 0;
    loaded_ = false;
}

bool ApngLoader::loadAsSingleFrame(const std::string& filePath) {
    int width, height, channels;
    
    unsigned char* data = stbi_load(filePath.c_str(), &width, &height, &channels, 4);
    if (!data) {
        LOG_ERROR("ApngLoader: Failed to load image: %s", filePath.c_str());
        return false;
    }
    
    // 创建单帧
    ApngFrame frame;
    frame.width = (uint32_t)width;
    frame.height = (uint32_t)height;
    size_t imagesize = width * height * 4;

#ifdef __ANDROID__
    if (!mppBufferGroup_) {
        try {
            // 使用 DMA_HEAP DRM 类型分配物理连续内存
            mpp_buffer_group_get_internal((MppBufferGroup*)&mppBufferGroup_, MPP_BUFFER_TYPE_DMA_HEAP);
        } catch (const std::exception& e) {
            LOG_WARN("ApngLoader: Failed to get MPP buffer group (DMA_HEAP): %s", e.what());
        } catch (...) {
            LOG_WARN("ApngLoader: Failed to get MPP buffer group (DMA_HEAP): unknown exception");
        }
    }
    
    if (mppBufferGroup_) {
        try {
            MppBuffer mppBuf = nullptr;
            if (mpp_buffer_get((MppBufferGroup)mppBufferGroup_, &mppBuf, imagesize) == MPP_OK) {
                frame.mppBuffer = mppBuf;
                frame.data = (uint8_t*)mpp_buffer_get_ptr(mppBuf);
                frame.dmaBufFd = mpp_buffer_get_fd(mppBuf);
                LOG_DEBUG("ApngLoader: Allocated MPP buffer for single frame: fd=%d", frame.dmaBufFd);
            }
        } catch (const std::exception& e) {
            LOG_WARN("ApngLoader: Failed to allocate MPP buffer: %s", e.what());
        } catch (...) {
            LOG_WARN("ApngLoader: Failed to allocate MPP buffer: unknown exception");
        }
    }
#endif

    if (!frame.data) {
        frame.data = new uint8_t[imagesize];
    }
    memcpy(frame.data, data, imagesize);
    frame.delayNum = 1;
    frame.delayDen = 10;  // 默认 100ms 延迟
    frame.timestampMs = 0;
    
    stbi_image_free(data);
    
    frames_.push_back(frame);
    calculateTimestamps();
    loaded_ = true;
    
    LOG_DEBUG("ApngLoader: Loaded image as single frame: %s (%dx%d)", 
             filePath.c_str(), width, height);
    
    return true;
}

bool ApngLoader::load(const std::string& filePath) {
    free();
    filePath_ = filePath;
    
    // 检查 file extension - non-PNG images go directly to stb_image
    std::string ext;
    size_t dotPos = filePath.rfind('.');
    if (dotPos != std::string::npos) {
        ext = filePath.substr(dotPos);
        // 转为小写后比较
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
    }
    
    // JPEG, BMP, GIF (static), TGA, etc. - skip PNG parsing, 使用 stb_image directly
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".gif") {
        LOG_DEBUG("ApngLoader: Non-PNG format detected (%s), using stb_image", ext.c_str());
        return loadAsSingleFrame(filePath);
    }
    
    FILE * f;
    uint32_t id, i, j, w, h, w0, h0, x0, y0;
    uint32_t delay_num = 1, delay_den = 10, dop = 0, bop = 0, rowbytes, imagesize;
    unsigned char sig[8];
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    CHUNK_LOCAL chunk;
    CHUNK_LOCAL chunkIHDR;
    std::vector<CHUNK_LOCAL> chunksInfo;
    bool isAnimated = false;
    bool skipFirst = false;
    bool hasInfo = false;
    
    struct RawFrame {
        uint8_t* p;
        png_bytep* rows;
        RawFrame() : p(nullptr), rows(nullptr) {}
        void free_mem() {
            if (p) {
                delete[] p;
                p = nullptr;
            }
            if (rows) {
                delete[] rows;
                rows = nullptr;
            }
        }
    } frameRaw, frameCur, frameNext;

    LOG_DEBUG("ApngLoader: Reading '%s'...", filePath.c_str());

    if ((f = fopen(filePath.c_str(), "rb")) == NULL) {
        LOG_ERROR("ApngLoader: Failed to open file: %s", filePath.c_str());
        return false;
    }

    if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        LOG_ERROR("ApngLoader: Invalid PNG signature: %s", filePath.c_str());
        fclose(f);
        return loadAsSingleFrame(filePath);
    }

    id = read_chunk_internal(f, &chunkIHDR);
    if (id != id_IHDR || chunkIHDR.size != 25) {
        LOG_ERROR("ApngLoader: Invalid IHDR chunk: %s", filePath.c_str());
        if (chunkIHDR.p) {
            delete[] chunkIHDR.p;
            chunkIHDR.p = nullptr;
            chunkIHDR.size = 0;
        }
        fclose(f);
        return loadAsSingleFrame(filePath);
    }

    w = png_get_uint_32(chunkIHDR.p + 8);
    h = png_get_uint_32(chunkIHDR.p + 12);
    w0 = w; h0 = h;

    if (w > MAX_PNG_SIZE || h > MAX_PNG_SIZE) {
        LOG_ERROR("ApngLoader: Image too large: %ux%u", w, h);
        delete[] chunkIHDR.p;
        fclose(f);
        return false;
    }

    x0 = 0; y0 = 0;
    rowbytes = w * 4;
    imagesize = h * rowbytes;

#ifdef __ANDROID__
    if (!mppBufferGroup_) {
        try {
            // RK3568/RK3588 优先使用 DMA_HEAP CMA 内存
            mpp_buffer_group_get_internal((MppBufferGroup*)&mppBufferGroup_, MPP_BUFFER_TYPE_DMA_HEAP);
        } catch (const std::exception& e) {
            LOG_WARN("ApngLoader: Failed to get MPP buffer group (DMA_HEAP): %s", e.what());
        } catch (...) {
            LOG_WARN("ApngLoader: Failed to get MPP buffer group (DMA_HEAP): unknown exception");
        }
        
        if (!mppBufferGroup_) {
            try {
                // 回退DRM
                mpp_buffer_group_get_internal((MppBufferGroup*)&mppBufferGroup_, MPP_BUFFER_TYPE_DRM);
            } catch (const std::exception& e) {
                LOG_WARN("ApngLoader: Failed to get MPP buffer group (DRM): %s", e.what());
            } catch (...) {
                LOG_WARN("ApngLoader: Failed to get MPP buffer group (DRM): unknown exception");
            }
        }
    }
#endif

    frameRaw.p = new uint8_t[imagesize];
    frameRaw.rows = new png_bytep[h];
    for (j=0; j<h; j++)
        frameRaw.rows[j] = frameRaw.p + j * rowbytes;

    if (processing_start(png_ptr, info_ptr, (void *)frameRaw.rows, hasInfo, chunkIHDR, chunksInfo) != 0) {
        LOG_ERROR("ApngLoader: processing_start failed");
        frameRaw.free_mem();
        frameCur.free_mem(); //必须清理
        if (chunkIHDR.p) {
            delete[] chunkIHDR.p;
            chunkIHDR.p = nullptr;
            chunkIHDR.size = 0;
        }
        fclose(f);
        // 注意：processing_start 失败时已在内部通过 setjmp 调用 png_destroy_read_struct，
        // 因此这里不能再次调用，避免 double-free。
        return loadAsSingleFrame(filePath);
    }

    frameCur.p = new uint8_t[imagesize];
    memset(frameCur.p, 0, imagesize);
    frameCur.rows = new png_bytep[h];
    for (j=0; j<h; j++)
        frameCur.rows[j] = frameCur.p + j * rowbytes;

    while ( !feof(f) ) {
        id = read_chunk_internal(f, &chunk);
        if (!id) break;

        if (id == id_acTL && !hasInfo && !isAnimated) {
            isAnimated = true;
            skipFirst = true;
        } else if (id == id_fcTL && (!hasInfo || isAnimated)) {
            if (hasInfo) {
                if (processing_finish(png_ptr, info_ptr) == 0) {
                    frameNext.p = new uint8_t[imagesize];
                    frameNext.rows = new png_bytep[h];
                    for (j=0; j<h; j++)
                        frameNext.rows[j] = frameNext.p + j * rowbytes;

                    if (dop == 2)
                        memcpy(frameNext.p, frameCur.p, imagesize);

                    compose_frame(frameCur.rows, frameRaw.rows, (uint8_t)bop, x0, y0, w0, h0);
                    
                    // Store 帧
                    ApngFrame newFrame;
                    newFrame.width = w;
                    newFrame.height = h;

#ifdef __ANDROID__
                    if (mppBufferGroup_) {
                        try {
                            MppBuffer mppBuf = nullptr;
                            if (mpp_buffer_get((MppBufferGroup)mppBufferGroup_, &mppBuf, imagesize) == MPP_OK) {
                                newFrame.mppBuffer = mppBuf;
                                newFrame.data = (uint8_t*)mpp_buffer_get_ptr(mppBuf);
                                newFrame.dmaBufFd = mpp_buffer_get_fd(mppBuf);
                            }
                        } catch (const std::exception& e) {
                            LOG_WARN("ApngLoader: Failed to allocate MPP buffer: %s", e.what());
                        } catch (...) {
                            LOG_WARN("ApngLoader: Failed to allocate MPP buffer: unknown exception");
                        }
                    }
#endif
                    if (!newFrame.data) {
                        newFrame.data = new uint8_t[imagesize];
                    }
                    memcpy(newFrame.data, frameCur.p, imagesize);
                    newFrame.delayNum = delay_num;
                    newFrame.delayDen = delay_den;
                    frames_.push_back(newFrame);

                    if (dop != 2) {
                        memcpy(frameNext.p, frameCur.p, imagesize);
                        if (dop == 1) {
                            for (j=0; j<h0; j++)
                                memset(frameNext.rows[y0 + j] + x0*4, 0, w0*4);
                        }
                    }
                    frameCur.free_mem();
                    frameCur.p = frameNext.p;
                    frameCur.rows = frameNext.rows;
                    frameNext.p = nullptr; frameNext.rows = nullptr;
                } else {
                    if (chunk.p) delete[] chunk.p;
                    break;
                }
            }

            // New 帧 parameters
            w0 = png_get_uint_32(chunk.p + 12);
            h0 = png_get_uint_32(chunk.p + 16);
            x0 = png_get_uint_32(chunk.p + 20);
            y0 = png_get_uint_32(chunk.p + 24);
            delay_num = (uint32_t)png_get_uint_16(chunk.p + 28);
            delay_den = (uint32_t)png_get_uint_16(chunk.p + 30);
            if (delay_den == 0) delay_den = 100;
            dop = chunk.p[32];
            bop = chunk.p[33];

            if (w0 > w || h0 > h || x0 + w0 > w || y0 + h0 > h || dop > 2 || bop > 1) {
                if (chunk.p) delete[] chunk.p;
                break;
            }

            if (hasInfo) {
                memcpy(chunkIHDR.p + 8, chunk.p + 12, 8);
                if (processing_start(png_ptr, info_ptr, (void *)frameRaw.rows, hasInfo, chunkIHDR, chunksInfo) != 0) {
                    if (chunk.p) delete[] chunk.p;
                    break;
                }
            } else {
                skipFirst = false;
            }

            if (frames_.size() == (skipFirst ? 1 : 0)) {
                bop = 0;
                if (dop == 2) dop = 1;
            }
        } else if (id == id_IDAT) {
            hasInfo = true;
            if (processing_data(png_ptr, info_ptr, chunk.p, chunk.size) != 0) {
                if (chunk.p) delete[] chunk.p;
                break;
            }
        } else if (id == id_fdAT && isAnimated) {
            png_save_uint_32(chunk.p + 4, chunk.size - 16);
            memcpy(chunk.p + 8, "IDAT", 4);
            if (processing_data(png_ptr, info_ptr, chunk.p + 4, chunk.size - 4) != 0) {
                if (chunk.p) delete[] chunk.p;
                break;
            }
        } else if (id == id_IEND) {
            if (hasInfo && processing_finish(png_ptr, info_ptr) == 0) {
                compose_frame(frameCur.rows, frameRaw.rows, (uint8_t)bop, x0, y0, w0, h0);
                ApngFrame newFrame;
                newFrame.width = w;
                newFrame.height = h;

#ifdef __ANDROID__
                if (mppBufferGroup_) {
                    try {
                        MppBuffer mppBuf = nullptr;
                        if (mpp_buffer_get((MppBufferGroup)mppBufferGroup_, &mppBuf, imagesize) == MPP_OK) {
                            newFrame.mppBuffer = mppBuf;
                            newFrame.data = (uint8_t*)mpp_buffer_get_ptr(mppBuf);
                            newFrame.dmaBufFd = mpp_buffer_get_fd(mppBuf);
                        }
                    } catch (const std::exception& e) {
                        LOG_WARN("ApngLoader: Failed to allocate MPP buffer: %s", e.what());
                    } catch (...) {
                        LOG_WARN("ApngLoader: Failed to allocate MPP buffer: unknown exception");
                    }
                }
#endif
                if (!newFrame.data) {
                    newFrame.data = new uint8_t[imagesize];
                }
                memcpy(newFrame.data, frameCur.p, imagesize);
                newFrame.delayNum = delay_num;
                newFrame.delayDen = delay_den;
                frames_.push_back(newFrame);
            }
            if (chunk.p) delete[] chunk.p;
            break;
        } else {
            // 检查 if chunk type is valid text
            if (chunk.p && (notabc(chunk.p[4]) || notabc(chunk.p[5]) || notabc(chunk.p[6]) || notabc(chunk.p[7]))) {
                if (chunk.p) delete[] chunk.p;
                break;
            }

            if (!hasInfo && chunk.p) {
                if (processing_data(png_ptr, info_ptr, chunk.p, chunk.size) != 0) {
                    if (chunk.p) delete[] chunk.p;
                    break;
                }
                chunksInfo.push_back(chunk);
                continue;
            }
        }
        if (chunk.p) delete[] chunk.p;
    }

    // 清理 libpng
    if (png_ptr) {
        processing_finish(png_ptr, info_ptr);
    }

    // 清理 resources
    frameRaw.free_mem();
    frameCur.free_mem();
    frameNext.free_mem();
    for (size_t k=0; k<chunksInfo.size(); k++) if (chunksInfo[k].p) delete[] chunksInfo[k].p;
    if (chunkIHDR.p) delete[] chunkIHDR.p;
    fclose(f);

    if (frames_.empty()) {
        LOG_WARN("ApngLoader: No frames loaded, falling back to stb_image");
        return loadAsSingleFrame(filePath);
    }

    // 如果 skipFirst 为 true 且仍有更多帧，则移除第一帧
    if (skipFirst && frames_.size() > 1) {
        // 正确释放第一帧的内存（兼容普通内存和 MPP buffer
        auto& firstFrame = frames_[0];
#ifdef __ANDROID__
        if (firstFrame.mppBuffer) {
            mpp_buffer_put((MppBuffer)firstFrame.mppBuffer);
            firstFrame.mppBuffer = nullptr;
            firstFrame.dmaBufFd = -1;
            firstFrame.data = nullptr;  // MPP buffer 管理的内存不需delete
        } else
#endif
        {
            firstFrame.freeData();
        }
        frames_.erase(frames_.begin());
    }

    calculateTimestamps();
    loaded_ = true;
    LOG_DEBUG("ApngLoader: Successfully loaded APNG with %zu frames", frames_.size());
    return true;
}

void ApngLoader::calculateTimestamps() {
    double time = 0;
    for (auto& frame : frames_) {
        frame.timestampMs = time;
        time += frame.getDelayMs();
    }
    totalDurationMs_ = time;
}

const ApngFrame* ApngLoader::getFrame(size_t index) const {
    if (index < frames_.size()) {
        return &frames_[index];
    }
    return nullptr;
}

const ApngFrame* ApngLoader::getFrameAtTime(double timeMs) const {
    if (frames_.empty()) return nullptr;
    if (frames_.size() == 1) return &frames_[0];
    
    // 循环播放
    double loopTime = fmod(timeMs, totalDurationMs_);
    if (loopTime < 0) loopTime += totalDurationMs_;
    
    // 查找 matching 帧
    for (int i = (int)frames_.size() - 1; i >= 0; --i) {
        if (loopTime >= frames_[i].timestampMs) {
            return &frames_[i];
        }
    }
    return &frames_[0];
}

} // 命名空间 hsvj
