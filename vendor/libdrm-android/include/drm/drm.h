/**
 * @file drm.h
 * @brief Linux DRM 核心定义（Android 简化版）
 * 
 * 来源：Linux 内核 include/uapi/drm/drm.h
 * 仅包含 Rockchip DRM 渲染所需的定义
 */

#ifndef _DRM_H_
#define _DRM_H_

#include <linux/types.h>
#include <sys/ioctl.h>

#define DRM_IOCTL_BASE 'd'

#define DRM_IO(nr)        _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr, type) _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type) _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)

/* DRM object types */
#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER   0xe0e0e0e0
#define DRM_MODE_OBJECT_MODE      0xdededede
#define DRM_MODE_OBJECT_PROPERTY  0xb0b0b0b0
#define DRM_MODE_OBJECT_FB        0xfbfbfbfb
#define DRM_MODE_OBJECT_BLOB      0xbbbbbbbb
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee
#define DRM_MODE_OBJECT_ANY       0

/* Plane types */
#define DRM_PLANE_TYPE_OVERLAY  0
#define DRM_PLANE_TYPE_PRIMARY  1
#define DRM_PLANE_TYPE_CURSOR   2

/* Connection status */
#define DRM_MODE_CONNECTED         1
#define DRM_MODE_DISCONNECTED      2
#define DRM_MODE_UNKNOWNCONNECTION 3

/* Mode type flags */
#define DRM_MODE_TYPE_PREFERRED  (1 << 3)

/* Atomic flags */
#define DRM_MODE_PAGE_FLIP_EVENT     0x01
#define DRM_MODE_PAGE_FLIP_ASYNC     0x02
#define DRM_MODE_ATOMIC_TEST_ONLY    0x0100
#define DRM_MODE_ATOMIC_NONBLOCK     0x0200
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400

#endif /* _DRM_H_ */

