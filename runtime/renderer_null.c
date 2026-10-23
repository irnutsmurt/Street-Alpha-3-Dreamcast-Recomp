/* Placeholder renderer used during bring-up (no output). */
#include "pvr.h"

int renderer_init(void *window) { (void)window; return 1; }
void renderer_draw(const PvrFrame *frame) { (void)frame; }
void renderer_present_fb(void) {}
void renderer_shutdown(void) {}
void renderer_vram_written(uint32_t off, int size) { (void)off; (void)size; }
void renderer_vram_block_written(uint32_t off, uint32_t bytes) { (void)off; (void)bytes; }
void renderer_settings_changed(void) {}
uint64_t renderer_frame_ns(void) { return 0; }
