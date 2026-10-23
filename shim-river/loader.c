/*
 * Act as iBoot for xnu on river (derived from shim/loader.c).
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT. River's ABL passes the boot image's ramdisk (header v1) through
 * /chosen linux,initrd-start/end, so the SWEETXNU payload is the ramdisk.
 * Copy the segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump with the MMU and caches off,
 * x0 = boot_args (physical). Every destination is checked against the one
 * RAM window that river's live device tree leaves free of firmware
 * carve-outs, so a wrong payload cannot write over modem/TZ memory.
 */
#include <stdint.h>
#include "console.h"
#include "../shim/fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

/*
 * Low DRAM above ABL's boot buffers (shim, DTB and ramdisk end below
 * 0x16000000) and below River's GPU zap shader at 0x81800000, the first
 * fixed carve-out (firmware regions start at 0x84300000).
 */
#define RIVER_RAM_LO		0x18000000UL
#define RIVER_RAM_HI		0x81800000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(uintptr_t a)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (((volatile const char *)a)[i] != want[i])
			return 0;
	return 1;
}

static int in_window(uint64_t lo, uint64_t size)
{
	return lo >= RIVER_RAM_LO && size <= RIVER_RAM_HI - lo;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/* Find the payload; 0 on success, else a reason. */
const char *loader_find(uintptr_t dtb, uintptr_t *start, uintptr_t *end)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", (uint32_t)rd_start, "hi=", (uint32_t)rd_end);
	if (rd_end <= rd_start || (rd_start >> 32) || (rd_end >> 32))
		return "bad initrd range";
	if (rd_end > RIVER_RAM_LO && rd_start < RIVER_RAM_HI)
		return "initrd overlaps kernel window";
	if ((rd_start & 7) || !magic_ok(rd_start))
		return "no SWEETXNU payload";
	*start = rd_start;
	*end = rd_end;
	return 0;
}

const char *loader_boot(uintptr_t rd_start, uintptr_t rd_end)
{
	const struct payload *p = (const struct payload *)rd_start;
	if (p->version != 1 || (p->adt_off & 7) || p->nsegs == 0 || p->nsegs > 64)
		return "bad payload header";
	if (p->phys_base != RIVER_RAM_LO || !in_window(p->phys_base, p->mem_size))
		return "payload RAM is not river's window";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", (uint32_t)p->phys_base, "size=", (uint32_t)p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* Validate everything before the first write. */
	uint64_t len = rd_end - rd_start;
	if (p->adt_off + p->adt_size > len)
		return "ADT outside payload";
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		if (s->file_off + s->file_size > len || s->file_size > s->vmsize ||
		    !in_window(s->vmaddr + pa_of_va_base, s->vmsize))
			return "segment outside window";
	}
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;
	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	if (!in_window(adt_pa, top - adt_pa) || !in_window(entry_pa, 4))
		return "ADT/boot_args outside window";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", (uint32_t)(p->kernel_end + pa_of_va_base));

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	/*
	 * No boot video (zeroed above): xnu's video console only draws 8/16/32
	 * bpp, and with v_baseAddr == 0 initialize_screen() forces serial mode
	 * instead of mapping this 24bpp scanout. Output goes through the SWEET
	 * early console and the ADT's sweet-fbcon node (bytes-per-pixel = 3).
	 */
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", (uint32_t)ba_pa, "top=", (uint32_t)top);
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
