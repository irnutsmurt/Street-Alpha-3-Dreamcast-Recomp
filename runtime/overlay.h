#pragma once
#include <stdint.h>

#include "dispatch.h"

typedef struct {
    uint32_t addr;
    GuestFn fn;
    uint32_t sig_len;
    const uint8_t *sig;  /* original bytes at addr, used to identify the overlay */
} OverlayEntry;

extern const OverlayEntry g_overlay_funcs[];
extern const unsigned g_overlay_count;
