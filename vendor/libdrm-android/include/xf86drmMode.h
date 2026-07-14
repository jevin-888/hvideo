/**
 * @file xf86drmMode.h
 * @brief libdrm 模式设置接口（Android 简化版）
 * 
 * 来源：libdrm xf86drmMode.h
 * 提供 DRM KMS（内核模式设置）功能
 */

#ifndef _XF86DRMMODE_H_
#define _XF86DRMMODE_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 模式信息 */
typedef struct drm_mode_modeinfo drmModeModeInfo, *drmModeModeInfoPtr;

/* 资源 */
typedef struct _drmModeRes {
    int count_fbs;
    uint32_t *fbs;
    
    int count_crtcs;
    uint32_t *crtcs;
    
    int count_connectors;
    uint32_t *connectors;
    
    int count_encoders;
    uint32_t *encoders;
    
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
} drmModeRes, *drmModeResPtr;

/* CRTC */
typedef struct _drmModeCrtc {
    uint32_t crtc_id;
    uint32_t buffer_id;
    
    uint32_t x, y;
    uint32_t width, height;
    int mode_valid;
    drmModeModeInfo mode;
    
    int gamma_size;
} drmModeCrtc, *drmModeCrtcPtr;

/* Encoder */
typedef struct _drmModeEncoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} drmModeEncoder, *drmModeEncoderPtr;

/* Connector */
typedef struct _drmModeConnector {
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mmWidth, mmHeight;
    uint32_t subpixel;
    
    int count_modes;
    drmModeModeInfoPtr modes;
    
    int count_props;
    uint32_t *props;
    uint64_t *prop_values;
    
    int count_encoders;
    uint32_t *encoders;
} drmModeConnector, *drmModeConnectorPtr;

/* Plane */
typedef struct _drmModePlane {
    uint32_t count_formats;
    uint32_t *formats;
    uint32_t plane_id;
    
    uint32_t crtc_id;
    uint32_t fb_id;
    
    uint32_t crtc_x, crtc_y;
    uint32_t x, y;
    
    uint32_t possible_crtcs;
    uint32_t gamma_size;
} drmModePlane, *drmModePlanePtr;

/* Plane Resources */
typedef struct _drmModePlaneRes {
    uint32_t count_planes;
    uint32_t *planes;
} drmModePlaneRes, *drmModePlaneResPtr;

/* Object Properties */
typedef struct _drmModeObjectProperties {
    uint32_t count_props;
    uint32_t *props;
    uint64_t *prop_values;
} drmModeObjectProperties, *drmModeObjectPropertiesPtr;

/* Property */
typedef struct _drmModeProperty {
    uint32_t prop_id;
    uint32_t flags;
    char name[DRM_PROP_NAME_LEN];
    int count_values;
    uint64_t *values;
    int count_enums;
    struct drm_mode_property_enum *enums;
    int count_blobs;
    uint32_t *blob_ids;
} drmModeProperty, *drmModePropertyPtr;

/* 内联函数实现 */

static inline drmModeResPtr drmModeGetResources(int fd) {
    struct drm_mode_card_res res = {};
    
    // 首先获取计数
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return NULL;
    
    drmModeResPtr r = (drmModeResPtr)calloc(1, sizeof(drmModeRes));
    if (!r) return NULL;
    
    r->count_fbs = res.count_fbs;
    r->count_crtcs = res.count_crtcs;
    r->count_connectors = res.count_connectors;
    r->count_encoders = res.count_encoders;
    r->min_width = res.min_width;
    r->max_width = res.max_width;
    r->min_height = res.min_height;
    r->max_height = res.max_height;
    
    // 分配数组
    if (res.count_crtcs) {
        r->crtcs = (uint32_t*)malloc(res.count_crtcs * sizeof(uint32_t));
        res.crtc_id_ptr = (uint64_t)(uintptr_t)r->crtcs;
    }
    if (res.count_connectors) {
        r->connectors = (uint32_t*)malloc(res.count_connectors * sizeof(uint32_t));
        res.connector_id_ptr = (uint64_t)(uintptr_t)r->connectors;
    }
    if (res.count_encoders) {
        r->encoders = (uint32_t*)malloc(res.count_encoders * sizeof(uint32_t));
        res.encoder_id_ptr = (uint64_t)(uintptr_t)r->encoders;
    }
    
    // 再次调用获取实际数据
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        free(r->crtcs);
        free(r->connectors);
        free(r->encoders);
        free(r);
        return NULL;
    }
    
    return r;
}

static inline void drmModeFreeResources(drmModeResPtr ptr) {
    if (!ptr) return;
    free(ptr->fbs);
    free(ptr->crtcs);
    free(ptr->connectors);
    free(ptr->encoders);
    free(ptr);
}

static inline drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connectorId) {
    struct drm_mode_get_connector conn = {};
    conn.connector_id = connectorId;
    
    // 首先获取计数
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
        return NULL;
    
    drmModeConnectorPtr c = (drmModeConnectorPtr)calloc(1, sizeof(drmModeConnector));
    if (!c) return NULL;
    
    c->connector_id = conn.connector_id;
    c->encoder_id = conn.encoder_id;
    c->connector_type = conn.connector_type;
    c->connector_type_id = conn.connector_type_id;
    c->connection = conn.connection;
    c->mmWidth = conn.mm_width;
    c->mmHeight = conn.mm_height;
    c->subpixel = conn.subpixel;
    c->count_modes = conn.count_modes;
    c->count_props = conn.count_props;
    c->count_encoders = conn.count_encoders;
    
    // 分配数组
    if (conn.count_modes) {
        c->modes = (drmModeModeInfoPtr)malloc(conn.count_modes * sizeof(drmModeModeInfo));
        conn.modes_ptr = (uint64_t)(uintptr_t)c->modes;
    }
    if (conn.count_props) {
        c->props = (uint32_t*)malloc(conn.count_props * sizeof(uint32_t));
        c->prop_values = (uint64_t*)malloc(conn.count_props * sizeof(uint64_t));
        conn.props_ptr = (uint64_t)(uintptr_t)c->props;
        conn.prop_values_ptr = (uint64_t)(uintptr_t)c->prop_values;
    }
    if (conn.count_encoders) {
        c->encoders = (uint32_t*)malloc(conn.count_encoders * sizeof(uint32_t));
        conn.encoders_ptr = (uint64_t)(uintptr_t)c->encoders;
    }
    
    // 再次调用获取实际数据
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
        free(c->modes);
        free(c->props);
        free(c->prop_values);
        free(c->encoders);
        free(c);
        return NULL;
    }
    
    return c;
}

static inline void drmModeFreeConnector(drmModeConnectorPtr ptr) {
    if (!ptr) return;
    free(ptr->modes);
    free(ptr->props);
    free(ptr->prop_values);
    free(ptr->encoders);
    free(ptr);
}

static inline drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoderId) {
    struct drm_mode_get_encoder enc = {};
    enc.encoder_id = encoderId;
    
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
        return NULL;
    
    drmModeEncoderPtr e = (drmModeEncoderPtr)calloc(1, sizeof(drmModeEncoder));
    if (!e) return NULL;
    
    e->encoder_id = enc.encoder_id;
    e->encoder_type = enc.encoder_type;
    e->crtc_id = enc.crtc_id;
    e->possible_crtcs = enc.possible_crtcs;
    e->possible_clones = enc.possible_clones;
    
    return e;
}

static inline void drmModeFreeEncoder(drmModeEncoderPtr ptr) {
    free(ptr);
}

static inline drmModePlaneResPtr drmModeGetPlaneResources(int fd) {
    struct drm_mode_get_plane_res res = {};
    
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) < 0)
        return NULL;
    
    drmModePlaneResPtr r = (drmModePlaneResPtr)calloc(1, sizeof(drmModePlaneRes));
    if (!r) return NULL;
    
    r->count_planes = res.count_planes;
    
    if (res.count_planes) {
        r->planes = (uint32_t*)malloc(res.count_planes * sizeof(uint32_t));
        res.plane_id_ptr = (uint64_t)(uintptr_t)r->planes;
        
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) < 0) {
            free(r->planes);
            free(r);
            return NULL;
        }
    }
    
    return r;
}

static inline void drmModeFreePlaneResources(drmModePlaneResPtr ptr) {
    if (!ptr) return;
    free(ptr->planes);
    free(ptr);
}

static inline drmModePlanePtr drmModeGetPlane(int fd, uint32_t planeId) {
    struct drm_mode_get_plane plane = {};
    plane.plane_id = planeId;
    
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) < 0)
        return NULL;
    
    drmModePlanePtr p = (drmModePlanePtr)calloc(1, sizeof(drmModePlane));
    if (!p) return NULL;
    
    p->plane_id = plane.plane_id;
    p->crtc_id = plane.crtc_id;
    p->fb_id = plane.fb_id;
    p->possible_crtcs = plane.possible_crtcs;
    p->gamma_size = plane.gamma_size;
    p->count_formats = plane.count_format_types;
    
    if (plane.count_format_types) {
        p->formats = (uint32_t*)malloc(plane.count_format_types * sizeof(uint32_t));
        plane.format_type_ptr = (uint64_t)(uintptr_t)p->formats;
        
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) < 0) {
            free(p->formats);
            free(p);
            return NULL;
        }
    }
    
    return p;
}

static inline void drmModeFreePlane(drmModePlanePtr ptr) {
    if (!ptr) return;
    free(ptr->formats);
    free(ptr);
}

static inline drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, 
                                                                     uint32_t object_id,
                                                                     uint32_t object_type) {
    struct drm_mode_obj_get_properties props = {};
    props.obj_id = object_id;
    props.obj_type = object_type;
    
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) < 0)
        return NULL;
    
    drmModeObjectPropertiesPtr p = (drmModeObjectPropertiesPtr)calloc(1, sizeof(drmModeObjectProperties));
    if (!p) return NULL;
    
    p->count_props = props.count_props;
    
    if (props.count_props) {
        p->props = (uint32_t*)malloc(props.count_props * sizeof(uint32_t));
        p->prop_values = (uint64_t*)malloc(props.count_props * sizeof(uint64_t));
        props.props_ptr = (uint64_t)(uintptr_t)p->props;
        props.prop_values_ptr = (uint64_t)(uintptr_t)p->prop_values;
        
        if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) < 0) {
            free(p->props);
            free(p->prop_values);
            free(p);
            return NULL;
        }
    }
    
    return p;
}

static inline void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr ptr) {
    if (!ptr) return;
    free(ptr->props);
    free(ptr->prop_values);
    free(ptr);
}

static inline drmModePropertyPtr drmModeGetProperty(int fd, uint32_t propertyId) {
    struct drm_mode_get_property prop = {};
    prop.prop_id = propertyId;
    
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
        return NULL;
    
    drmModePropertyPtr p = (drmModePropertyPtr)calloc(1, sizeof(drmModeProperty));
    if (!p) return NULL;
    
    p->prop_id = prop.prop_id;
    p->flags = prop.flags;
    memcpy(p->name, prop.name, DRM_PROP_NAME_LEN);
    p->count_values = prop.count_values;
    p->count_enums = prop.count_enum_blobs;
    
    return p;
}

static inline void drmModeFreeProperty(drmModePropertyPtr ptr) {
    if (!ptr) return;
    free(ptr->values);
    free(ptr->enums);
    free(ptr->blob_ids);
    free(ptr);
}

#ifdef __cplusplus
}
#endif

#endif /* _XF86DRMMODE_H_ */
