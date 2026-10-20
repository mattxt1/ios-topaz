/*
 * Text console on river's 24bpp ABL splash scanout. With the MMU off every
 * access is Device memory, so only byte stores are used (-mstrict-align).
 * Labels are gray and values white on black, so the panel's channel order
 * is irrelevant and a correct 3-byte layout shows crisp glyphs; a wrong
 * stride or depth shows sheared or repeated text instead.
 */
#include "console.h"
#include "../shim/font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8
/* Leave the bottom band row free for the XNU stage markers. */
#define TEXT_H		(FB_H - 64)

static uint32_t con_bg;
static int row;

static void pixel(int x, int y, uint32_t c)
{
	volatile uint8_t *p = (volatile uint8_t *)FB_BASE + (uintptr_t)y * FB_STRIDE + (uintptr_t)x * FB_BPP;
	p[0] = (uint8_t)c;
	p[1] = (uint8_t)(c >> 8);
	p[2] = (uint8_t)(c >> 16);
}

void con_band(int y, int h, uint32_t c)
{
	for (int yy = y; yy < y + h && yy < FB_H; yy++)
		for (int x = 0; x < FB_W; x++)
			pixel(x, yy, c);
}

void con_clear(uint32_t bg)
{
	con_band(0, FB_H, bg);
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > TEXT_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					pixel(x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, c);
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, GRAY);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, GRAY);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, GRAY);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, GRAY);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}
