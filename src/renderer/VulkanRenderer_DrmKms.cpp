#ifdef __ANDROID__

#include "VulkanRenderer.h"
#include "utils/Logger.h"

#include <android/hardware_buffer.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
}

#ifndef DRM_CAP_PRIME
#define DRM_CAP_PRIME 0x5
#endif
#ifndef DRM_CLIENT_CAP_UNIVERSAL_PLANES
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#endif
#ifndef DRM_CLIENT_CAP_ATOMIC
#define DRM_CLIENT_CAP_ATOMIC 3
#endif
#ifndef DRM_IOCTL_MODE_CREATE_DUMB
struct drm_mode_create_dumb {
  uint32_t height;
  uint32_t width;
  uint32_t bpp;
  uint32_t flags;
  uint32_t handle;
  uint32_t pitch;
  uint64_t size;
};
#define DRM_IOCTL_MODE_CREATE_DUMB DRM_IOWR(0xB2, drm_mode_create_dumb)
#endif
#ifndef DRM_IOCTL_MODE_DESTROY_DUMB
struct drm_mode_destroy_dumb {
  uint32_t handle;
};
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, drm_mode_destroy_dumb)
#endif
#ifndef DRM_IOCTL_MODE_CREATEPROPBLOB
struct drm_mode_create_blob {
  uint64_t data;
  uint32_t length;
  uint32_t blob_id;
};
#define DRM_IOCTL_MODE_CREATEPROPBLOB DRM_IOWR(0xBD, drm_mode_create_blob)
#endif
#ifndef DRM_IOCTL_MODE_DESTROYPROPBLOB
struct drm_mode_destroy_blob {
  uint32_t blob_id;
};
#define DRM_IOCTL_MODE_DESTROYPROPBLOB DRM_IOWR(0xBE, drm_mode_destroy_blob)
#endif

namespace hsvj {
namespace {

struct NativeHandleLayout {
  int version;
  int numFds;
  int numInts;
  int data[0];
};

struct AhbNativeHandleInfo {
  std::vector<int> dupFds;
  std::vector<int> ints;
  int pixelStride = 0;
  int byteStride = 0;
};

struct DrmObjectProp {
  uint32_t id = 0;
  uint64_t value = 0;
};

bool propEnabled(const char *name) {
  char value[PROP_VALUE_MAX] = {};
  const int len = __system_property_get(name, value);
  if (len <= 0) {
    return false;
  }
  return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
         strcasecmp(value, "on") == 0 || strcasecmp(value, "yes") == 0;
}

bool drmKmsPresenterRequested() {
  char value[PROP_VALUE_MAX] = {};
  const int len =
      __system_property_get("persist.hsvj.output.backend", value);
  return len <= 0 || strcmp(value, "drm-kms") == 0;
}

bool drmGetCapValue(int fd, uint64_t capability, uint64_t *value) {
  struct DrmGetCap {
    uint64_t capability;
    uint64_t value;
  } cap = {};
  cap.capability = capability;
  if (ioctl(fd, DRM_IOWR(0x0c, DrmGetCap), &cap) != 0) {
    return false;
  }
  *value = cap.value;
  return true;
}

int drmSetClientCapValue(int fd, uint64_t capability, uint64_t value) {
  struct DrmSetClientCap {
    uint64_t capability;
    uint64_t value;
  } cap = {};
  cap.capability = capability;
  cap.value = value;
  return ioctl(fd, DRM_IOW(0x0d, DrmSetClientCap), &cap);
}

DrmObjectProp getDrmObjectProp(int fd, uint32_t objectId, uint32_t objectType,
                               const char *name) {
  DrmObjectProp result{};
  drmModeObjectPropertiesPtr props =
      drmModeObjectGetProperties(fd, objectId, objectType);
  if (!props) {
    return result;
  }
  for (uint32_t i = 0; i < props->count_props; ++i) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop) {
      continue;
    }
    if (strcmp(prop->name, name) == 0) {
      result.id = prop->prop_id;
      result.value = props->prop_values[i];
      drmModeFreeProperty(prop);
      break;
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
  return result;
}

uint64_t getDrmObjectPropRangeMax(int fd, uint32_t objectId,
                                  uint32_t objectType, const char *name,
                                  uint64_t fallback) {
  drmModeObjectPropertiesPtr props =
      drmModeObjectGetProperties(fd, objectId, objectType);
  if (!props) {
    return fallback;
  }
  uint64_t result = fallback;
  for (uint32_t i = 0; i < props->count_props; ++i) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop) {
      continue;
    }
    if (strcmp(prop->name, name) == 0) {
      if ((prop->flags & DRM_MODE_PROP_RANGE) && prop->values &&
          prop->count_values >= 2) {
        result = prop->values[1];
      } else {
        result = props->prop_values[i];
      }
      drmModeFreeProperty(prop);
      break;
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
  return result;
}

bool createDrmModeBlob(int fd, const drmModeModeInfo &mode,
                       uint32_t *outBlobId) {
  if (!outBlobId) {
    return false;
  }
  drm_mode_create_blob blob{};
  blob.data = reinterpret_cast<uint64_t>(&mode);
  blob.length = sizeof(mode);
  if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) != 0 ||
      blob.blob_id == 0) {
    return false;
  }
  *outBlobId = blob.blob_id;
  return true;
}

void destroyDrmModeBlob(int fd, uint32_t blobId) {
  if (fd < 0 || blobId == 0) {
    return;
  }
  drm_mode_destroy_blob blob{};
  blob.blob_id = blobId;
  ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob);
}

int findDrmCrtcIndex(drmModeResPtr res, uint32_t crtcId) {
  if (!res) {
    return -1;
  }
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtcId) {
      return i;
    }
  }
  return -1;
}

uint32_t findDrmCrtcForConnector(int fd, drmModeResPtr res,
                                 drmModeConnectorPtr connector) {
  if (!res || !connector) {
    return 0;
  }
  if (connector->encoder_id != 0) {
    drmModeEncoderPtr encoder = drmModeGetEncoder(fd, connector->encoder_id);
    if (encoder) {
      const uint32_t crtcId = encoder->crtc_id;
      drmModeFreeEncoder(encoder);
      if (crtcId != 0) {
        return crtcId;
      }
    }
  }
  for (int i = 0; i < connector->count_encoders; ++i) {
    drmModeEncoderPtr encoder = drmModeGetEncoder(fd, connector->encoders[i]);
    if (!encoder) {
      continue;
    }
    for (int crtcIndex = 0; crtcIndex < res->count_crtcs; ++crtcIndex) {
      if (encoder->possible_crtcs & (1 << crtcIndex)) {
        const uint32_t crtcId = res->crtcs[crtcIndex];
        drmModeFreeEncoder(encoder);
        return crtcId;
      }
    }
    drmModeFreeEncoder(encoder);
  }
  return 0;
}

std::string drmPlaneTypeName(uint64_t type) {
  switch (type) {
  case DRM_PLANE_TYPE_PRIMARY:
    return "Primary";
  case DRM_PLANE_TYPE_OVERLAY:
    return "Overlay";
  case DRM_PLANE_TYPE_CURSOR:
    return "Cursor";
  default:
    return "unknown";
  }
}

std::string getDrmPlaneType(int fd, uint32_t planeId) {
  const DrmObjectProp type =
      getDrmObjectProp(fd, planeId, DRM_MODE_OBJECT_PLANE, "type");
  return type.id ? drmPlaneTypeName(type.value) : "unknown";
}

uint32_t findDrmPrimaryPlane(int fd, int crtcIndex, uint32_t requiredFormat) {
  drmModePlaneResPtr planeRes = drmModeGetPlaneResources(fd);
  if (!planeRes || crtcIndex < 0) {
    return 0;
  }
  const uint32_t crtcMask = 1u << static_cast<uint32_t>(crtcIndex);
  uint32_t overlay = 0;
  uint32_t primary = 0;
  uint32_t fallback = 0;
  for (uint32_t i = 0; i < planeRes->count_planes; ++i) {
    drmModePlanePtr plane = drmModeGetPlane(fd, planeRes->planes[i]);
    if (!plane) {
      continue;
    }
    const bool possible = (plane->possible_crtcs & crtcMask) != 0;
    bool formatOk = false;
    for (uint32_t f = 0; f < plane->count_formats; ++f) {
      if (plane->formats[f] == requiredFormat) {
        formatOk = true;
        break;
      }
    }
    const std::string type = getDrmPlaneType(fd, plane->plane_id);
    if (possible && formatOk && type == "Overlay" && overlay == 0) {
      overlay = plane->plane_id;
    }
    if (possible && formatOk && type == "Primary" && primary == 0) {
      primary = plane->plane_id;
    }
    if (possible && formatOk && fallback == 0) {
      fallback = plane->plane_id;
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(planeRes);
  return overlay != 0 ? overlay : (primary != 0 ? primary : fallback);
}

void cleanupDrmFb(int drmFd, uint32_t fbId, uint32_t gemHandle) {
  if (fbId != 0) {
    unsigned int fb = fbId;
    ioctl(drmFd, DRM_IOCTL_MODE_RMFB, &fb);
  }
  if (gemHandle != 0) {
    drm_gem_close gemClose{};
    gemClose.handle = gemHandle;
    ioctl(drmFd, DRM_IOCTL_GEM_CLOSE, &gemClose);
  }
}

void destroyDumbBuffer(int drmFd, uint32_t handle) {
  if (handle == 0) {
    return;
  }
  drm_mode_destroy_dumb destroy{};
  destroy.handle = handle;
  ioctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

VkRenderPass createProbeRenderPass(VkDevice device, VkFormat format) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &color;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  if (vkCreateRenderPass(device, &info, nullptr, &renderPass) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return renderPass;
}

void queryRockchipGrallocStride(const NativeHandleLayout *handle,
                                AhbNativeHandleInfo &info) {
  void *libGralloc = dlopen("libgralloc_priv_omx.so", RTLD_NOW | RTLD_LOCAL);
  if (!libGralloc) {
    libGralloc =
        dlopen("/vendor/lib64/libgralloc_priv_omx.so", RTLD_NOW | RTLD_LOCAL);
  }
  if (!libGralloc) {
    LOG_WARN("[DrmKmsAhbProbe] dlopen libgralloc_priv_omx.so failed: %s",
             dlerror());
    return;
  }

  using GetStrideFn = int (*)(const NativeHandleLayout *, int *);
  auto getPixelStride =
      reinterpret_cast<GetStrideFn>(dlsym(libGralloc, "get_pixel_stride"));
  auto getByteStride =
      reinterpret_cast<GetStrideFn>(dlsym(libGralloc, "get_byte_stride"));

  int value = 0;
  if (getPixelStride && getPixelStride(handle, &value) == 0) {
    info.pixelStride = value;
  }
  value = 0;
  if (getByteStride && getByteStride(handle, &value) == 0) {
    info.byteStride = value;
  }
  LOG_WARN("[DrmKmsAhbProbe] gralloc stride pixel=%d byte=%d "
           "symbols pixel=%d byte=%d",
           info.pixelStride, info.byteStride, getPixelStride ? 1 : 0,
           getByteStride ? 1 : 0);
  dlclose(libGralloc);
}

AhbNativeHandleInfo inspectAhbNativeHandle(AHardwareBuffer *buffer) {
  AhbNativeHandleInfo info{};
  if (!buffer) {
    return info;
  }

  using GetNativeHandleFn = const NativeHandleLayout *(*)(AHardwareBuffer *);
  void *libAndroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (!libAndroid) {
    LOG_WARN("[DrmKmsAhbProbe] dlopen libandroid.so failed: %s", dlerror());
    return info;
  }

  auto getNativeHandle = reinterpret_cast<GetNativeHandleFn>(
      dlsym(libAndroid, "AHardwareBuffer_getNativeHandle"));
  if (!getNativeHandle) {
    LOG_WARN("[DrmKmsAhbProbe] AHardwareBuffer_getNativeHandle unavailable");
    dlclose(libAndroid);
    return info;
  }

  const NativeHandleLayout *handle = getNativeHandle(buffer);
  if (!handle || handle->numFds <= 0) {
    LOG_WARN("[DrmKmsAhbProbe] native handle missing fds handle=%p",
             static_cast<const void *>(handle));
    dlclose(libAndroid);
    return info;
  }

  for (int i = 0; i < handle->numFds; ++i) {
    const int dupFd = dup(handle->data[i]);
    if (dupFd >= 0) {
      info.dupFds.push_back(dupFd);
    }
  }
  for (int i = 0; i < handle->numInts; ++i) {
    info.ints.push_back(handle->data[handle->numFds + i]);
  }
  const int dumpCount = std::min<int>(handle->numInts, 24);
  char intDump[512] = {};
  size_t offset = 0;
  for (int i = 0; i < dumpCount && offset < sizeof(intDump); ++i) {
    const int written =
        snprintf(intDump + offset, sizeof(intDump) - offset, "%s%d",
                 i == 0 ? "" : ",", info.ints[static_cast<size_t>(i)]);
    if (written <= 0) {
      break;
    }
    offset += static_cast<size_t>(written);
  }
  LOG_WARN("[DrmKmsAhbProbe] native handle fds=%d ints=%d dupFds=%zu "
           "ints[0..%d]=[%s]",
           handle->numFds, handle->numInts, info.dupFds.size(), dumpCount - 1,
           intDump);
  queryRockchipGrallocStride(handle, info);
  dlclose(libAndroid);
  return info;
}

bool tryCreateDrmFbFromAhb(int dmaBufFd, uint32_t width, uint32_t height,
                           uint32_t pitchBytes, uint32_t drmFormat,
                           uint64_t modifier) {
  if (dmaBufFd < 0 || width == 0 || height == 0 || pitchBytes == 0) {
    return false;
  }

  int drmFd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (drmFd < 0) {
    LOG_WARN("[DrmKmsAhbProbe] open card0 failed: %s", strerror(errno));
    return false;
  }

  drm_prime_handle prime{};
  prime.fd = dmaBufFd;
  if (ioctl(drmFd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) != 0 ||
      prime.handle == 0) {
    LOG_WARN("[DrmKmsAhbProbe] PRIME_FD_TO_HANDLE failed: %s",
             strerror(errno));
    close(drmFd);
    return false;
  }

  drm_mode_fb_cmd2 fb{};
  fb.width = width;
  fb.height = height;
  fb.pixel_format = drmFormat;
  fb.handles[0] = prime.handle;
  fb.pitches[0] = pitchBytes;
  fb.offsets[0] = 0;
  fb.flags = DRM_MODE_FB_MODIFIERS;
  fb.modifier[0] = modifier;
  if (ioctl(drmFd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0 || fb.fb_id == 0) {
    LOG_WARN("[DrmKmsAhbProbe] ADDFB2 modifier failed format=0x%x "
             "%ux%u pitch=%u modifier=0x%llx: %s",
             drmFormat, width, height, fb.pitches[0],
             static_cast<unsigned long long>(modifier), strerror(errno));
    fb.flags = 0;
    fb.modifier[0] = 0;
    if (ioctl(drmFd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0 || fb.fb_id == 0) {
      LOG_WARN("[DrmKmsAhbProbe] ADDFB2 no-modifier failed format=0x%x "
               "%ux%u pitch=%u: %s",
               drmFormat, width, height, fb.pitches[0], strerror(errno));
      cleanupDrmFb(drmFd, 0, prime.handle);
      close(drmFd);
      return false;
    }
  }

  LOG_WARN("[DrmKmsAhbProbe] DRM FB success format=0x%x %ux%u pitch=%u "
           "modifier=0x%llx gem=%u fb=%u",
           drmFormat, width, height, fb.pitches[0],
           static_cast<unsigned long long>(modifier), prime.handle, fb.fb_id);
  cleanupDrmFb(drmFd, fb.fb_id, prime.handle);
  close(drmFd);
  return true;
}

bool createPersistentDrmFbFromAhb(int drmFd, int dmaBufFd, uint32_t width,
                                  uint32_t height, uint32_t pitchBytes,
                                  uint32_t drmFormat, uint32_t *outGem,
                                  uint32_t *outFb) {
  if (drmFd < 0 || dmaBufFd < 0 || !outGem || !outFb || pitchBytes == 0) {
    return false;
  }
  drm_prime_handle prime{};
  prime.fd = dmaBufFd;
  if (ioctl(drmFd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) != 0 ||
      prime.handle == 0) {
    LOG_WARN("[DrmKmsPresenter] PRIME_FD_TO_HANDLE failed: %s",
             strerror(errno));
    return false;
  }

  drm_mode_fb_cmd2 fb{};
  fb.width = width;
  fb.height = height;
  fb.pixel_format = drmFormat;
  fb.handles[0] = prime.handle;
  fb.pitches[0] = pitchBytes;
  fb.offsets[0] = 0;
  fb.flags = DRM_MODE_FB_MODIFIERS;
  fb.modifier[0] = DRM_FORMAT_MOD_LINEAR;
  if (ioctl(drmFd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0 || fb.fb_id == 0) {
    fb.flags = 0;
    fb.modifier[0] = 0;
    if (ioctl(drmFd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0 || fb.fb_id == 0) {
      LOG_WARN("[DrmKmsPresenter] ADDFB2 failed %ux%u pitch=%u fmt=0x%x: %s",
               width, height, pitchBytes, drmFormat, strerror(errno));
      cleanupDrmFb(drmFd, 0, prime.handle);
      return false;
    }
  }
  *outGem = prime.handle;
  *outFb = fb.fb_id;
  return true;
}

bool tryCreateDrmFbCandidates(const AhbNativeHandleInfo &handleInfo,
                              uint32_t width, uint32_t height,
                              uint32_t descStridePixels,
                              uint32_t drmFormat) {
  std::vector<uint32_t> pitches;
  auto addPitch = [&](uint32_t pitch) {
    if (pitch >= width * 4 &&
        std::find(pitches.begin(), pitches.end(), pitch) == pitches.end()) {
      pitches.push_back(pitch);
    }
  };
  addPitch(descStridePixels * 4);
  addPitch(static_cast<uint32_t>(handleInfo.pixelStride) * 4);
  addPitch(static_cast<uint32_t>(handleInfo.byteStride));
  addPitch(width * 4);
  addPitch(((width + 15u) & ~15u) * 4);
  addPitch(((width + 63u) & ~63u) * 4);

  std::vector<uint64_t> modifiers = {
      DRM_FORMAT_MOD_LINEAR,
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_SPARSE),
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_SPLIT |
                              AFBC_FORMAT_MOD_SPARSE),
  };

  bool anySuccess = false;
  for (size_t fdIndex = 0; fdIndex < handleInfo.dupFds.size(); ++fdIndex) {
    for (uint32_t pitch : pitches) {
      for (uint64_t modifier : modifiers) {
        LOG_INFO("[DrmKmsAhbProbe] try DRM FB fdIndex=%zu fd=%d pitch=%u "
                 "modifier=0x%llx",
                 fdIndex, handleInfo.dupFds[fdIndex], pitch,
                 static_cast<unsigned long long>(modifier));
        if (tryCreateDrmFbFromAhb(handleInfo.dupFds[fdIndex], width, height,
                                  pitch, drmFormat, modifier)) {
          anySuccess = true;
        }
      }
    }
  }
  return anySuccess;
}

} // namespace

void VulkanRenderer::runDrmKmsExportProbeIfRequested() {
  if (!propEnabled("debug.hsvj.drm_kms_export_probe")) {
    return;
  }

  LOG_WARN("[DrmKmsExportProbe] start");
  if (device_ == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE ||
      swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
    LOG_WARN("[DrmKmsExportProbe] skipped: renderer not ready device=%p "
             "physical=%p extent=%ux%u",
             (void *)device_, (void *)physicalDevice_, swapchainExtent_.width,
             swapchainExtent_.height);
    return;
  }

  auto getMemoryFd =
      reinterpret_cast<PFN_vkGetMemoryFdKHR>(
          vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
  auto getImageModifier =
      reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
          vkGetDeviceProcAddr(device_,
                              "vkGetImageDrmFormatModifierPropertiesEXT"));
  auto getFormatProperties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
          vkGetInstanceProcAddr(instance_,
                                "vkGetPhysicalDeviceFormatProperties2"));
  if (!getMemoryFd) {
    LOG_WARN("[DrmKmsExportProbe] vkGetMemoryFdKHR unavailable");
    return;
  }
  if (!getImageModifier) {
    LOG_WARN("[DrmKmsExportProbe] "
             "vkGetImageDrmFormatModifierPropertiesEXT unavailable");
    return;
  }
  if (!getFormatProperties2) {
    LOG_WARN("[DrmKmsExportProbe] "
             "vkGetPhysicalDeviceFormatProperties2 unavailable");
    return;
  }

  const VkFormat vkFormat = VK_FORMAT_B8G8R8A8_UNORM;
  VkDrmFormatModifierPropertiesListEXT modifierList{
      VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 props2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  props2.pNext = &modifierList;
  getFormatProperties2(physicalDevice_, vkFormat, &props2);
  if (modifierList.drmFormatModifierCount == 0) {
    LOG_WARN("[DrmKmsExportProbe] no DRM modifiers for B8G8R8A8");
    return;
  }

  std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(
      modifierList.drmFormatModifierCount);
  modifierList.pDrmFormatModifierProperties = modifierProps.data();
  getFormatProperties2(physicalDevice_, vkFormat, &props2);

  std::vector<uint64_t> candidateModifiers;
  for (const auto &mod : modifierProps) {
    if ((mod.drmFormatModifierTilingFeatures &
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0 &&
        mod.drmFormatModifierPlaneCount == 1) {
      candidateModifiers.push_back(mod.drmFormatModifier);
    }
  }
  if (candidateModifiers.empty()) {
    LOG_WARN("[DrmKmsExportProbe] no one-plane COLOR_ATTACHMENT modifier "
             "for B8G8R8A8 (count=%u)",
             modifierList.drmFormatModifierCount);
    return;
  }

  VkImageDrmFormatModifierListCreateInfoEXT modifierCreate{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
  modifierCreate.drmFormatModifierCount =
      static_cast<uint32_t>(candidateModifiers.size());
  modifierCreate.pDrmFormatModifiers = candidateModifiers.data();

  VkExternalMemoryImageCreateInfo externalImage{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  externalImage.pNext = &modifierCreate;
  externalImage.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.pNext = &externalImage;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = vkFormat;
  imageInfo.extent = {swapchainExtent_.width, swapchainExtent_.height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &image);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsExportProbe] modifier vkCreateImage failed result=%d "
             "candidateModifiers=%zu",
             result, candidateModifiers.size());
    return;
  }

  VkMemoryRequirements memReq{};
  vkGetImageMemoryRequirements(device_, image, &memReq);

  VkMemoryDedicatedAllocateInfo dedicated{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = image;
  VkExportMemoryAllocateInfo exportInfo{
      VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  dedicated.pNext = &exportInfo;

  const uint32_t memoryType =
      findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memoryType == UINT32_MAX) {
    LOG_WARN("[DrmKmsExportProbe] no DEVICE_LOCAL memory type bits=0x%x",
             memReq.memoryTypeBits);
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.pNext = &dedicated;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = memoryType;

  VkDeviceMemory memory = VK_NULL_HANDLE;
  result = vkAllocateMemory(device_, &allocInfo, nullptr, &memory);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsExportProbe] vkAllocateMemory failed result=%d size=%llu",
             result, static_cast<unsigned long long>(memReq.size));
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  result = vkBindImageMemory(device_, image, memory, 0);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsExportProbe] vkBindImageMemory failed result=%d", result);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  VkImageSubresource subresource{};
  subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  VkSubresourceLayout layout{};
  vkGetImageSubresourceLayout(device_, image, &subresource, &layout);
  VkImageDrmFormatModifierPropertiesEXT imageModifier{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
  result = getImageModifier(device_, image, &imageModifier);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsExportProbe] get image modifier failed result=%d", result);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  int dmaBufFd = -1;
  VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  fdInfo.memory = memory;
  fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  result = getMemoryFd(device_, &fdInfo, &dmaBufFd);
  if (result != VK_SUCCESS || dmaBufFd < 0) {
    LOG_WARN("[DrmKmsExportProbe] vkGetMemoryFdKHR failed result=%d fd=%d",
             result, dmaBufFd);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  int drmFd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (drmFd < 0) {
    LOG_WARN("[DrmKmsExportProbe] open card0 failed: %s", strerror(errno));
    close(dmaBufFd);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  uint64_t primeCap = 0;
  if (drmGetCapValue(drmFd, DRM_CAP_PRIME, &primeCap)) {
    LOG_INFO("[DrmKmsExportProbe] DRM_CAP_PRIME=0x%llx",
             static_cast<unsigned long long>(primeCap));
  }

  drm_prime_handle prime{};
  prime.fd = dmaBufFd;
  if (ioctl(drmFd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) != 0 ||
      prime.handle == 0) {
    LOG_WARN("[DrmKmsExportProbe] PRIME_FD_TO_HANDLE failed: %s",
             strerror(errno));
    close(drmFd);
    close(dmaBufFd);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  drm_mode_fb_cmd2 fb{};
  fb.width = swapchainExtent_.width;
  fb.height = swapchainExtent_.height;
  fb.pixel_format = DRM_FORMAT_ARGB8888;
  fb.flags = DRM_MODE_FB_MODIFIERS;
  fb.handles[0] = prime.handle;
  fb.pitches[0] = static_cast<uint32_t>(layout.rowPitch);
  fb.offsets[0] = static_cast<uint32_t>(layout.offset);
  fb.modifier[0] = imageModifier.drmFormatModifier;

  if (ioctl(drmFd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0 || fb.fb_id == 0) {
    LOG_WARN("[DrmKmsExportProbe] ADDFB2 failed %ux%u pitch=%u offset=%u "
             "modifier=0x%llx: %s",
             fb.width, fb.height, fb.pitches[0], fb.offsets[0],
             static_cast<unsigned long long>(fb.modifier[0]), strerror(errno));
    cleanupDrmFb(drmFd, 0, prime.handle);
    close(drmFd);
    close(dmaBufFd);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    return;
  }

  LOG_WARN("[DrmKmsExportProbe] success image=%ux%u rowPitch=%llu fd=%d "
           "gem=%u fb=%u modifier=0x%llx memorySize=%llu",
           swapchainExtent_.width, swapchainExtent_.height,
           static_cast<unsigned long long>(layout.rowPitch), dmaBufFd,
           prime.handle, fb.fb_id,
           static_cast<unsigned long long>(imageModifier.drmFormatModifier),
           static_cast<unsigned long long>(memReq.size));

  cleanupDrmFb(drmFd, fb.fb_id, prime.handle);
  close(drmFd);
  close(dmaBufFd);
  vkFreeMemory(device_, memory, nullptr);
  vkDestroyImage(device_, image, nullptr);
}

void VulkanRenderer::runDrmKmsImportProbeIfRequested() {
  if (!propEnabled("debug.hsvj.drm_kms_import_probe")) {
    return;
  }

  LOG_WARN("[DrmKmsImportProbe] start");
  if (device_ == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE ||
      swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
    LOG_WARN("[DrmKmsImportProbe] skipped: renderer not ready");
    return;
  }

  int drmFd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (drmFd < 0) {
    LOG_WARN("[DrmKmsImportProbe] open card0 failed: %s", strerror(errno));
    return;
  }

  drm_mode_create_dumb create{};
  create.width = swapchainExtent_.width;
  create.height = swapchainExtent_.height;
  create.bpp = 32;
  if (ioctl(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0 ||
      create.handle == 0) {
    LOG_WARN("[DrmKmsImportProbe] CREATE_DUMB failed %ux%u: %s",
             create.width, create.height, strerror(errno));
    close(drmFd);
    return;
  }

  drm_mode_fb_cmd2 fb{};
  fb.width = create.width;
  fb.height = create.height;
  fb.pixel_format = DRM_FORMAT_ARGB8888;
  fb.flags = DRM_MODE_FB_MODIFIERS;
  fb.handles[0] = create.handle;
  fb.pitches[0] = create.pitch;
  fb.offsets[0] = 0;
  fb.modifier[0] = DRM_FORMAT_MOD_LINEAR;
  if (ioctl(drmFd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0 || fb.fb_id == 0) {
    LOG_WARN("[DrmKmsImportProbe] ADDFB2 dumb failed pitch=%u size=%llu: %s",
             create.pitch, static_cast<unsigned long long>(create.size),
             strerror(errno));
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  drm_prime_handle prime{};
  prime.handle = create.handle;
  prime.flags = O_CLOEXEC;
  if (ioctl(drmFd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0 ||
      prime.fd < 0) {
    LOG_WARN("[DrmKmsImportProbe] PRIME_HANDLE_TO_FD failed: %s",
             strerror(errno));
    cleanupDrmFb(drmFd, fb.fb_id, 0);
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  VkSubresourceLayout planeLayout{};
  planeLayout.offset = 0;
  planeLayout.size = create.size;
  planeLayout.rowPitch = create.pitch;
  VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  modifierInfo.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
  modifierInfo.drmFormatModifierPlaneCount = 1;
  modifierInfo.pPlaneLayouts = &planeLayout;

  VkExternalMemoryImageCreateInfo externalImage{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  externalImage.pNext = &modifierInfo;
  externalImage.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  const VkFormat vkFormat = VK_FORMAT_B8G8R8A8_UNORM;
  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.pNext = &externalImage;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = vkFormat;
  imageInfo.extent = {create.width, create.height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &image);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsImportProbe] vkCreateImage import failed result=%d",
             result);
    close(prime.fd);
    cleanupDrmFb(drmFd, fb.fb_id, 0);
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  VkMemoryRequirements memReq{};
  vkGetImageMemoryRequirements(device_, image, &memReq);
  VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  importInfo.fd = prime.fd;
  VkMemoryDedicatedAllocateInfo dedicated{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.pNext = &importInfo;
  dedicated.image = image;

  const uint32_t memoryType =
      findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memoryType == UINT32_MAX) {
    LOG_WARN("[DrmKmsImportProbe] no memory type for imported dumb bits=0x%x",
             memReq.memoryTypeBits);
    vkDestroyImage(device_, image, nullptr);
    close(prime.fd);
    cleanupDrmFb(drmFd, fb.fb_id, 0);
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.pNext = &dedicated;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = memoryType;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  result = vkAllocateMemory(device_, &allocInfo, nullptr, &memory);
  // Ownership of prime.fd transfers to Vulkan on success.
  if (result == VK_SUCCESS) {
    prime.fd = -1;
  }
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsImportProbe] vkAllocateMemory import failed result=%d",
             result);
    vkDestroyImage(device_, image, nullptr);
    if (prime.fd >= 0) {
      close(prime.fd);
    }
    cleanupDrmFb(drmFd, fb.fb_id, 0);
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  result = vkBindImageMemory(device_, image, memory, 0);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsImportProbe] vkBindImageMemory import failed result=%d",
             result);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    cleanupDrmFb(drmFd, fb.fb_id, 0);
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = vkFormat;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  VkImageView imageView = VK_NULL_HANDLE;
  result = vkCreateImageView(device_, &viewInfo, nullptr, &imageView);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsImportProbe] vkCreateImageView failed result=%d", result);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    cleanupDrmFb(drmFd, fb.fb_id, 0);
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  VkRenderPass renderPass = createProbeRenderPass(device_, vkFormat);
  if (renderPass == VK_NULL_HANDLE) {
    LOG_WARN("[DrmKmsImportProbe] probe render pass creation failed");
    vkDestroyImageView(device_, imageView, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    cleanupDrmFb(drmFd, fb.fb_id, 0);
    destroyDumbBuffer(drmFd, create.handle);
    close(drmFd);
    return;
  }

  VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbInfo.renderPass = renderPass;
  fbInfo.attachmentCount = 1;
  fbInfo.pAttachments = &imageView;
  fbInfo.width = create.width;
  fbInfo.height = create.height;
  fbInfo.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  result = vkCreateFramebuffer(device_, &fbInfo, nullptr, &framebuffer);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsImportProbe] vkCreateFramebuffer failed result=%d",
             result);
  } else {
    LOG_WARN("[DrmKmsImportProbe] success dumb=%ux%u pitch=%u size=%llu "
             "drmFb=%u gem=%u vkMem=%llu",
             create.width, create.height, create.pitch,
             static_cast<unsigned long long>(create.size), fb.fb_id,
             create.handle, static_cast<unsigned long long>(memReq.size));
  }

  if (framebuffer != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(device_, framebuffer, nullptr);
  }
  vkDestroyRenderPass(device_, renderPass, nullptr);
  vkDestroyImageView(device_, imageView, nullptr);
  vkFreeMemory(device_, memory, nullptr);
  vkDestroyImage(device_, image, nullptr);
  cleanupDrmFb(drmFd, fb.fb_id, 0);
  destroyDumbBuffer(drmFd, create.handle);
  close(drmFd);
}

void VulkanRenderer::runDrmKmsAhbProbeIfRequested() {
  if (!propEnabled("debug.hsvj.drm_kms_ahb_probe")) {
    return;
  }

  LOG_WARN("[DrmKmsAhbProbe] start");
  if (device_ == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE ||
      swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
    LOG_WARN("[DrmKmsAhbProbe] skipped: renderer not ready");
    return;
  }

  auto getAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
      vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
  if (!getAhbProps) {
    LOG_WARN("[DrmKmsAhbProbe] vkGetAndroidHardwareBufferPropertiesANDROID "
             "unavailable");
    return;
  }

  const uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                         AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
  AHardwareBuffer_Desc desc{};
  desc.width = swapchainExtent_.width;
  desc.height = swapchainExtent_.height;
  desc.layers = 1;
  desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  desc.usage = usage;

  AHardwareBuffer *buffer = nullptr;
  int allocResult = AHardwareBuffer_allocate(&desc, &buffer);
  if (allocResult != 0 || !buffer) {
    LOG_WARN("[DrmKmsAhbProbe] AHardwareBuffer_allocate RGBA failed result=%d "
             "%ux%u usage=0x%llx",
             allocResult, desc.width, desc.height,
             static_cast<unsigned long long>(usage));
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
    allocResult = AHardwareBuffer_allocate(&desc, &buffer);
  }
  if (allocResult != 0 || !buffer) {
    LOG_WARN("[DrmKmsAhbProbe] AHardwareBuffer_allocate RGBX failed result=%d "
             "%ux%u usage=0x%llx",
             allocResult, desc.width, desc.height,
             static_cast<unsigned long long>(usage));
    return;
  }

  AHardwareBuffer_Desc actualDesc{};
  AHardwareBuffer_describe(buffer, &actualDesc);
  LOG_WARN("[DrmKmsAhbProbe] allocated ahb=%p %ux%u stride=%u format=%u "
           "usage=0x%llx",
           static_cast<void *>(buffer), actualDesc.width, actualDesc.height,
           actualDesc.stride, actualDesc.format,
           static_cast<unsigned long long>(actualDesc.usage));

  VkAndroidHardwareBufferFormatPropertiesANDROID formatProps{
      VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
  VkAndroidHardwareBufferPropertiesANDROID props{
      VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
  props.pNext = &formatProps;
  VkResult result = getAhbProps(device_, buffer, &props);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsAhbProbe] get AHB props failed result=%d", result);
    AHardwareBuffer_release(buffer);
    return;
  }

  const VkFormat vkFormat = formatProps.format != VK_FORMAT_UNDEFINED
                                ? formatProps.format
                                : VK_FORMAT_R8G8B8A8_UNORM;
  if (formatProps.format == VK_FORMAT_UNDEFINED) {
    LOG_WARN("[DrmKmsAhbProbe] external-only format unsupported for final "
             "color attachment externalFormat=%llu",
             static_cast<unsigned long long>(formatProps.externalFormat));
    AHardwareBuffer_release(buffer);
    return;
  }
  LOG_WARN("[DrmKmsAhbProbe] vk props allocation=%llu memoryBits=0x%x "
           "vkFormat=%d externalFormat=%llu formatFeatures=0x%llx",
           static_cast<unsigned long long>(props.allocationSize),
           props.memoryTypeBits, static_cast<int>(vkFormat),
           static_cast<unsigned long long>(formatProps.externalFormat),
           static_cast<unsigned long long>(formatProps.formatFeatures));

  VkExternalMemoryImageCreateInfo externalImage{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  externalImage.handleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.pNext = &externalImage;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = vkFormat;
  imageInfo.extent = {actualDesc.width, actualDesc.height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  result = vkCreateImage(device_, &imageInfo, nullptr, &image);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsAhbProbe] vkCreateImage failed result=%d", result);
    AHardwareBuffer_release(buffer);
    return;
  }

  VkImportAndroidHardwareBufferInfoANDROID importInfo{
      VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
  importInfo.buffer = buffer;
  VkMemoryDedicatedAllocateInfo dedicated{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.pNext = &importInfo;
  dedicated.image = image;

  const uint32_t memoryType =
      findMemoryType(props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memoryType == UINT32_MAX) {
    LOG_WARN("[DrmKmsAhbProbe] no DEVICE_LOCAL memory type bits=0x%x",
             props.memoryTypeBits);
    vkDestroyImage(device_, image, nullptr);
    AHardwareBuffer_release(buffer);
    return;
  }

  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.pNext = &dedicated;
  allocInfo.allocationSize = props.allocationSize;
  allocInfo.memoryTypeIndex = memoryType;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  result = vkAllocateMemory(device_, &allocInfo, nullptr, &memory);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsAhbProbe] vkAllocateMemory failed result=%d size=%llu",
             result, static_cast<unsigned long long>(props.allocationSize));
    vkDestroyImage(device_, image, nullptr);
    AHardwareBuffer_release(buffer);
    return;
  }

  result = vkBindImageMemory(device_, image, memory, 0);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsAhbProbe] vkBindImageMemory failed result=%d", result);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    AHardwareBuffer_release(buffer);
    return;
  }

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = vkFormat;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  VkImageView imageView = VK_NULL_HANDLE;
  result = vkCreateImageView(device_, &viewInfo, nullptr, &imageView);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsAhbProbe] vkCreateImageView failed result=%d", result);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    AHardwareBuffer_release(buffer);
    return;
  }

  VkRenderPass renderPass = createProbeRenderPass(device_, vkFormat);
  if (renderPass == VK_NULL_HANDLE) {
    LOG_WARN("[DrmKmsAhbProbe] render pass creation failed");
    vkDestroyImageView(device_, imageView, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    AHardwareBuffer_release(buffer);
    return;
  }

  VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbInfo.renderPass = renderPass;
  fbInfo.attachmentCount = 1;
  fbInfo.pAttachments = &imageView;
  fbInfo.width = actualDesc.width;
  fbInfo.height = actualDesc.height;
  fbInfo.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  result = vkCreateFramebuffer(device_, &fbInfo, nullptr, &framebuffer);
  if (result != VK_SUCCESS) {
    LOG_WARN("[DrmKmsAhbProbe] vkCreateFramebuffer failed result=%d", result);
  } else {
    LOG_WARN("[DrmKmsAhbProbe] Vulkan framebuffer success %ux%u vkFormat=%d",
             actualDesc.width, actualDesc.height, static_cast<int>(vkFormat));
  }

  AhbNativeHandleInfo handleInfo = inspectAhbNativeHandle(buffer);
  if (!handleInfo.dupFds.empty()) {
    const uint32_t drmFormat =
        actualDesc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
            ? DRM_FORMAT_XBGR8888
            : DRM_FORMAT_ABGR8888;
    const bool fbOk = tryCreateDrmFbCandidates(
        handleInfo, actualDesc.width, actualDesc.height, actualDesc.stride,
        drmFormat);
    LOG_WARN("[DrmKmsAhbProbe] DRM FB candidate result=%s",
             fbOk ? "success" : "failed");
    for (int fd : handleInfo.dupFds) {
      close(fd);
    }
  } else {
    LOG_WARN("[DrmKmsAhbProbe] no dma-buf fd from AHB; app-side KMS FB "
             "creation needs gralloc/native-handle integration");
  }

  if (framebuffer != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(device_, framebuffer, nullptr);
  }
  vkDestroyRenderPass(device_, renderPass, nullptr);
  vkDestroyImageView(device_, imageView, nullptr);
  vkFreeMemory(device_, memory, nullptr);
  vkDestroyImage(device_, image, nullptr);
  AHardwareBuffer_release(buffer);
}

bool VulkanRenderer::initializeDrmKmsPresenterIfRequested() {
  drmKmsPresenter_.requested = drmKmsPresenterRequested();
  if (!drmKmsPresenter_.requested) {
    return false;
  }
  if (drmKmsPresenter_.initialized) {
    return true;
  }
  if (device_ == VK_NULL_HANDLE || renderPass_ == VK_NULL_HANDLE ||
      swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
    LOG_WARN("[DrmKmsPresenter] skipped: renderer not ready");
    return false;
  }

  DrmKmsPresenterState state{};
  state.requested = true;
  state.width = swapchainExtent_.width;
  state.height = swapchainExtent_.height;
  state.drmFd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (state.drmFd < 0) {
    LOG_WARN("[DrmKmsPresenter] open card0 failed: %s", strerror(errno));
    return false;
  }

  drmSetClientCapValue(state.drmFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCapValue(state.drmFd, DRM_CLIENT_CAP_ATOMIC, 1);

  drmModeResPtr res = drmModeGetResources(state.drmFd);
  if (!res) {
    LOG_WARN("[DrmKmsPresenter] drmModeGetResources failed: %s",
             strerror(errno));
    close(state.drmFd);
    return false;
  }

  drmModeConnectorPtr selectedConnector = nullptr;
  drmModeModeInfo selectedMode{};
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnectorPtr connector =
        drmModeGetConnector(state.drmFd, res->connectors[i]);
    if (!connector) {
      continue;
    }
    if (connector->connection == DRM_MODE_CONNECTED &&
        connector->count_modes > 0) {
      selectedConnector = connector;
      selectedMode = connector->modes[0];
      for (int m = 0; m < connector->count_modes; ++m) {
        const drmModeModeInfo &mode = connector->modes[m];
        if (mode.hdisplay == state.width && mode.vdisplay == state.height) {
          selectedMode = mode;
          break;
        }
      }
      break;
    }
    drmModeFreeConnector(connector);
  }
  if (!selectedConnector) {
    LOG_WARN("[DrmKmsPresenter] no connected connector");
    drmModeFreeResources(res);
    close(state.drmFd);
    return false;
  }

  // The DRM mode is the physical output size. The logical canvas remains
  // controlled by SystemConfig; only the final composed image uses this size.
  state.width = selectedMode.hdisplay;
  state.height = selectedMode.vdisplay;
  swapchainExtent_.width = state.width;
  swapchainExtent_.height = state.height;

  state.connectorId = selectedConnector->connector_id;
  state.crtcId = findDrmCrtcForConnector(state.drmFd, res, selectedConnector);
  state.crtcIndex = findDrmCrtcIndex(res, state.crtcId);
  state.planeId =
      findDrmPrimaryPlane(state.drmFd, state.crtcIndex, DRM_FORMAT_ABGR8888);
  if (state.crtcId == 0 || state.crtcIndex < 0 || state.planeId == 0) {
    LOG_WARN("[DrmKmsPresenter] missing crtc/plane connector=%u crtc=%u "
             "crtcIndex=%d plane=%u",
             state.connectorId, state.crtcId, state.crtcIndex, state.planeId);
    drmModeFreeConnector(selectedConnector);
    drmModeFreeResources(res);
    close(state.drmFd);
    return false;
  }

  if (!createDrmModeBlob(state.drmFd, selectedMode, &state.modeBlobId)) {
    LOG_WARN("[DrmKmsPresenter] create mode blob failed: %s", strerror(errno));
    drmModeFreeConnector(selectedConnector);
    drmModeFreeResources(res);
    close(state.drmFd);
    return false;
  }

  state.propCrtcActive =
      getDrmObjectProp(state.drmFd, state.crtcId, DRM_MODE_OBJECT_CRTC,
                       "ACTIVE")
          .id;
  state.propCrtcModeId =
      getDrmObjectProp(state.drmFd, state.crtcId, DRM_MODE_OBJECT_CRTC,
                       "MODE_ID")
          .id;
  state.propConnectorCrtcId =
      getDrmObjectProp(state.drmFd, state.connectorId,
                       DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID")
          .id;
  state.propPlaneFbId =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "FB_ID")
          .id;
  state.propPlaneCrtcId =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "CRTC_ID")
          .id;
  state.propPlaneSrcX =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "SRC_X")
          .id;
  state.propPlaneSrcY =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "SRC_Y")
          .id;
  state.propPlaneSrcW =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "SRC_W")
          .id;
  state.propPlaneSrcH =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "SRC_H")
          .id;
  state.propPlaneCrtcX =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "CRTC_X")
          .id;
  state.propPlaneCrtcY =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "CRTC_Y")
          .id;
  state.propPlaneCrtcW =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "CRTC_W")
          .id;
  state.propPlaneCrtcH =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "CRTC_H")
          .id;
  DrmObjectProp zposProp =
      getDrmObjectProp(state.drmFd, state.planeId, DRM_MODE_OBJECT_PLANE,
                       "zpos");
  state.propPlaneZpos = zposProp.id;
  state.planeZpos =
      state.propPlaneZpos
          ? getDrmObjectPropRangeMax(state.drmFd, state.planeId,
                                     DRM_MODE_OBJECT_PLANE, "zpos",
                                     zposProp.value)
          : 0;

  const bool propsOk =
      state.propCrtcActive && state.propCrtcModeId &&
      state.propConnectorCrtcId && state.propPlaneFbId &&
      state.propPlaneCrtcId && state.propPlaneSrcX && state.propPlaneSrcY &&
      state.propPlaneSrcW && state.propPlaneSrcH && state.propPlaneCrtcX &&
      state.propPlaneCrtcY && state.propPlaneCrtcW && state.propPlaneCrtcH;
  if (!propsOk) {
    LOG_WARN("[DrmKmsPresenter] missing atomic properties");
    destroyDrmModeBlob(state.drmFd, state.modeBlobId);
    drmModeFreeConnector(selectedConnector);
    drmModeFreeResources(res);
    close(state.drmFd);
    return false;
  }

  state.buffers.resize(MAX_FRAMES_IN_FLIGHT + 2);
  for (size_t i = 0; i < state.buffers.size(); ++i) {
    DrmKmsOutputBuffer &out = state.buffers[i];

    AHardwareBuffer_Desc desc{};
    desc.width = state.width;
    desc.height = state.height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    if (AHardwareBuffer_allocate(&desc, &out.buffer) != 0 || !out.buffer) {
      LOG_WARN("[DrmKmsPresenter] AHardwareBuffer_allocate failed index=%zu",
               i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }

    AHardwareBuffer_Desc actualDesc{};
    AHardwareBuffer_describe(out.buffer, &actualDesc);
    VkAndroidHardwareBufferFormatPropertiesANDROID formatProps{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    props.pNext = &formatProps;
    auto getAhbProps =
        reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(device_,
                                "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!getAhbProps ||
        getAhbProps(device_, out.buffer, &props) != VK_SUCCESS ||
        formatProps.format == VK_FORMAT_UNDEFINED) {
      LOG_WARN("[DrmKmsPresenter] AHB Vulkan props failed index=%zu", i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }
    const uint32_t drmFormat =
        actualDesc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
            ? DRM_FORMAT_XBGR8888
            : DRM_FORMAT_ABGR8888;
    if (formatProps.format != swapchainImageFormat_) {
      LOG_WARN("[DrmKmsPresenter] AHB vkFormat=%d is incompatible with "
               "swapchain vkFormat=%d; keep Surface presenter",
               static_cast<int>(formatProps.format),
               static_cast<int>(swapchainImageFormat_));
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }
    if (state.renderPass == VK_NULL_HANDLE) {
      state.renderPass = createProbeRenderPass(device_, formatProps.format);
      state.vkFormat = formatProps.format;
      state.drmFormat = drmFormat;
      if (state.renderPass == VK_NULL_HANDLE) {
        LOG_WARN("[DrmKmsPresenter] output render pass creation failed "
                 "vkFormat=%d",
                 static_cast<int>(formatProps.format));
        drmKmsPresenter_ = std::move(state);
        shutdownDrmKmsPresenter();
        drmModeFreeConnector(selectedConnector);
        drmModeFreeResources(res);
        return false;
      }
    } else if (state.vkFormat != formatProps.format ||
               state.drmFormat != drmFormat) {
      LOG_WARN("[DrmKmsPresenter] inconsistent AHB format index=%zu "
               "vkFormat=%d/%d drm=0x%x/0x%x",
               i, static_cast<int>(formatProps.format),
               static_cast<int>(state.vkFormat), drmFormat, state.drmFormat);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }
    LOG_INFO("[DrmKmsPresenter] output buffer index=%zu ahbFormat=%u "
             "vkFormat=%d drmFourcc=0x%x stride=%u allocation=%llu",
             i, actualDesc.format, static_cast<int>(formatProps.format),
             drmFormat, actualDesc.stride,
             static_cast<unsigned long long>(props.allocationSize));

    VkExternalMemoryImageCreateInfo externalImage{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalImage.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalImage;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = formatProps.format;
    imageInfo.extent = {actualDesc.width, actualDesc.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &imageInfo, nullptr, &out.image) !=
        VK_SUCCESS) {
      LOG_WARN("[DrmKmsPresenter] vkCreateImage failed index=%zu", i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    importInfo.buffer = out.buffer;
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &importInfo;
    dedicated.image = out.image;
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext = &dedicated;
    allocInfo.allocationSize = props.allocationSize;
    allocInfo.memoryTypeIndex =
        findMemoryType(props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device_, &allocInfo, nullptr, &out.memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(device_, out.image, out.memory, 0) != VK_SUCCESS) {
      LOG_WARN("[DrmKmsPresenter] memory import failed index=%zu", i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = formatProps.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &viewInfo, nullptr, &out.imageView) !=
        VK_SUCCESS) {
      LOG_WARN("[DrmKmsPresenter] vkCreateImageView failed index=%zu", i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }

    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = state.renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &out.imageView;
    fbInfo.width = actualDesc.width;
    fbInfo.height = actualDesc.height;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device_, &fbInfo, nullptr, &out.framebuffer) !=
        VK_SUCCESS) {
      LOG_WARN("[DrmKmsPresenter] vkCreateFramebuffer failed index=%zu", i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }

    AhbNativeHandleInfo handleInfo = inspectAhbNativeHandle(out.buffer);
    if (handleInfo.dupFds.empty()) {
      LOG_WARN("[DrmKmsPresenter] AHB has no dma-buf fd index=%zu", i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }
    out.dmaBufFd = handleInfo.dupFds[0];
    for (size_t fdIndex = 1; fdIndex < handleInfo.dupFds.size(); ++fdIndex) {
      close(handleInfo.dupFds[fdIndex]);
    }
    const uint32_t pitchBytes =
        actualDesc.stride > 0 ? actualDesc.stride * 4 : state.width * 4;
    if (!createPersistentDrmFbFromAhb(state.drmFd, out.dmaBufFd,
                                      actualDesc.width, actualDesc.height,
                                      pitchBytes, drmFormat,
                                      &out.gemHandle, &out.fbId)) {
      LOG_WARN("[DrmKmsPresenter] DRM FB creation failed index=%zu", i);
      drmKmsPresenter_ = std::move(state);
      shutdownDrmKmsPresenter();
      drmModeFreeConnector(selectedConnector);
      drmModeFreeResources(res);
      return false;
    }
  }

  state.initialized = true;
  const std::string planeType = getDrmPlaneType(state.drmFd, state.planeId);
  LOG_WARN("[DrmKmsPresenter] initialized connector=%u crtc=%u plane=%u "
           "type=%s zpos=%llu mode=%ux%u buffers=%zu vkFormat=%d "
           "drmFourcc=0x%x",
           state.connectorId, state.crtcId, state.planeId, planeType.c_str(),
           static_cast<unsigned long long>(state.planeZpos), state.width,
           state.height, state.buffers.size(), static_cast<int>(state.vkFormat),
           state.drmFormat);

  drmModeFreeConnector(selectedConnector);
  drmModeFreeResources(res);
  drmKmsPresenter_ = std::move(state);
  startDrmKmsCommitThread();
  return true;
}

bool VulkanRenderer::isDrmKmsBackendRequested() const {
  return drmKmsBackendRequested_ || drmKmsPresenterRequested();
}

void VulkanRenderer::shutdownDrmKmsPresenter() {
  stopDrmKmsCommitThread();
  DrmKmsPresenterState &state = drmKmsPresenter_;
  if (device_ != VK_NULL_HANDLE) {
    for (DrmKmsOutputBuffer &buffer : state.buffers) {
      if (buffer.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_, buffer.framebuffer, nullptr);
        buffer.framebuffer = VK_NULL_HANDLE;
      }
      if (buffer.imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, buffer.imageView, nullptr);
        buffer.imageView = VK_NULL_HANDLE;
      }
      if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
      }
      if (buffer.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, buffer.image, nullptr);
        buffer.image = VK_NULL_HANDLE;
      }
    }
    if (state.renderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, state.renderPass, nullptr);
      state.renderPass = VK_NULL_HANDLE;
    }
  }
  if (state.drmFd >= 0) {
    for (DrmKmsOutputBuffer &buffer : state.buffers) {
      cleanupDrmFb(state.drmFd, buffer.fbId, buffer.gemHandle);
      buffer.fbId = 0;
      buffer.gemHandle = 0;
      if (buffer.dmaBufFd >= 0) {
        close(buffer.dmaBufFd);
        buffer.dmaBufFd = -1;
      }
      if (buffer.buffer) {
        AHardwareBuffer_release(buffer.buffer);
        buffer.buffer = nullptr;
      }
    }
    if (state.modeBlobId != 0) {
      destroyDrmModeBlob(state.drmFd, state.modeBlobId);
      state.modeBlobId = 0;
    }
    close(state.drmFd);
    state.drmFd = -1;
  } else {
    for (DrmKmsOutputBuffer &buffer : state.buffers) {
      if (buffer.dmaBufFd >= 0) {
        close(buffer.dmaBufFd);
        buffer.dmaBufFd = -1;
      }
      if (buffer.buffer) {
        AHardwareBuffer_release(buffer.buffer);
        buffer.buffer = nullptr;
      }
    }
  }
  state.buffers.clear();
  state.initialized = false;
}

bool VulkanRenderer::isDrmKmsPresentActive() const {
  return drmKmsPresenter_.requested && drmKmsPresenter_.initialized;
}

bool VulkanRenderer::commitReadyDrmKmsBuffer() {
  if (!isDrmKmsPresentActive() || device_ == VK_NULL_HANDLE) {
    return false;
  }

  DrmKmsPresenterState &state = drmKmsPresenter_;
  int readyBufferIndex = -1;
  uint64_t readySequence = 0;
  for (size_t i = 0; i < state.buffers.size(); ++i) {
    DrmKmsOutputBuffer &candidate = state.buffers[i];
    if (!candidate.renderPending || candidate.renderFence == VK_NULL_HANDLE) {
      continue;
    }
    const VkResult status = vkGetFenceStatus(device_, candidate.renderFence);
    if (status == VK_SUCCESS) {
      if (candidate.sequence >= readySequence) {
        readySequence = candidate.sequence;
        readyBufferIndex = static_cast<int>(i);
      }
    } else if (status == VK_ERROR_DEVICE_LOST) {
      markDeviceLostFatal("vkGetFenceStatus(drm-kms)", status);
      return false;
    } else if (status != VK_NOT_READY) {
      static int fenceStatusWarnCount = 0;
      if (++fenceStatusWarnCount <= 10) {
        LOG_WARN("[DrmKmsPresenter] fence status failed buffer=%zu result=%d",
                 i, status);
      }
      candidate.renderPending = false;
      candidate.renderFence = VK_NULL_HANDLE;
    }
  }

  if (readyBufferIndex < 0) {
    return false;
  }

  DrmKmsOutputBuffer &buffer =
      state.buffers[static_cast<size_t>(readyBufferIndex)];
  for (DrmKmsOutputBuffer &candidate : state.buffers) {
    if (candidate.renderPending && candidate.sequence <= readySequence) {
      candidate.renderPending = false;
      candidate.renderFence = VK_NULL_HANDLE;
    }
  }

  {
    std::lock_guard<std::mutex> lock(drmKmsCommitMutex_);
    if (drmKmsQueuedCommitBuffer_ >= 0 &&
        drmKmsQueuedCommitBuffer_ != readyBufferIndex &&
        static_cast<size_t>(drmKmsQueuedCommitBuffer_) < state.buffers.size()) {
      state.buffers[static_cast<size_t>(drmKmsQueuedCommitBuffer_)].kmsPending =
          false;
    }
    buffer.kmsPending = true;
    drmKmsQueuedCommitBuffer_ = readyBufferIndex;
    drmKmsQueuedCommitSequence_ = readySequence;
  }
  drmKmsCommitCv_.notify_one();

  static int queueLogCount = 0;
  if (++queueLogCount <= 8 || queueLogCount % 120 == 0) {
    LOG_INFO("[DrmKmsPresenter] present queued fb=%u buffer=%d sequence=%llu "
             "count=%d",
             buffer.fbId, readyBufferIndex,
             static_cast<unsigned long long>(readySequence), queueLogCount);
  }
  return true;
}

bool VulkanRenderer::commitDrmKmsBufferBlocking(size_t bufferIndex,
                                                uint64_t sequence) {
  if (!isDrmKmsPresentActive() || bufferIndex >= drmKmsPresenter_.buffers.size()) {
    return false;
  }

  DrmKmsPresenterState &state = drmKmsPresenter_;
  DrmKmsOutputBuffer &buffer = state.buffers[bufferIndex];
  std::vector<uint32_t> objectIds;
  std::vector<uint32_t> propCounts;
  std::vector<uint32_t> propIds;
  std::vector<uint64_t> propValues;
  auto addObject = [&](uint32_t objectId, const std::vector<uint32_t> &ids,
                       const std::vector<uint64_t> &values) {
    objectIds.push_back(objectId);
    propCounts.push_back(static_cast<uint32_t>(ids.size()));
    propIds.insert(propIds.end(), ids.begin(), ids.end());
    propValues.insert(propValues.end(), values.begin(), values.end());
  };

  if (!state.modeSetCommitted) {
    addObject(state.crtcId, {state.propCrtcActive, state.propCrtcModeId},
              {1, state.modeBlobId});
    addObject(state.connectorId, {state.propConnectorCrtcId}, {state.crtcId});
  }
  std::vector<uint32_t> planePropIds = {
      state.propPlaneFbId,   state.propPlaneCrtcId, state.propPlaneSrcX,
      state.propPlaneSrcY,   state.propPlaneSrcW,   state.propPlaneSrcH,
      state.propPlaneCrtcX,  state.propPlaneCrtcY,  state.propPlaneCrtcW,
      state.propPlaneCrtcH};
  std::vector<uint64_t> planePropValues = {
      buffer.fbId,
      state.crtcId,
      0,
      0,
      static_cast<uint64_t>(state.width) << 16,
      static_cast<uint64_t>(state.height) << 16,
      0,
      0,
      state.width,
      state.height};
  if (state.propPlaneZpos != 0) {
    planePropIds.push_back(state.propPlaneZpos);
    planePropValues.push_back(state.planeZpos);
  }
  addObject(state.planeId, planePropIds, planePropValues);

  drm_mode_atomic atomic{};
  // This runs on the dedicated KMS commit thread. A blocking atomic commit
  // completes at vblank before the old scanout buffer is made reusable.
  atomic.flags = 0;
  if (!state.modeSetCommitted) {
    atomic.flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
  }
  atomic.count_objs = static_cast<uint32_t>(objectIds.size());
  atomic.objs_ptr = reinterpret_cast<uint64_t>(objectIds.data());
  atomic.count_props_ptr = reinterpret_cast<uint64_t>(propCounts.data());
  atomic.props_ptr = reinterpret_cast<uint64_t>(propIds.data());
  atomic.prop_values_ptr = reinterpret_cast<uint64_t>(propValues.data());

  auto commitStart = std::chrono::steady_clock::now();
  const int ret = ioctl(state.drmFd, DRM_IOCTL_MODE_ATOMIC, &atomic);
  const int commitErrno = errno;
  auto commitEnd = std::chrono::steady_clock::now();
  const long long commitMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(commitEnd -
                                                            commitStart)
          .count();
  if (ret != 0) {
    LOG_WARN("[DrmKmsPresenter] atomic commit failed fb=%u errno=%d(%s) "
             "commit=%lldms sequence=%llu",
             buffer.fbId, commitErrno, strerror(commitErrno), commitMs,
             static_cast<unsigned long long>(sequence));
    return false;
  }

  state.modeSetCommitted = true;

  static int commitLogCount = 0;
  if (++commitLogCount <= 8 || commitMs >= 8 || commitLogCount % 120 == 0) {
    LOG_INFO("[DrmKmsPresenter] present ok fb=%u buffer=%zu atomic=%lldms "
             "sequence=%llu count=%d",
             buffer.fbId, bufferIndex, commitMs,
             static_cast<unsigned long long>(sequence), commitLogCount);
  }
  return true;
}

void VulkanRenderer::startDrmKmsCommitThread() {
  std::lock_guard<std::mutex> lock(drmKmsCommitMutex_);
  if (drmKmsCommitThreadStarted_) {
    return;
  }
  drmKmsCommitThreadStop_ = false;
  drmKmsQueuedCommitBuffer_ = -1;
  drmKmsQueuedCommitSequence_ = 0;
  drmKmsCommittingBuffer_ = -1;
  drmKmsCommitThreadStarted_ = true;
  drmKmsCommitThread_ =
      std::thread(&VulkanRenderer::drmKmsCommitThreadLoop, this);
}

void VulkanRenderer::stopDrmKmsCommitThread() {
  {
    std::lock_guard<std::mutex> lock(drmKmsCommitMutex_);
    if (!drmKmsCommitThreadStarted_) {
      return;
    }
    drmKmsCommitThreadStop_ = true;
    drmKmsQueuedCommitBuffer_ = -1;
    drmKmsQueuedCommitSequence_ = 0;
  }
  drmKmsCommitCv_.notify_one();
  if (drmKmsCommitThread_.joinable()) {
    drmKmsCommitThread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(drmKmsCommitMutex_);
    drmKmsCommitThreadStarted_ = false;
    drmKmsCommitThreadStop_ = false;
    drmKmsCommittingBuffer_ = -1;
  }
}

void VulkanRenderer::drmKmsCommitThreadLoop() {
  for (;;) {
    int bufferIndex = -1;
    uint64_t sequence = 0;
    {
      std::unique_lock<std::mutex> lock(drmKmsCommitMutex_);
      drmKmsCommitCv_.wait(lock, [&]() {
        return drmKmsCommitThreadStop_ || drmKmsQueuedCommitBuffer_ >= 0;
      });
      if (drmKmsCommitThreadStop_ && drmKmsQueuedCommitBuffer_ < 0) {
        break;
      }
      bufferIndex = drmKmsQueuedCommitBuffer_;
      sequence = drmKmsQueuedCommitSequence_;
      drmKmsQueuedCommitBuffer_ = -1;
      drmKmsQueuedCommitSequence_ = 0;
      drmKmsCommittingBuffer_ = bufferIndex;
    }

    const bool committed =
        bufferIndex >= 0
            ? commitDrmKmsBufferBlocking(static_cast<size_t>(bufferIndex),
                                         sequence)
            : false;

    {
      std::lock_guard<std::mutex> lock(drmKmsCommitMutex_);
      if (bufferIndex >= 0 &&
          static_cast<size_t>(bufferIndex) < drmKmsPresenter_.buffers.size()) {
        drmKmsPresenter_.buffers[static_cast<size_t>(bufferIndex)].kmsPending =
            false;
        if (committed) {
          drmKmsPresenter_.currentScanoutBuffer = bufferIndex;
        }
      }
      drmKmsCommittingBuffer_ = -1;
    }
  }
}

bool VulkanRenderer::acquireDrmKmsImageForCurrentFrame() {
  if (!isDrmKmsPresentActive() || drmKmsPresenter_.buffers.empty()) {
    return false;
  }
  (void)commitReadyDrmKmsBuffer();
  DrmKmsPresenterState &state = drmKmsPresenter_;
  const size_t count = state.buffers.size();
  for (size_t step = 1; step <= count; ++step) {
    const size_t candidate = (state.currentBuffer + step) % count;
    {
      std::lock_guard<std::mutex> lock(drmKmsCommitMutex_);
      if (state.buffers[candidate].kmsPending ||
          (state.currentScanoutBuffer >= 0 &&
           candidate == static_cast<size_t>(state.currentScanoutBuffer) &&
           count > 1)) {
        continue;
      }
    }
    if (state.buffers[candidate].renderPending) {
      continue;
    }
    state.currentBuffer = candidate;
    return true;
  }

  static int noBufferWarnCount = 0;
  if (++noBufferWarnCount <= 10 || noBufferWarnCount % 120 == 0) {
    LOG_WARN("[DrmKmsPresenter] no output buffer available; all buffers "
             "still render-pending");
  }
  return false;
}

VkFramebuffer VulkanRenderer::getDrmKmsFramebuffer() const {
  if (!isDrmKmsPresentActive() || drmKmsPresenter_.buffers.empty()) {
    return VK_NULL_HANDLE;
  }
  return drmKmsPresenter_.buffers[drmKmsPresenter_.currentBuffer].framebuffer;
}

bool VulkanRenderer::acquireOutputImageForCurrentFrame() {
  if (isDrmKmsPresentActive()) {
    return acquireDrmKmsImageForCurrentFrame();
  }
  return acquireSwapchainImageForCurrentFrame();
}

VkFramebuffer VulkanRenderer::getOutputFramebuffer() const {
  if (isDrmKmsPresentActive()) {
    return getDrmKmsFramebuffer();
  }
  return getSwapchainFramebuffer();
}

bool VulkanRenderer::submitAndPresentDrmKmsFrame() {
  if (!isDrmKmsPresentActive() || device_ == VK_NULL_HANDLE ||
      graphicsQueue_ == VK_NULL_HANDLE ||
      currentFrame_ >= commandBuffers_.size() ||
      currentFrame_ >= inFlightFences_.size()) {
    return false;
  }

  const size_t frameIndex = currentFrame_;
  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffers_[frameIndex];

  auto submitStart = std::chrono::steady_clock::now();
  VkResult submitResult =
      vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[frameIndex]);
  if (submitResult != VK_SUCCESS) {
    LOG_WARN("[DrmKmsPresenter] vkQueueSubmit failed result=%d", submitResult);
    if (submitResult == VK_ERROR_DEVICE_LOST) {
      markDeviceLostFatal("vkQueueSubmit(drm-kms)", submitResult);
    }
    return false;
  }

  auto submitEnd = std::chrono::steady_clock::now();
  DrmKmsPresenterState &state = drmKmsPresenter_;
  DrmKmsOutputBuffer &submittedBuffer = state.buffers[state.currentBuffer];
  submittedBuffer.renderFence = inFlightFences_[frameIndex];
  submittedBuffer.sequence = state.nextSequence++;
  submittedBuffer.renderPending = true;

  const bool committed = commitReadyDrmKmsBuffer();

  static int presentLogCount = 0;
  const auto submitMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(submitEnd -
                                                            submitStart)
          .count();
  if (++presentLogCount <= 8 || submitMs >= 8 || presentLogCount % 120 == 0) {
    LOG_INFO("[DrmKmsPresenter] submit queued buffer=%zu submit=%lldms "
             "ready=%d sequence=%llu count=%d",
             state.currentBuffer, static_cast<long long>(submitMs),
             committed ? 1 : 0,
             static_cast<unsigned long long>(submittedBuffer.sequence),
             presentLogCount);
  }
  currentFrame_ = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
  currentFrameHasSwapchainImage_ = false;
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
  return true;
}

} // namespace hsvj

#endif // __ANDROID__
