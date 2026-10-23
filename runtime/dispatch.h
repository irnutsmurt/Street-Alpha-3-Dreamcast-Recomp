#pragma once
#include <stdint.h>

typedef void (*GuestFn)(void);

typedef struct {
    uint32_t addr;
    GuestFn fn;
} DispatchEntry;

extern const DispatchEntry g_dispatch_main[];
extern const unsigned g_dispatch_main_count;
