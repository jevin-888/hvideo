#ifdef __ANDROID__

#include "renderer/DrmKmsProbe.h"
#include "utils/Logger.h"

#include <android/log.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <xf86drm.h>
#include <xf86drmMode.h>
}

#ifndef DRM_CLIENT_CAP_UNIVERSAL_PLANES
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#endif
#ifndef DRM_CLIENT_CAP_ATOMIC
#define DRM_CLIENT_CAP_ATOMIC 3
#endif
#ifndef DRM_CAP_DUMB_BUFFER
#define DRM_CAP_DUMB_BUFFER 0x1
#endif
#ifndef DRM_MODE_UNKNOWNCONNECTION
#define DRM_MODE_UNKNOWNCONNECTION 3
#endif

namespace hsvj {
namespace {

struct PlaneInfo {
  uint32_t id = 0;
  std::string type;
  uint32_t possibleCrtcs = 0;
  uint32_t fbId = 0;
  std::vector<uint32_t> formats;
};

struct ObjectProp {
  uint32_t id = 0;
  uint64_t value = 0;
};

struct AtomicPropGroup {
  uint32_t objectId = 0;
  std::vector<uint32_t> props;
  std::vector<uint64_t> values;
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

const char *connectorStatusName(uint32_t status) {
  switch (status) {
  case DRM_MODE_CONNECTED:
    return "connected";
  case DRM_MODE_DISCONNECTED:
    return "disconnected";
  case DRM_MODE_UNKNOWNCONNECTION:
    return "unknown";
  }
  return "invalid";
}

std::string planeTypeName(uint64_t type) {
  switch (type) {
  case DRM_PLANE_TYPE_OVERLAY:
    return "Overlay";
  case DRM_PLANE_TYPE_PRIMARY:
    return "Primary";
  case DRM_PLANE_TYPE_CURSOR:
    return "Cursor";
  default:
    return "unknown";
  }
}

ObjectProp getObjectProp(int fd, uint32_t objectId, uint32_t objectType,
                         const char *name) {
  ObjectProp result;
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

std::string getPlaneType(int fd, uint32_t planeId) {
  drmModeObjectPropertiesPtr props =
      drmModeObjectGetProperties(fd, planeId, DRM_MODE_OBJECT_PLANE);
  if (!props) {
    return "unknown";
  }

  std::string type = "unknown";
  for (uint32_t i = 0; i < props->count_props; ++i) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop) {
      continue;
    }
    if (strcmp(prop->name, "type") == 0) {
      type = planeTypeName(props->prop_values[i]);
      drmModeFreeProperty(prop);
      break;
    }
    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return type;
}

int findCrtcIndex(drmModeResPtr res, uint32_t crtcId) {
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

uint32_t findUsableCrtc(int fd, drmModeResPtr res, drmModeConnectorPtr conn) {
  if (!res || !conn) {
    return 0;
  }

  if (conn->encoder_id != 0) {
    drmModeEncoderPtr encoder = drmModeGetEncoder(fd, conn->encoder_id);
    if (encoder) {
      const uint32_t crtcId = encoder->crtc_id;
      drmModeFreeEncoder(encoder);
      if (crtcId != 0) {
        return crtcId;
      }
    }
  }

  for (int i = 0; i < conn->count_encoders; ++i) {
    drmModeEncoderPtr encoder = drmModeGetEncoder(fd, conn->encoders[i]);
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

std::vector<PlaneInfo> collectPlanes(int fd) {
  std::vector<PlaneInfo> planes;
  drmModePlaneResPtr planeRes = drmModeGetPlaneResources(fd);
  if (!planeRes) {
    LOG_WARN("[DrmKmsProbe] drmModeGetPlaneResources failed: %s",
             strerror(errno));
    return planes;
  }

  planes.reserve(planeRes->count_planes);
  for (uint32_t i = 0; i < planeRes->count_planes; ++i) {
    drmModePlanePtr plane = drmModeGetPlane(fd, planeRes->planes[i]);
    if (!plane) {
      continue;
    }

    PlaneInfo info;
    info.id = plane->plane_id;
    info.possibleCrtcs = plane->possible_crtcs;
    info.fbId = plane->fb_id;
    info.type = getPlaneType(fd, info.id);
    for (uint32_t f = 0; f < plane->count_formats; ++f) {
      info.formats.push_back(plane->formats[f]);
    }
    planes.push_back(info);

    char formatSummary[128] = {};
    size_t offset = 0;
    for (size_t f = 0; f < info.formats.size() && f < 6; ++f) {
      const uint32_t fmt = info.formats[f];
      const int written = snprintf(formatSummary + offset,
                                   sizeof(formatSummary) - offset,
                                   "%s%c%c%c%c",
                                   f == 0 ? "" : ",",
                                   fmt & 0xff,
                                   (fmt >> 8) & 0xff,
                                   (fmt >> 16) & 0xff,
                                   (fmt >> 24) & 0xff);
      if (written <= 0 ||
          static_cast<size_t>(written) >= sizeof(formatSummary) - offset) {
        break;
      }
      offset += static_cast<size_t>(written);
    }

    LOG_INFO("[DrmKmsProbe] plane id=%u type=%s possibleCrtcs=0x%x "
             "fb=%u formats=%u [%s%s]",
             info.id, info.type.c_str(), info.possibleCrtcs, info.fbId,
             static_cast<unsigned>(info.formats.size()), formatSummary,
             info.formats.size() > 6 ? ",..." : "");

    drmModeFreePlane(plane);
  }

  drmModeFreePlaneResources(planeRes);
  return planes;
}

int setClientCap(int fd, uint64_t capability, uint64_t value) {
  struct DrmSetClientCap {
    uint64_t capability;
    uint64_t value;
  } cap = {};
  cap.capability = capability;
  cap.value = value;
  return ioctl(fd, DRM_IOW(0x0d, DrmSetClientCap), &cap);
}

bool getCap(int fd, uint64_t capability, uint64_t *value) {
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

void addAtomicProp(std::vector<AtomicPropGroup> *groups, uint32_t objectId,
                   uint32_t propId, uint64_t value) {
  for (AtomicPropGroup &group : *groups) {
    if (group.objectId == objectId) {
      group.props.push_back(propId);
      group.values.push_back(value);
      return;
    }
  }
  AtomicPropGroup group;
  group.objectId = objectId;
  group.props.push_back(propId);
  group.values.push_back(value);
  groups->push_back(std::move(group));
}

uint32_t getCurrentCrtcFb(int fd, uint32_t crtcId) {
  drm_mode_crtc crtc = {};
  crtc.crtc_id = crtcId;
  if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0) {
    LOG_WARN("[DrmKmsProbe] DRM_IOCTL_MODE_GETCRTC crtc=%u failed: %s",
             crtcId, strerror(errno));
    return 0;
  }
  return crtc.fb_id;
}

bool runCurrentStateAtomicTestOnly(int fd, drmModeConnectorPtr conn,
                                   drmModeModeInfo *mode, uint32_t crtcId,
                                   int crtcIndex,
                                   const std::vector<PlaneInfo> &planes) {
  if (!conn || !mode || crtcId == 0 || crtcIndex < 0) {
    return false;
  }

  const uint32_t crtcMask = 1u << static_cast<uint32_t>(crtcIndex);
  const PlaneInfo *primary = nullptr;
  for (const PlaneInfo &plane : planes) {
    if ((plane.possibleCrtcs & crtcMask) != 0 && plane.fbId != 0) {
      primary = &plane;
      break;
    }
  }
  if (!primary) {
    for (const PlaneInfo &plane : planes) {
      if (plane.type == "Primary" && (plane.possibleCrtcs & crtcMask) != 0) {
        primary = &plane;
        break;
      }
    }
    if (!primary) {
      LOG_WARN("[DrmKmsProbe] no plane for crtc=%u", crtcId);
      return false;
    }
  }

  uint32_t fbId = primary->fbId;
  if (fbId == 0) {
    fbId = getCurrentCrtcFb(fd, crtcId);
  }
  if (fbId == 0) {
    LOG_WARN("[DrmKmsProbe] no current fb for atomic TEST_ONLY plane=%u type=%s",
             primary->id, primary->type.c_str());
    return false;
  }

  const ObjectProp crtcActive =
      getObjectProp(fd, crtcId, DRM_MODE_OBJECT_CRTC, "ACTIVE");
  const ObjectProp crtcMode =
      getObjectProp(fd, crtcId, DRM_MODE_OBJECT_CRTC, "MODE_ID");
  const ObjectProp connCrtc =
      getObjectProp(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR,
                    "CRTC_ID");
  const ObjectProp planeFb =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "FB_ID");
  const ObjectProp planeCrtc =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
  const ObjectProp planeSrcX =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "SRC_X");
  const ObjectProp planeSrcY =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
  const ObjectProp planeSrcW =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "SRC_W");
  const ObjectProp planeSrcH =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "SRC_H");
  const ObjectProp planeCrtcX =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
  const ObjectProp planeCrtcY =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
  const ObjectProp planeCrtcW =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
  const ObjectProp planeCrtcH =
      getObjectProp(fd, primary->id, DRM_MODE_OBJECT_PLANE, "CRTC_H");

  const bool propsOk =
      crtcActive.id && crtcMode.id && crtcMode.value && connCrtc.id &&
      planeFb.id && planeCrtc.id && planeSrcX.id && planeSrcY.id &&
      planeSrcW.id && planeSrcH.id && planeCrtcX.id && planeCrtcY.id &&
      planeCrtcW.id && planeCrtcH.id;
  if (!propsOk) {
    LOG_WARN("[DrmKmsProbe] missing properties for current-state atomic "
             "TEST_ONLY active=%u mode=%u/%llu conn=%u planeFb=%u",
             crtcActive.id, crtcMode.id,
             static_cast<unsigned long long>(crtcMode.value), connCrtc.id,
             planeFb.id);
    return false;
  }

  std::vector<AtomicPropGroup> groups;
  addAtomicProp(&groups, crtcId, crtcActive.id, 1);
  addAtomicProp(&groups, crtcId, crtcMode.id, crtcMode.value);
  addAtomicProp(&groups, conn->connector_id, connCrtc.id, crtcId);
  addAtomicProp(&groups, primary->id, planeFb.id, fbId);
  addAtomicProp(&groups, primary->id, planeCrtc.id, crtcId);
  addAtomicProp(&groups, primary->id, planeSrcX.id, 0);
  addAtomicProp(&groups, primary->id, planeSrcY.id, 0);
  addAtomicProp(&groups, primary->id, planeSrcW.id,
                static_cast<uint64_t>(mode->hdisplay) << 16);
  addAtomicProp(&groups, primary->id, planeSrcH.id,
                static_cast<uint64_t>(mode->vdisplay) << 16);
  addAtomicProp(&groups, primary->id, planeCrtcX.id, 0);
  addAtomicProp(&groups, primary->id, planeCrtcY.id, 0);
  addAtomicProp(&groups, primary->id, planeCrtcW.id, mode->hdisplay);
  addAtomicProp(&groups, primary->id, planeCrtcH.id, mode->vdisplay);

  std::vector<uint32_t> objectIds;
  std::vector<uint32_t> propCounts;
  std::vector<uint32_t> propIds;
  std::vector<uint64_t> propValues;
  for (const AtomicPropGroup &group : groups) {
    objectIds.push_back(group.objectId);
    propCounts.push_back(static_cast<uint32_t>(group.props.size()));
    propIds.insert(propIds.end(), group.props.begin(), group.props.end());
    propValues.insert(propValues.end(), group.values.begin(), group.values.end());
  }

  drm_mode_atomic atomic = {};
  atomic.flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
  atomic.count_objs = static_cast<uint32_t>(objectIds.size());
  atomic.objs_ptr = reinterpret_cast<uint64_t>(objectIds.data());
  atomic.count_props_ptr = reinterpret_cast<uint64_t>(propCounts.data());
  atomic.props_ptr = reinterpret_cast<uint64_t>(propIds.data());
  atomic.prop_values_ptr = reinterpret_cast<uint64_t>(propValues.data());

  const int ret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
  if (ret == 0) {
    LOG_INFO("[DrmKmsProbe] current-state atomic TEST_ONLY ok connector=%u "
             "crtc=%u primaryPlane=%u fb=%u modeBlob=%llu",
             conn->connector_id, crtcId, primary->id, fbId,
             static_cast<unsigned long long>(crtcMode.value));
    return true;
  }

  LOG_WARN("[DrmKmsProbe] current-state atomic TEST_ONLY failed connector=%u "
           "crtc=%u primaryPlane=%u fb=%u: %s",
           conn->connector_id, crtcId, primary->id, fbId, strerror(errno));
  return false;
}

bool probeCard(const char *path) {
  const int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    LOG_WARN("[DrmKmsProbe] open %s failed: %s", path, strerror(errno));
    return false;
  }

  LOG_INFO("[DrmKmsProbe] opened %s", path);
  drmVersionPtr version = drmGetVersion(fd);
  if (version) {
    LOG_INFO("[DrmKmsProbe] driver name=%.*s desc=%.*s date=%.*s",
             version->name_len, version->name ? version->name : "",
             version->desc_len, version->desc ? version->desc : "",
             version->date_len, version->date ? version->date : "");
    drmFreeVersion(version);
  }

  const int universalRet = setClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  const int atomicRet = setClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
  LOG_INFO("[DrmKmsProbe] caps universal=%d(%s) atomic=%d(%s)",
           universalRet, universalRet == 0 ? "ok" : strerror(errno),
           atomicRet, atomicRet == 0 ? "ok" : strerror(errno));

  uint64_t dumbCap = 0;
  if (getCap(fd, DRM_CAP_DUMB_BUFFER, &dumbCap)) {
    LOG_INFO("[DrmKmsProbe] cap DUMB_BUFFER=%llu",
             static_cast<unsigned long long>(dumbCap));
  }

  drmModeResPtr res = drmModeGetResources(fd);
  if (!res) {
    LOG_WARN("[DrmKmsProbe] drmModeGetResources failed: %s", strerror(errno));
    close(fd);
    return false;
  }

  LOG_INFO("[DrmKmsProbe] resources fb=%d crtc=%d connector=%d encoder=%d",
           res->count_fbs, res->count_crtcs, res->count_connectors,
           res->count_encoders);

  std::vector<PlaneInfo> planes = collectPlanes(fd);

  bool foundConnected = false;
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
    if (!conn) {
      continue;
    }

    LOG_INFO("[DrmKmsProbe] connector id=%u type=%u status=%s modes=%d "
             "encoders=%d",
             conn->connector_id, conn->connector_type,
             connectorStatusName(conn->connection), conn->count_modes,
             conn->count_encoders);

    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      foundConnected = true;
      drmModeModeInfo mode = conn->modes[0];
      const uint32_t crtcId = findUsableCrtc(fd, res, conn);
      const int crtcIndex = findCrtcIndex(res, crtcId);
      LOG_INFO("[DrmKmsProbe] selected connector=%u crtc=%u crtcIndex=%d "
               "mode=%s %dx%d@%d",
               conn->connector_id, crtcId, crtcIndex, mode.name,
               mode.hdisplay, mode.vdisplay, mode.vrefresh);

      int masterRet = drmSetMaster(fd);
      const bool hasMaster = masterRet == 0;
      LOG_INFO("[DrmKmsProbe] drmSetMaster=%d(%s)", masterRet,
               hasMaster ? "ok" : strerror(errno));
      if (hasMaster) {
        LOG_INFO("[DrmKmsProbe] master available");
        drmDropMaster(fd);
      }
      runCurrentStateAtomicTestOnly(fd, conn, &mode, crtcId, crtcIndex,
                                    planes);
    }

    drmModeFreeConnector(conn);
  }

  drmModeFreeResources(res);
  close(fd);

  LOG_INFO("[DrmKmsProbe] summary path=%s connected=%d", path,
           foundConnected ? 1 : 0);
  return foundConnected;
}

} // namespace

void DrmKmsProbe::runStartupProbeIfRequested() {
  if (!propEnabled("debug.hsvj.drm_kms_probe")) {
    return;
  }
  runOnce();
}

void DrmKmsProbe::runOnce() {
  LOG_WARN("[DrmKmsProbe] starting DRM/KMS capability probe");
  bool found = false;
  const char *paths[] = {"/dev/dri/card0", "/dev/dri/card1"};
  for (const char *path : paths) {
    found = probeCard(path) || found;
  }
  LOG_WARN("[DrmKmsProbe] complete usableConnector=%d", found ? 1 : 0);
}

} // namespace hsvj

#endif // __ANDROID__
