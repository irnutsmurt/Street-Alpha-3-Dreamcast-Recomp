#pragma once
#include <stdint.h>

#include "dispatch.h"

typedef struct {
    uint32_t addr;
    GuestFn fn;
    uint32_t sig_len;
    const uint8_t *sig;  /* original bytes at addr, used to identify the overlay */
    uint32_t tag;        /* its file: g_overlay_tags[tag] ("PL1BA", "END1A") */
} OverlayEntry;

extern const OverlayEntry g_overlay_funcs[];
extern const unsigned g_overlay_count;
extern const char *const g_overlay_tags[];
extern const unsigned g_overlay_tag_count;

void overlay_file_opened(const char *name);  /* a file the game opens: an overlay's notes its slot */
