/* Character tables in the extension RAM (see chartables.h). */
#include <string.h>

#include "chartables.h"
#include "cpu.h"
#include "runtime.h"

static uint8_t *ext(uint32_t a, uint32_t n) {
    uint32_t off = a - EXT_RAM_BASE;
    return off < EXT_RAM_SIZE && n <= EXT_RAM_SIZE - off ? g_ext_ram + off : NULL;
}

void char_tables_init(void) {
    for (unsigned i = 0; i < g_char_table_count; i++) {
        const CharTable *t = &g_char_tables[i];
        uint8_t *dst = ext(t->ext, t->size * CHAR_TABLE_ENTRIES);
        if (!dst) fatal("character table %08X: bad place %08X", t->dc, t->ext);
        memcpy(dst, ram_ptr(t->dc), t->count * t->size);
        /* the new characters: Ryu's entry until the add-on says otherwise */
        for (uint32_t c = t->count; c < CHAR_TABLE_ENTRIES; c++) memcpy(dst + c * t->size, dst, t->size);
    }
    for (unsigned off = 0; off + 8 <= g_psp_char_data_size;) {
        uint32_t a, n;
        memcpy(&a, g_psp_char_data + off, 4);
        memcpy(&n, g_psp_char_data + off + 4, 4);
        uint8_t *dst = ext(a, n);
        if (!dst || off + 8 + n > g_psp_char_data_size) fatal("PSP character data: bad record at %u", off);
        memcpy(dst, g_psp_char_data + off + 8, n);
        off += 8 + ((n + 3) & ~3u);
    }
}
