/**
 * @file drm_fourcc.h
 * @brief Linux DRM FourCC 和修饰符定义（Android 简化版）
 * 
 * 来源：Linux 内核 include/uapi/drm/drm_fourcc.h
 */

#ifndef _DRM_FOURCC_H_
#define _DRM_FOURCC_H_

#include <linux/types.h>

#define fourcc_code(a, b, c, d) ((__u32)(a) | ((__u32)(b) << 8) | \
                                 ((__u32)(c) << 16) | ((__u32)(d) << 24))

/* 常用像素格式 */
#define DRM_FORMAT_ARGB8888 fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888 fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_XBGR8888 fourcc_code('X', 'B', '2', '4')
#define DRM_FORMAT_RGBA8888 fourcc_code('R', 'A', '2', '4')
#define DRM_FORMAT_RGBX8888 fourcc_code('R', 'X', '2', '4')
#define DRM_FORMAT_BGRA8888 fourcc_code('B', 'A', '2', '4')
#define DRM_FORMAT_BGRX8888 fourcc_code('B', 'X', '2', '4')

#define DRM_FORMAT_RGB888   fourcc_code('R', 'G', '2', '4')
#define DRM_FORMAT_BGR888   fourcc_code('B', 'G', '2', '4')

#define DRM_FORMAT_RGB565   fourcc_code('R', 'G', '1', '6')
#define DRM_FORMAT_BGR565   fourcc_code('B', 'G', '1', '6')

/* YUV 格式 */
#define DRM_FORMAT_NV12     fourcc_code('N', 'V', '1', '2')
#define DRM_FORMAT_NV21     fourcc_code('N', 'V', '2', '1')
#define DRM_FORMAT_NV16     fourcc_code('N', 'V', '1', '6')
#define DRM_FORMAT_NV61     fourcc_code('N', 'V', '6', '1')
#define DRM_FORMAT_YUV420   fourcc_code('Y', 'U', '1', '2')
#define DRM_FORMAT_YVU420   fourcc_code('Y', 'V', '1', '2')
#define DRM_FORMAT_YUV422   fourcc_code('Y', 'U', '1', '6')
#define DRM_FORMAT_YVU422   fourcc_code('Y', 'V', '1', '6')
#define DRM_FORMAT_YUV444   fourcc_code('Y', 'U', '2', '4')
#define DRM_FORMAT_YVU444   fourcc_code('Y', 'V', '2', '4')

/* 格式修饰符 */
#define DRM_FORMAT_MOD_VENDOR_NONE    0
#define DRM_FORMAT_MOD_VENDOR_INTEL   0x01
#define DRM_FORMAT_MOD_VENDOR_AMD     0x02
#define DRM_FORMAT_MOD_VENDOR_NVIDIA  0x03
#define DRM_FORMAT_MOD_VENDOR_SAMSUNG 0x04
#define DRM_FORMAT_MOD_VENDOR_QCOM    0x05
#define DRM_FORMAT_MOD_VENDOR_VIVANTE 0x06
#define DRM_FORMAT_MOD_VENDOR_BROADCOM 0x07
#define DRM_FORMAT_MOD_VENDOR_ARM     0x08

#define DRM_FORMAT_RESERVED           ((1ULL << 56) - 1)
#define DRM_FORMAT_MOD_INVALID        0xffffffffffffffULL
#define DRM_FORMAT_MOD_LINEAR         0ULL

#define fourcc_mod_code(vendor, val) \
    (((((__u64)DRM_FORMAT_MOD_VENDOR_## vendor) << 56) | ((val) & 0x00ffffffffffffffULL)))

/* ARM AFBC 修饰符 */
#define DRM_FORMAT_MOD_ARM_AFBC(__afbc_mode) \
    fourcc_mod_code(ARM, __afbc_mode)

/* AFBC 模式位 */
#define AFBC_FORMAT_MOD_BLOCK_SIZE_16x16   (1ULL << 0)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_32x8    (2ULL << 0)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_64x4    (3ULL << 0)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_MASK    0x3ULL

#define AFBC_FORMAT_MOD_YTR               (1ULL << 4)
#define AFBC_FORMAT_MOD_SPLIT             (1ULL << 5)
#define AFBC_FORMAT_MOD_SPARSE            (1ULL << 6)
#define AFBC_FORMAT_MOD_CBR               (1ULL << 7)
#define AFBC_FORMAT_MOD_TILED             (1ULL << 8)
#define AFBC_FORMAT_MOD_SC                (1ULL << 9)
#define AFBC_FORMAT_MOD_DB                (1ULL << 10)
#define AFBC_FORMAT_MOD_BCH               (1ULL << 11)
#define AFBC_FORMAT_MOD_USM               (1ULL << 12)

#endif /* _DRM_FOURCC_H_ */

