/**
 * @file drm_mode.h
 * @brief Linux DRM 模式设置定义（Android 简化版）
 * 
 * 来源：Linux 内核 include/uapi/drm/drm_mode.h
 */

#ifndef _DRM_MODE_H_
#define _DRM_MODE_H_

#include <linux/types.h>

#define DRM_DISPLAY_MODE_LEN 32
#define DRM_PROP_NAME_LEN    32

#define DRM_MODE_PROP_PENDING   (1 << 0)
#define DRM_MODE_PROP_RANGE     (1 << 1)
#define DRM_MODE_PROP_IMMUTABLE (1 << 2)
#define DRM_MODE_PROP_ENUM      (1 << 3)
#define DRM_MODE_PROP_BLOB      (1 << 4)
#define DRM_MODE_PROP_BITMASK   (1 << 5)

/* FB flags */
#define DRM_MODE_FB_INTERLACED  (1 << 0)
#define DRM_MODE_FB_MODIFIERS   (1 << 1)

struct drm_mode_modeinfo {
    __u32 clock;
    __u16 hdisplay;
    __u16 hsync_start;
    __u16 hsync_end;
    __u16 htotal;
    __u16 hskew;
    __u16 vdisplay;
    __u16 vsync_start;
    __u16 vsync_end;
    __u16 vtotal;
    __u16 vscan;
    
    __u32 vrefresh;
    
    __u32 flags;
    __u32 type;
    char name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_card_res {
    __u64 fb_id_ptr;
    __u64 crtc_id_ptr;
    __u64 connector_id_ptr;
    __u64 encoder_id_ptr;
    __u32 count_fbs;
    __u32 count_crtcs;
    __u32 count_connectors;
    __u32 count_encoders;
    __u32 min_width;
    __u32 max_width;
    __u32 min_height;
    __u32 max_height;
};

struct drm_mode_crtc {
    __u64 set_connectors_ptr;
    __u32 count_connectors;
    
    __u32 crtc_id;
    __u32 fb_id;
    
    __u32 x;
    __u32 y;
    
    __u32 gamma_size;
    __u32 mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
    __u32 encoder_id;
    __u32 encoder_type;
    
    __u32 crtc_id;
    
    __u32 possible_crtcs;
    __u32 possible_clones;
};

struct drm_mode_get_connector {
    __u64 encoders_ptr;
    __u64 modes_ptr;
    __u64 props_ptr;
    __u64 prop_values_ptr;
    
    __u32 count_modes;
    __u32 count_props;
    __u32 count_encoders;
    
    __u32 encoder_id;
    __u32 connector_id;
    __u32 connector_type;
    __u32 connector_type_id;
    
    __u32 connection;
    __u32 mm_width;
    __u32 mm_height;
    __u32 subpixel;
    
    __u32 pad;
};

struct drm_mode_get_plane {
    __u32 plane_id;
    
    __u32 crtc_id;
    __u32 fb_id;
    
    __u32 possible_crtcs;
    __u32 gamma_size;
    
    __u32 count_format_types;
    __u64 format_type_ptr;
};

struct drm_mode_get_plane_res {
    __u64 plane_id_ptr;
    __u32 count_planes;
};

struct drm_mode_obj_get_properties {
    __u64 props_ptr;
    __u64 prop_values_ptr;
    __u32 count_props;
    __u32 obj_id;
    __u32 obj_type;
};

struct drm_mode_get_property {
    __u64 values_ptr;
    __u64 enum_blob_ptr;
    
    __u32 prop_id;
    __u32 flags;
    char name[DRM_PROP_NAME_LEN];
    
    __u32 count_values;
    __u32 count_enum_blobs;
};

struct drm_mode_fb_cmd2 {
    __u32 fb_id;
    __u32 width;
    __u32 height;
    __u32 pixel_format;
    __u32 flags;
    
    __u32 handles[4];
    __u32 pitches[4];
    __u32 offsets[4];
    __u64 modifier[4];
};

struct drm_mode_atomic {
    __u32 flags;
    __u32 count_objs;
    __u64 objs_ptr;
    __u64 count_props_ptr;
    __u64 props_ptr;
    __u64 prop_values_ptr;
    __u64 reserved;
    __u64 user_data;
};

/* IOCTL numbers */
#define DRM_IOCTL_MODE_GETRESOURCES    DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC         DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC         DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER      DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR    DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB2          DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_RMFB            DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_GETPLANE        DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_GETPLANERESOURCES DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_GETPROPERTY     DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_ATOMIC          DRM_IOWR(0xBC, struct drm_mode_atomic)

#endif /* _DRM_MODE_H_ */

