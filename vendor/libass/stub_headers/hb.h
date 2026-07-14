/*
 * Stub HarfBuzz header for libass compilation
 * This is a minimal header to allow libass to compile without harfbuzz
 */

#ifndef HB_H
#define HB_H

#include <stdint.h>
#include <stdbool.h>

/* Minimal HarfBuzz types needed by libass */
typedef void* hb_buffer_t;
typedef void* hb_font_t;
typedef void* hb_face_t;
typedef void* hb_blob_t;
typedef void* hb_font_funcs_t;
typedef uint32_t hb_script_t;
typedef uint32_t hb_language_t;
typedef uint32_t hb_codepoint_t;

/* Feature structure */
typedef struct {
    uint32_t tag;
    uint32_t value;
    unsigned int start;
    unsigned int end;
} hb_feature_t;

/* Script constants */
#define HB_SCRIPT_UNKNOWN 0x00000000

/* Function stubs - these will not be called when CONFIG_HARFBUZZ=0 */
static inline const char* hb_version_string() { return "0.0.0"; }

#endif /* HB_H */
