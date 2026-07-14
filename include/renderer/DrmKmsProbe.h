#ifndef HSVJ_RENDERER_DRM_KMS_PROBE_H
#define HSVJ_RENDERER_DRM_KMS_PROBE_H

#ifdef __ANDROID__

namespace hsvj {

class DrmKmsProbe {
public:
  static void runStartupProbeIfRequested();
  static void runOnce();
};

} // namespace hsvj

#endif // __ANDROID__

#endif // HSVJ_RENDERER_DRM_KMS_PROBE_H
