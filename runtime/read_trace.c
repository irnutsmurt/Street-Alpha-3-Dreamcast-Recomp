/* Debug: which instructions read which game-data addresses (SFA3_READ_TRACE).
 *
 * The instrumented recompiled code (tools/read_trace.py) sets g_pc before
 * each instruction; R8/R16/R32 call read_trace_note for addresses in the
 * game's data and work RAM (0x8C094000-0x8C300000). Each distinct
 * (instruction, address) pair is kept once; the "rtdump <file>" control
 * command writes them out as text: "PC ADDRESS" per line, hex. Comparing two
 * runs with different characters finds the tables indexed by character. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

#ifdef SFA3_READ_TRACE
uint32_t g_pc;

#define SLOTS (1u << 24)
static uint64_t *s_set;
static uint32_t s_count;

void read_trace_note(uint32_t a) {
    if (!s_set) s_set = calloc(SLOTS, sizeof *s_set);
    if (!s_set || s_count > SLOTS / 2) return;
    uint64_t key = (uint64_t)g_pc << 32 | (a & 0x1FFFFFFFu) | 0x80000000u;
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = (uint32_t)(h >> 40) & (SLOTS - 1);; i = (i + 1) & (SLOTS - 1)) {
        if (s_set[i] == key) return;
        if (!s_set[i]) { s_set[i] = key; s_count++; return; }
    }
}

void read_trace_dump(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (uint32_t i = 0; s_set && i < SLOTS; i++)
        if (s_set[i]) fprintf(f, "%08X %08X\n", (uint32_t)(s_set[i] >> 32), (uint32_t)s_set[i]);
    fclose(f);
    logf_("read trace: %u (instruction, address) pairs to %s", s_count, path);
}
#else
void read_trace_dump(const char *path) { (void)path; }
#endif
