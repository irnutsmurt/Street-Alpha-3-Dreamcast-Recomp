#pragma once
#include <stdint.h>

int gdi_open(const char *path);
int gdi_read_sector(uint32_t fad, uint8_t *out);
void gdi_toc(int area, uint32_t *toc);
void gdi_override(const char *name, const char *path);  /* a root file read from path instead */
