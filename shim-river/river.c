/*
 * Moto G7 river boot shim: ABL enters on a Silver core (Cortex-A53 class,
 * no 16 KiB translation granule), so Silver only reports, finds the
 * payload and wakes Gold CPU 0x100 through PSCI, then parks. Gold checks
 * its granule and boots xnu. Nothing here touches the GIC, clocks,
 * regulators or any storage controller; the only writes outside our own
 * image are the splash framebuffer and, for a valid payload, the
 * 0xa0000000-0xee000000 kernel window (loader.c).
 *
 * Without a payload (or on any failure) the screen keeps the report for
 * 60 s, then PSCI SYSTEM_RESET reboots into the installed Android.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_VERSION		0x84000000UL
#define GOLD0_MPIDR		0x100UL
#define HOLD_S			60

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t gold_dtb, rd_lo, rd_hi;

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void delay_s(unsigned s)
{
	isb();
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") * s;
	while (rsr("cntpct_el0") < end)
		isb();
}

static void __attribute__((noreturn)) hold_and_reset(const char *why)
{
	con_skip();
	con_str("STOP", why);
	con_str("RESET", "Android in 60 s");
	__asm__ volatile("dsb sy" ::: "memory");
	delay_s(HOLD_S);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfi");
}

static void report_cpu(const char *label)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	con_hex2(label, "midr=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2(" ", "el=", (uint32_t)(rsr("CurrentEL") >> 2), "tg16=", (uint32_t)((mmfr0 >> 20) & 0xf));
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_skip();
	con_hex2("EXCEPT", "slot=", (uint32_t)slot, "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	hold_and_reset("exception");
}

/* Gold, from start.S secondary_entry (x0 = context = 4). */
void secondary_main(uint64_t idx)
{
	(void)idx;
	stage = "gold";
	con_skip();
	report_cpu("GOLD");
	if (!gold_dtb)
		hold_and_reset("gold woke without a handoff");
	if (((rsr("id_aa64mmfr0_el1") >> 20) & 0xf) == 0)
		hold_and_reset("gold lacks 16K granule");
	if (!rd_lo)
		hold_and_reset("no payload: shim-only test done");
	stage = "loader";
	hold_and_reset(loader_boot(rd_lo, rd_hi));
}

/* Silver boot CPU, from start.S _start (x0 = DTB from ABL). */
void shim_main(uint64_t dtb)
{
	stage = "silver";
	con_clear(BLACK);
	/* Calibration bands: 8 px white, 8 px black, 8 px gray, full width. */
	con_band(FB_H - 24, 8, WHITE);
	con_band(FB_H - 8, 8, GRAY);
	con_str("RIVER", "xnu boot shim, RAM only");
	report_cpu("SILVER");
	con_hex2("PSCI", "ver=", (uint32_t)smc(PSCI_VERSION, 0, 0, 0), "dtb=", (uint32_t)dtb);

	uintptr_t lo = 0, hi = 0;
	const char *why = loader_find(dtb, &lo, &hi);
	if (why)
		con_str("PAYLOAD", why);
	else
		con_hex2("PAYLOAD", "at=", (uint32_t)lo, "len=", (uint32_t)(hi - lo));
	rd_lo = why ? 0 : lo;
	rd_hi = why ? 0 : hi;

	stage = "cpu_on";
	gold_dtb = dtb;
	__asm__ volatile("dsb sy" ::: "memory");
	int64_t r = (int64_t)smc(PSCI_CPU_ON, GOLD0_MPIDR, (uintptr_t)secondary_entry, 4);
	if (r != 0) {
		con_hex("CPU_ON", (uint64_t)r);
		hold_and_reset("PSCI CPU_ON(0x100) failed");
	}
	/* Parked with DAIF masked (start.S); xnu never learns about Silver. */
	for (;;)
		__asm__ volatile("wfi");
}
