/**
 * @file xf86drm.h
 * @brief libdrm 用户空间接口（Android 简化版）
 * 
 * 来源：libdrm xf86drm.h
 * 仅包含 Rockchip DRM 渲染所需的函数声明
 */

#ifndef _XF86DRM_H_
#define _XF86DRM_H_

#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DRM 版本信息 */
typedef struct _drmVersion {
    int     version_major;
    int     version_minor;
    int     version_patchlevel;
    int     name_len;
    char    *name;
    int     date_len;
    char    *date;
    int     desc_len;
    char    *desc;
} drmVersion, *drmVersionPtr;

/* PRIME 句柄 */
struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
};

/* GEM 关闭 */
struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

/* 函数声明 - 这些函数在 Android 上通过直接 ioctl 实现 */

/* 获取 DRM 版本 */
static inline drmVersionPtr drmGetVersion(int fd) {
    // Android 上我们不使用这个，返回静态数据
    static drmVersion ver = {
        .version_major = 1,
        .version_minor = 0,
        .version_patchlevel = 0,
        .name_len = 8,
        .name = (char*)"rockchip",
        .date_len = 0,
        .date = (char*)"",
        .desc_len = 0,
        .desc = (char*)""
    };
    (void)fd;
    return &ver;
}

static inline void drmFreeVersion(drmVersionPtr v) {
    (void)v;
    // 静态数据，不需要释放
}

/* DRM Master 控制 */
static inline int drmSetMaster(int fd) {
    // DRM_IOCTL_SET_MASTER = 0x1e
    return ioctl(fd, _IO('d', 0x1e));
}

static inline int drmDropMaster(int fd) {
    // DRM_IOCTL_DROP_MASTER = 0x1f
    return ioctl(fd, _IO('d', 0x1f));
}

/* FB flags */
#define DRM_MODE_FB_MODIFIERS (1 << 1)

/* PRIME FD 导入 */
#define DRM_IOCTL_PRIME_FD_TO_HANDLE _IOWR('d', 0x2e, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR('d', 0x2d, struct drm_prime_handle)

/* GEM 关闭 */
#define DRM_IOCTL_GEM_CLOSE _IOW('d', 0x09, struct drm_gem_close)

/* FB 操作 */
#define DRM_IOCTL_MODE_ADDFB2 _IOWR('d', 0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_RMFB   _IOWR('d', 0xAF, unsigned int)

/* 原子操作 */
typedef void* drmModeAtomicReqPtr;

static inline drmModeAtomicReqPtr drmModeAtomicAlloc(void) {
    // 分配一个简单的缓冲区用于存储原子请求
    // 实际实现会更复杂，这里简化处理
    return malloc(sizeof(struct drm_mode_atomic) + 4096);
}

static inline void drmModeAtomicFree(drmModeAtomicReqPtr req) {
    if (req) free(req);
}

static inline int drmModeAtomicAddProperty(drmModeAtomicReqPtr req,
                                           uint32_t object_id,
                                           uint32_t property_id,
                                           uint64_t value) {
    // 简化实现：实际需要构建原子请求结构
    (void)req;
    (void)object_id;
    (void)property_id;
    (void)value;
    return 0;
}

static inline void drmModeAtomicSetCursor(drmModeAtomicReqPtr req, int cursor) {
    (void)req;
    (void)cursor;
}

static inline int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req,
                                      uint32_t flags, void *user_data) {
    struct drm_mode_atomic *atomic = (struct drm_mode_atomic*)req;
    if (!atomic) return -1;
    
    atomic->flags = flags;
    atomic->user_data = (uint64_t)(uintptr_t)user_data;
    
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, atomic);
}

#ifdef __cplusplus
}
#endif

#endif /* _XF86DRM_H_ */
