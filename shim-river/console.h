#pragma once
#include <stdint.h>

/*
 * Moto G7 river: ABL keeps scanning out its splash reservation
 * (live FDT splash_region@0x90001000, 0x13ff000 bytes) after a temporary
 * boot. Its layout is 24bpp, 3 bytes per pixel with 1080 * 3 byte rows,
 * inferred from where 32bpp diagnostic writes landed; this console is the
 * first direct test of that inference (see README.md).
 */
#define FB_BASE		0x90001000UL
#define FB_SIZE		0x013ff000UL
#define FB_W		1080
#define FB_H		2270
#define FB_BPP		3
#define FB_STRIDE	(FB_W * FB_BPP)

/* Gray levels only: identical bytes, so RGB vs. BGR order cannot matter. */
#define BLACK	0x000000U
#define GRAY	0x808080U
#define WHITE	0xffffffU

void con_clear(uint32_t bg);
void con_band(int y, int h, uint32_t c);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
