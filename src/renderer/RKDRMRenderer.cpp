/**

 * @file RKDRMRenderer.cpp（文件名）

 * @brief Rockchip DRM AFBC 零拷贝 渲染器 实现

 *

 * Uses Rockchip specific DRM extensions to implement RKMPP AFBC 零拷贝:

 * - 通过 DRM_IOCTL_PRIME_FD_TO_HANDLE 导入 DMA-BUF

 * - 创建 FB using DRM_FORMAT_MOD_ARM_AFBC modifier

 * - 使用 DRM atomic 操作进行无边框渲染

 */



#ifdef __ANDROID__



#include "renderer/RKDRMRenderer.h"

#include "utils/Logger.h"



#include <algorithm>

#include <cstring>

#include <cerrno>

#include <fcntl.h>

#include <sys/ioctl.h>

#include <sys/mman.h>

#include <unistd.h>





extern "C" {

#include <libavutil/frame.h>

#include <libavutil/hwcontext_drm.h>

}



// 使用头文件中的定义

#include <drm/drm_fourcc.h>



namespace hsvj {



// ============================================================================

// 构造与析构

// ============================================================================



RKDRMRenderer::RKDRMRenderer()

    : drmFd_(-1), initialized_(false), afbcSupported_(false), crtcId_(0),

      connectorId_(0), primaryPlaneId_(0), displayWidth_(0), displayHeight_(0),

      fbCacheIndex_(0), atomicReq_(nullptr), framesRendered_(0),

      framesFailed_(0) {



  std::memset(&displayMode_, 0, sizeof(displayMode_));

  std::memset(fbCache_, 0, sizeof(fbCache_));

  for (int i = 0; i < FB_CACHE_SIZE; i++) {

    fbCache_[i].dmaBufFd = -1;

  }

}



RKDRMRenderer::~RKDRMRenderer() { shutdown(); }



// ============================================================================

// 初始化

// ============================================================================



bool RKDRMRenderer::initialize() {

  std::lock_guard<std::mutex> lock(mutex_);



  if (initialized_) {

    return true;

  }



  // 步骤 1：Open DRM device

  if (!openDrmDevice()) {

    LOG_ERROR("[RK DRM] Failed to open DRM device");

    return false;

  }



  // 步骤 2：Find CRTC and Connector

  if (!findCrtcAndConnector()) {

    LOG_ERROR("[RK DRM] Failed to find CRTC/Connector");

    close(drmFd_);

    drmFd_ = -1;

    return false;

  }



  // 步骤 3：Find overlay planes

  if (!findOverlayPlanes()) {

    LOG_WARN("[RK DRM] No overlay planes found, will use primary plane");

  }



  // 步骤 4：Check AFBC support

  if (!checkAFBCSupport()) {

    LOG_WARN("[RK DRM] AFBC support check failed, may not be supported or needs special config");

    // Continue trying, some drivers may support at run时间

  }



  // 步骤 5：尝试设置 DRM Master（可选，overlay plane 渲染不需要它）

  // 注意：Android 上通常由 SurfaceFlinger 持有 master 权限

  // 其他应用无法获取该权限属于正常情况，overlay plane 渲染不需要 master 权限。

  if (drmSetMaster(drmFd_) != 0) {
    // 继续执行，overlay plane 渲染不需要 master 权限
  }



  // 步骤 6：创建 atomic request

  atomicReq_ = drmModeAtomicAlloc();

  if (!atomicReq_) {

    LOG_ERROR("[RK DRM] Failed to allocate atomic request");

    close(drmFd_);

    drmFd_ = -1;

    return false;

  }



  initialized_ = true;

  return true;

}



void RKDRMRenderer::shutdown() {

  std::lock_guard<std::mutex> lock(mutex_);



  if (!initialized_) {

    return;

  }



  // 释放 FB cache

  for (int i = 0; i < FB_CACHE_SIZE; i++) {

    if (fbCache_[i].valid) {

      destroyFrameBuffer(fbCache_[i].fbId, fbCache_[i].gemHandle);

      fbCache_[i].valid = false;

    }

  }



  // 释放 atomic request

  if (atomicReq_) {

    drmModeAtomicFree(atomicReq_);

    atomicReq_ = nullptr;

  }



  // 释放 DRM Master (if obtained before)

  if (drmFd_ >= 0) {

    // 尝试释放 master，失败属于正常情况（可能未获取或已被其他进程持有）

    drmDropMaster(drmFd_);

    close(drmFd_);

    drmFd_ = -1;

  }



  initialized_ = false;

}



// ============================================================================

// DRM 设备初始化

// ============================================================================



bool RKDRMRenderer::openDrmDevice() {

  // 尝试多个 DRM 设备路径

  const char *drmDevices[] = {"/dev/dri/card0", "/dev/dri/card1",

                              "/dev/dri/renderD128", nullptr};



  for (int i = 0; drmDevices[i] != nullptr; i++) {

    drmFd_ = open(drmDevices[i], O_RDWR | O_CLOEXEC);

    if (drmFd_ >= 0) {

      // 确认是否为 Rockchip DRM

      drmVersionPtr ver = drmGetVersion(drmFd_);

      if (ver) {

        bool isRockchip = (strstr(ver->name, "rockchip") != nullptr);

        drmFreeVersion(ver);



        if (isRockchip) {

          return true;

        }

      }

      close(drmFd_);

      drmFd_ = -1;

    }

  }



  // 如果未找到 Rockchip 驱动，则使用第一个可用驱动

  drmFd_ = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

  if (drmFd_ >= 0) {

    LOG_WARN("[RK DRM] Using non-Rockchip DRM device, AFBC may not be supported");

    return true;

  }



  LOG_ERROR("[RK DRM] Cannot open any DRM device");

  return false;

}



bool RKDRMRenderer::findCrtcAndConnector() {

  drmModeResPtr res = drmModeGetResources(drmFd_);

  if (!res) {

    LOG_ERROR("[RK DRM] drmModeGetResources failed");

    return false;

  }



  // 查找 first connected connector

  drmModeConnectorPtr connector = nullptr;

  for (int i = 0; i < res->count_connectors; i++) {

    connector = drmModeGetConnector(drmFd_, res->connectors[i]);

    if (connector && connector->connection == DRM_MODE_CONNECTED &&

        connector->count_modes > 0) {

      connectorId_ = connector->connector_id;



      // 使用 preferred 模式 or first 模式

      displayMode_ = connector->modes[0];

      for (int j = 0; j < connector->count_modes; j++) {

        if (connector->modes[j].type & DRM_MODE_TYPE_PREFERRED) {

          displayMode_ = connector->modes[j];

          break;

        }

      }



      displayWidth_ = displayMode_.hdisplay;

      displayHeight_ = displayMode_.vdisplay;

      break;

    }

    if (connector) {

      drmModeFreeConnector(connector);

      connector = nullptr;

    }

  }



  if (!connector) {

    LOG_ERROR("[RK DRM] No connected display found");

    drmModeFreeResources(res);

    return false;

  }



  // 查找 CRTC

  drmModeEncoderPtr encoder = nullptr;

  if (connector->encoder_id) {

    encoder = drmModeGetEncoder(drmFd_, connector->encoder_id);

  }



  if (encoder && encoder->crtc_id) {

    crtcId_ = encoder->crtc_id;

  } else {

    // 查找 available CRTC

    for (int i = 0; i < res->count_crtcs; i++) {

      if (connector->encoders) {

        for (int j = 0; j < connector->count_encoders; j++) {

          drmModeEncoderPtr enc =

              drmModeGetEncoder(drmFd_, connector->encoders[j]);

          if (enc && (enc->possible_crtcs & (1 << i))) {

            crtcId_ = res->crtcs[i];

            drmModeFreeEncoder(enc);

            break;

          }

          if (enc)

            drmModeFreeEncoder(enc);

        }

      }

      if (crtcId_)

        break;

    }

  }



  if (encoder)

    drmModeFreeEncoder(encoder);

  drmModeFreeConnector(connector);

  drmModeFreeResources(res);



  if (!crtcId_) {

    LOG_ERROR("[RK DRM] No available CRTC found");

    return false;

  }



  return true;

}



bool RKDRMRenderer::findOverlayPlanes() {

  drmModePlaneResPtr planeRes = drmModeGetPlaneResources(drmFd_);

  if (!planeRes) {

    LOG_WARN("[RK DRM] drmModeGetPlaneResources failed");

    return false;

  }



  for (uint32_t i = 0; i < planeRes->count_planes; i++) {

    drmModePlanePtr plane = drmModeGetPlane(drmFd_, planeRes->planes[i]);

    if (!plane)

      continue;



    // 检查 if plane 支持 当前 CRTC

    bool supportsCrtc = false;

    drmModeResPtr res = drmModeGetResources(drmFd_);

    if (res) {

      for (int j = 0; j < res->count_crtcs; j++) {

        if (res->crtcs[j] == crtcId_ && (plane->possible_crtcs & (1 << j))) {

          supportsCrtc = true;

          break;

        }

      }

      drmModeFreeResources(res);

    }



    if (supportsCrtc) {

      // 获取 plane type

      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(

          drmFd_, plane->plane_id, DRM_MODE_OBJECT_PLANE);



      uint32_t planeType = DRM_PLANE_TYPE_OVERLAY; // 默认值

    if (props) {

        for (uint32_t j = 0; j < props->count_props; j++) {

          drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[j]);

          if (prop && strcmp(prop->name, "type") == 0) {

            planeType = props->prop_values[j];

            drmModeFreeProperty(prop);

            break;

          }

          if (prop)

            drmModeFreeProperty(prop);

        }

        drmModeFreeObjectProperties(props);

      }



      if (planeType == DRM_PLANE_TYPE_PRIMARY) {

        primaryPlaneId_ = plane->plane_id;

      } else if (planeType == DRM_PLANE_TYPE_OVERLAY) {

        DRMPlaneInfo info = {};

        info.planeId = plane->plane_id;

        info.crtcId = crtcId_;

        info.inUse = false;

        overlayPlanes_.push_back(info);

      }

    }



    drmModeFreePlane(plane);

  }



  drmModeFreePlaneResources(planeRes);



  return !overlayPlanes_.empty();

}



bool RKDRMRenderer::checkAFBCSupport() {

  // 检查 if driver 支持 AFBC modifier

  // On Rockchip platform, AFBC is usually supported by 默认



  // Method 1: 检查 IN_FORMATS blob property

  if (!overlayPlanes_.empty()) {

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(

        drmFd_, overlayPlanes_[0].planeId, DRM_MODE_OBJECT_PLANE);



    if (props) {

      for (uint32_t i = 0; i < props->count_props; i++) {

        drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);

        if (prop && strcmp(prop->name, "IN_FORMATS") == 0) {

          // 存在 IN_FORMATS 表示支持 modifier

          afbcSupported_ = true;

          drmModeFreeProperty(prop);

          break;

        }

        if (prop)

          drmModeFreeProperty(prop);

      }

      drmModeFreeObjectProperties(props);

    }

  }



  // 方法 2：尝试创建 AFBC FB（实际测试）

  // This will be done on first 渲染

    return afbcSupported_;

}



// ============================================================================

// FB 管理

// ============================================================================



bool RKDRMRenderer::importDmaBufAsGem(int dmaBufFd, uint32_t *gemHandle) {

  struct drm_prime_handle prime = {};

  prime.fd = dmaBufFd;

  prime.flags = 0;

  prime.handle = 0;



  if (ioctl(drmFd_, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) != 0) {

    LOG_ERROR("[RK DRM] DRM_IOCTL_PRIME_FD_TO_HANDLE failed: %s",

              strerror(errno));

    return false;

  }



  *gemHandle = prime.handle;

  return true;

}



bool RKDRMRenderer::createAFBCFrameBuffer(int dmaBufFd, uint32_t width,

                                          uint32_t height, uint32_t format,

                                          uint64_t modifier, uint32_t *fbId) {

  // 将 DMA-BUF 导入为 GEM

  uint32_t gemHandle = 0;

  if (!importDmaBufAsGem(dmaBufFd, &gemHandle)) {

    return false;

  }



  // 计算 pitch（AFBC 场景下为估算值）

  uint32_t pitch = width;

  if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {

    pitch = width; // Y 平面 pitch

  } else if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888) {

    pitch = width * 4;

  }



  // 使用 modifier 创建 FB

  struct drm_mode_fb_cmd2 cmd = {};

  cmd.width = width;

  cmd.height = height;

  cmd.pixel_format = format;

  cmd.flags = DRM_MODE_FB_MODIFIERS;

  cmd.handles[0] = gemHandle;

  cmd.pitches[0] = pitch;

  cmd.offsets[0] = 0;

  cmd.modifier[0] = modifier;



  // NV12 需要设置第二个平面

  if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {

    cmd.handles[1] = gemHandle;

    cmd.pitches[1] = pitch;          // UV 平面 pitch

    cmd.offsets[1] = pitch * height; // UV 平面偏移

    cmd.modifier[1] = modifier;

  }



  if (ioctl(drmFd_, DRM_IOCTL_MODE_ADDFB2, &cmd) != 0) {

    // 如果出现 AFBC 格式不支持错误，则降级为警告（系统会自动回退）
    if (modifier != 0) {

      // AFBC 失败, return false to let caller try LINEAR
      // 不输出错误日志，避免与系统错误消息重复

    } else {

      // LINEAR also 失败, this is a real 错误

    LOG_ERROR("[RK DRM] DRM_IOCTL_MODE_ADDFB2 failed: %s (format=0x%x, "

                "modifier=LINEAR)",

                strerror(errno), format);

    }



    // 关闭 GEM handle

  struct drm_gem_close gemClose = {};

    gemClose.handle = gemHandle;

    ioctl(drmFd_, DRM_IOCTL_GEM_CLOSE, &gemClose);



    return false;

  }



  *fbId = cmd.fb_id;



  // 缓存 FB 信息

  AFBCFrameBuffer &cache = fbCache_[fbCacheIndex_];

  if (cache.valid) {

    destroyFrameBuffer(cache.fbId, cache.gemHandle);

  }

  cache.dmaBufFd = dmaBufFd;

  cache.gemHandle = gemHandle;

  cache.fbId = cmd.fb_id;

  cache.width = width;

  cache.height = height;

  cache.format = format;

  cache.modifier = modifier;

  cache.valid = true;



  fbCacheIndex_ = (fbCacheIndex_ + 1) % FB_CACHE_SIZE;



  return true;

}



void RKDRMRenderer::destroyFrameBuffer(uint32_t fbId, uint32_t gemHandle) {

  if (fbId) {

    ioctl(drmFd_, DRM_IOCTL_MODE_RMFB, &fbId);

  }

  if (gemHandle) {

    struct drm_gem_close gemClose = {};

    gemClose.handle = gemHandle;

    ioctl(drmFd_, DRM_IOCTL_GEM_CLOSE, &gemClose);

  }

}



// ============================================================================

// 渲染

// ============================================================================



bool RKDRMRenderer::renderDrmPrimeFrame(const AVFrame *frame, int planeIndex,

                                        int dstX, int dstY, int dstW,

                                        int dstH) {

  if (!initialized_ || !frame || frame->format != AV_PIX_FMT_DRM_PRIME) {

    return false;

  }



  std::lock_guard<std::mutex> lock(mutex_);



  // 获取 DRM 帧描述符

  const AVDRMFrameDescriptor *desc =

      reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);

  if (!desc || desc->nb_objects == 0 || desc->nb_layers == 0) {

    LOG_ERROR("[RK DRM] Invalid DRM frame descriptor");

    framesFailed_++;

    return false;

  }



  int dmaBufFd = desc->objects[0].fd;

  uint32_t width = frame->width;

  uint32_t height = frame->height;

  uint32_t format = desc->layers[0].format;



  // 检查格式是否支持 AFBC

  // 常见支持 AFBC 的格式：NV12、NV21、ARGB8888、XRGB8888

  // 类似 0x38 的特殊格式可能不支持 AFBC

  bool formatSupportsAFBC = (format == DRM_FORMAT_NV12 || 

                             format == DRM_FORMAT_NV21 ||

                             format == DRM_FORMAT_ARGB8888 || 

                             format == DRM_FORMAT_XRGB8888 ||

                             format == DRM_FORMAT_ABGR8888 ||

                             format == DRM_FORMAT_XBGR8888);



  // 确定 modifier（AFBC 或 LINEAR）

  uint64_t modifier = 0;

  if (formatSupportsAFBC && afbcSupported_) {

    // RKMPP 默认使用 AFBC 16x16 块

    modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |

                                       AFBC_FORMAT_MOD_SPARSE);

  } else {

    // 格式或系统不支持 AFBC 时，直接使用 LINEAR

    modifier = 0;

    if (!formatSupportsAFBC) {

      static int formatWarningCount = 0;

      if (++formatWarningCount <= 3) {

        LOG_WARN("[RK DRM] Format 0x%x doesn't support AFBC, using LINEAR modifier", format);

      }

    }

  }



  // 检查是否命中缓存

  uint32_t fbId = 0;

  for (int i = 0; i < FB_CACHE_SIZE; i++) {

    if (fbCache_[i].valid && fbCache_[i].dmaBufFd == dmaBufFd &&

        fbCache_[i].format == format && fbCache_[i].modifier == modifier) {

      fbId = fbCache_[i].fbId;

      break;

    }

  }



  // 创建新的 FB

  if (fbId == 0) {

    if (!createAFBCFrameBuffer(dmaBufFd, width, height, format, modifier,

                               &fbId)) {

      // 如果 AFBC 失败，则尝试 LINEAR（如果尚未使用 LINEAR）

    if (modifier != 0) {

        modifier = 0; // 技术标识：LINEAR

    if (!createAFBCFrameBuffer(dmaBufFd, width, height, format, modifier,

                                   &fbId)) {

          LOG_ERROR("[RK DRM] Failed to create FB (format=0x%x)", format);

          framesFailed_++;

          return false;

        }

      } else {

        LOG_ERROR("[RK DRM] Failed to create FB (format=0x%x, modifier=LINEAR)", format);

        framesFailed_++;

        return false;

      }

    }

  }



  // 检查 plane 索引

  if (planeIndex < 0 || planeIndex >= static_cast<int>(overlayPlanes_.size())) {

    // 使用第一个可用 overlay plane

    planeIndex = 0;

  }



  if (overlayPlanes_.empty()) {

    LOG_ERROR("[RK DRM] No available overlay plane");

    framesFailed_++;

    return false;

  }



  // 设置 atomic request

  if (!setupAtomicRequest(atomicReq_, planeIndex, fbId, width, height, dstX,

                          dstY, dstW, dstH)) {

    framesFailed_++;

    return false;

  }



  framesRendered_++;

  return true;

}



bool RKDRMRenderer::setupAtomicRequest(drmModeAtomicReqPtr req, int planeIndex,

                                       uint32_t fbId, int srcW, int srcH,

                                       int dstX, int dstY, int dstW, int dstH) {

  if (!req || planeIndex >= static_cast<int>(overlayPlanes_.size())) {

    return false;

  }



  uint32_t planeId = overlayPlanes_[planeIndex].planeId;



  // 获取属性 ID

  uint32_t propFbId = getPlaneProperty(planeId, "FB_ID");

  uint32_t propCrtcId = getPlaneProperty(planeId, "CRTC_ID");

  uint32_t propSrcX = getPlaneProperty(planeId, "SRC_X");

  uint32_t propSrcY = getPlaneProperty(planeId, "SRC_Y");

  uint32_t propSrcW = getPlaneProperty(planeId, "SRC_W");

  uint32_t propSrcH = getPlaneProperty(planeId, "SRC_H");

  uint32_t propCrtcX = getPlaneProperty(planeId, "CRTC_X");

  uint32_t propCrtcY = getPlaneProperty(planeId, "CRTC_Y");

  uint32_t propCrtcW = getPlaneProperty(planeId, "CRTC_W");

  uint32_t propCrtcH = getPlaneProperty(planeId, "CRTC_H");



  if (!propFbId || !propCrtcId) {

    LOG_ERROR("[RK DRM] Failed to get plane properties");

    return false;

  }



  // 设置属性

  drmModeAtomicAddProperty(req, planeId, propFbId, fbId);

  drmModeAtomicAddProperty(req, planeId, propCrtcId, crtcId_);



  // 源矩形（16.16 定点格式）
  drmModeAtomicAddProperty(req, planeId, propSrcX, 0);

  drmModeAtomicAddProperty(req, planeId, propSrcY, 0);

  drmModeAtomicAddProperty(req, planeId, propSrcW, srcW << 16);

  drmModeAtomicAddProperty(req, planeId, propSrcH, srcH << 16);



  // 目标矩形

  drmModeAtomicAddProperty(req, planeId, propCrtcX, dstX);

  drmModeAtomicAddProperty(req, planeId, propCrtcY, dstY);

  drmModeAtomicAddProperty(req, planeId, propCrtcW, dstW);

  drmModeAtomicAddProperty(req, planeId, propCrtcH, dstH);



  return true;

}



bool RKDRMRenderer::commit() {

  if (!initialized_ || !atomicReq_) {

    return false;

  }



  std::lock_guard<std::mutex> lock(mutex_);



  uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;



  int ret = drmModeAtomicCommit(drmFd_, atomicReq_, flags, nullptr);



  // 清理 request 以供下一帧使用

  drmModeAtomicSetCursor(atomicReq_, 0);



  if (ret != 0) {

    if (errno != EBUSY) { // EBUSY 是正常情况，上一帧尚未完成

    LOG_ERROR("[RK DRM] drmModeAtomicCommit failed: %s", strerror(errno));

    }

    return false;

  }



  return true;

}



// ============================================================================

// 辅助方法

// ============================================================================



uint32_t RKDRMRenderer::getPlaneProperty(uint32_t planeId, const char *name) {

  drmModeObjectPropertiesPtr props =

      drmModeObjectGetProperties(drmFd_, planeId, DRM_MODE_OBJECT_PLANE);

  if (!props)

    return 0;



  uint32_t propId = 0;

  for (uint32_t i = 0; i < props->count_props; i++) {

    drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);

    if (prop && strcmp(prop->name, name) == 0) {

      propId = prop->prop_id;

      drmModeFreeProperty(prop);

      break;

    }

    if (prop)

      drmModeFreeProperty(prop);

  }



  drmModeFreeObjectProperties(props);

  return propId;

}



uint32_t RKDRMRenderer::getCrtcProperty(uint32_t crtcId, const char *name) {

  drmModeObjectPropertiesPtr props =

      drmModeObjectGetProperties(drmFd_, crtcId, DRM_MODE_OBJECT_CRTC);

  if (!props)

    return 0;



  uint32_t propId = 0;

  for (uint32_t i = 0; i < props->count_props; i++) {

    drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);

    if (prop && strcmp(prop->name, name) == 0) {

      propId = prop->prop_id;

      drmModeFreeProperty(prop);

      break;

    }

    if (prop)

      drmModeFreeProperty(prop);

  }



  drmModeFreeObjectProperties(props);

  return propId;

}



uint32_t RKDRMRenderer::getConnectorProperty(uint32_t connectorId,

                                             const char *name) {

  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(

      drmFd_, connectorId, DRM_MODE_OBJECT_CONNECTOR);

  if (!props)

    return 0;



  uint32_t propId = 0;

  for (uint32_t i = 0; i < props->count_props; i++) {

    drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);

    if (prop && strcmp(prop->name, name) == 0) {

      propId = prop->prop_id;

      drmModeFreeProperty(prop);

      break;

    }

    if (prop)

      drmModeFreeProperty(prop);

  }



  drmModeFreeObjectProperties(props);

  return propId;

}



void RKDRMRenderer::getDisplaySize(int *width, int *height) const {

  if (width)

    *width = displayWidth_;

  if (height)

    *height = displayHeight_;

}



} // 命名空间 hsvj



#endif // 结束 __ANDROID__

