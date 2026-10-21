/*
 * Moto G7 river boot shim: ABL enters on a Silver core (Cortex-A53 class,
 * no 16 KiB translation granule), so Silver only reports, finds the
 * payload and wakes Gold CPU 0x100 through PSCI, then parks. Gold checks
 * its granule and boots xnu. Nothing here touches the GIC, regulators or
 * any storage controller. The only clock write is the Gold (perf) cluster
 * mux, copied from Linux's cpu_clock_init(); the only other writes outside
 * our own image are the splash framebuffer and, for a valid payload, the
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

/*
 * ABL runs only on Silver and never programs the Gold cluster clock; Linux
 * does it in clock-cpu-sdm632.c cpu_clock_init() (an early_initcall, before
 * any regulator/CPR or cpufreq work): APCS_ALIAS1 RCG source GPLL0 (sel 4),
 * divider 1 (800 MHz), update, then enable the core branch. Without it Gold
 * runs from the reset source, measured here with spin_mhz(). Same values and
 * order as Linux; no PLL, voltage or CPR register is touched.
 */
#define APCS_ALIAS1_CMD_RCGR	0x0b011050UL
#define APCS_ALIAS1_CFG_RCGR	(APCS_ALIAS1_CMD_RCGR + 4)
#define APCS_ALIAS1_CORE_CBCR	(APCS_ALIAS1_CMD_RCGR + 8)
#define APCS_CCI_CMD_RCGR	0x0b1d1050UL
#define RCG_GPLL0_DIV1		((4U << 8) | 1U)

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
#define dsb() __asm__ volatile("dsb sy" ::: "memory")

/* Effective MHz of a 1-cycle dependent loop (A53 and A73 both retire subs+b.ne in ~1 cycle). */
static uint32_t spin_mhz(void)
{
	uint64_t n = 1000000;
	isb();
	uint64_t t0 = rsr("cntpct_el0");
	__asm__ volatile("1: subs %0, %0, #1\n b.ne 1b" : "+r"(n) :: "cc");
	isb();
	uint64_t ticks = rsr("cntpct_el0") - t0;
	uint64_t hz = rsr("cntfrq_el0");
	return ticks ? (uint32_t)(hz / ticks) : 0;	/* 1e6 iterations: MHz = hz / ticks */
}

static void gold_clock_gpll0(void)
{
	con_hex2("C1RCG", "cmd=", rd32(APCS_ALIAS1_CMD_RCGR), "cfg=", rd32(APCS_ALIAS1_CFG_RCGR));
	con_hex2("C1RCG", "cbcr=", rd32(APCS_ALIAS1_CORE_CBCR), "ccicfg=", rd32(APCS_CCI_CMD_RCGR + 4));
#ifndef RIVER_NO_GOLD_CLK
	wr32(APCS_ALIAS1_CFG_RCGR, RCG_GPLL0_DIV1);
	dsb();
	wr32(APCS_ALIAS1_CMD_RCGR, rd32(APCS_ALIAS1_CMD_RCGR) | 1U);
	dsb();
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000;	/* Linux: 500 x 1 us */
	while ((rd32(APCS_ALIAS1_CMD_RCGR) & 1U) && rsr("cntpct_el0") < end)
		isb();
	wr32(APCS_ALIAS1_CORE_CBCR, rd32(APCS_ALIAS1_CORE_CBCR) | 1U);
	dsb();
	con_hex2("C1GPLL0", "cmd=", rd32(APCS_ALIAS1_CMD_RCGR), "cfg=", rd32(APCS_ALIAS1_CFG_RCGR));
	if (rd32(APCS_ALIAS1_CMD_RCGR) & 1U)
		con_str("C1GPLL0", "update bit stuck (continuing)");
#else
	con_str("C1RCG", "left at reset source (RIVER_NO_GOLD_CLK)");
#endif
}

/*
 * Gold at 1401.6 MHz (corner c4) from its own APCS C1 PLL. The shared APC
 * rail (PM8953 S5) stays as ABL left it: r21 read 870 mV, above the c4 CPR
 * ceiling of 865 mV in river's DT, so no voltage change is needed. Sequence
 * and values from Linux clock-cpu-sdm632.c / clock-pll.c (variable-rate PLL
 * init, set_rate, software enable), then the mux to source 5 (PLL main),
 * divider 1. On a lock failure the PLL goes back into reset and Gold stays
 * on GPLL0 800 MHz. Build with --gold-mhz 800 to skip this.
 */
#define APCS_C1_PLL		0x0b016000UL
#define PLL_MODE		0x00
#define PLL_L_VAL		0x08
#define PLL_ALPHA		0x10
#define PLL_USER_CTL		0x18
#define PLL_CFG_CTL_LO		0x20
#define PLL_CFG_CTL_HI		0x24
#define PLL_TEST_CTL_LO		0x30
#define PLL_TEST_CTL_HI		0x34
#define PLL_OUTCTRL		(1U << 0)
#define PLL_BYPASSNL		(1U << 1)
#define PLL_RESET_N		(1U << 2)
#define PLL_LOCKED		(1U << 31)
#define GOLD_PLL_L		73U			/* 73 x 19.2 MHz = 1401.6 MHz */
#define RCG_PLL_MAIN_DIV1	((5U << 8) | 1U)

static void __attribute__((unused)) delay_us(unsigned us)
{
	isb();
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") * us / 1000000;
	while (rsr("cntpct_el0") < end)
		isb();
}

static int rcg_switch(uint32_t cfg)
{
	wr32(APCS_ALIAS1_CFG_RCGR, cfg);
	dsb();
	wr32(APCS_ALIAS1_CMD_RCGR, rd32(APCS_ALIAS1_CMD_RCGR) | 1U);
	dsb();
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000;
	while ((rd32(APCS_ALIAS1_CMD_RCGR) & 1U) && rsr("cntpct_el0") < end)
		isb();
	return (rd32(APCS_ALIAS1_CMD_RCGR) & 1U) ? -1 : 0;
}

static volatile uint32_t gold_mhz_target = 800;

static void gold_clock_pll(void)
{
#if !defined(RIVER_NO_GOLD_CLK) && !defined(RIVER_GOLD_GPLL0_ONLY)
	uintptr_t p = APCS_C1_PLL;
	if (rd32(p + PLL_MODE) & PLL_OUTCTRL) {
		con_hex("C1PLL", rd32(p + PLL_MODE));
		con_str("C1PLL", "already running; left alone");
		return;
	}
	/* __variable_rate_pll_init */
	wr32(p + PLL_USER_CTL, rd32(p + PLL_USER_CTL) | (1U << 0) | (1U << 3));
	wr32(p + PLL_ALPHA, 0);
	wr32(p + PLL_CFG_CTL_LO, 0x200D4828U);
	wr32(p + PLL_CFG_CTL_HI, 0x00000006U);
	wr32(p + PLL_TEST_CTL_LO, 0x1C000000U);
	wr32(p + PLL_TEST_CTL_HI, 0x00004000U);
	/* variable_rate_pll_set_rate */
	wr32(p + PLL_L_VAL, GOLD_PLL_L);
	dsb();
	/* variable_rate_pll_clk_enable (test_ctl_dbg set: no debug bits) */
	uint32_t mode = rd32(p + PLL_MODE);
	mode |= PLL_BYPASSNL;
	wr32(p + PLL_MODE, mode);
	dsb();
	delay_us(10);
	mode |= PLL_RESET_N;
	wr32(p + PLL_MODE, mode);
	dsb();
	delay_us(200);
	int locked = 0;
	for (int i = 0; i < 200 && !locked; i++) {
		if (rd32(p + PLL_MODE) & PLL_LOCKED) {
			delay_us(1);
			locked = (rd32(p + PLL_MODE) & PLL_LOCKED) != 0;	/* not a transient */
		}
		if (!locked)
			delay_us(1);
	}
	if (!locked) {
		con_hex2("C1PLL", "mode=", rd32(p + PLL_MODE), "L=", rd32(p + PLL_L_VAL));
		wr32(p + PLL_MODE, 0);	/* back into reset/bypass */
		dsb();
		con_str("C1PLL", "no lock: Gold stays on GPLL0 800 MHz");
		return;
	}
	wr32(p + PLL_MODE, mode | PLL_OUTCTRL);
	dsb();
	if (rcg_switch(RCG_PLL_MAIN_DIV1)) {
		(void)rcg_switch(RCG_GPLL0_DIV1);
		con_str("C1PLL", "mux update stuck: back to GPLL0 800 MHz");
		return;
	}
	gold_mhz_target = 1401;
	con_hex2("C1PLL", "mode=", rd32(p + PLL_MODE), "cfg=", rd32(APCS_ALIAS1_CFG_RCGR));
	con_str("C1PLL", "Gold on PLL 1401.6 MHz");
#endif
}

/*
 * Integrity check for the new clock: Silver computes this at its known-good
 * 800 MHz before CPU_ON; Gold recomputes it at the new clock and falls back
 * to GPLL0 on any difference (multiply, shift and load/store paths).
 */
static uint64_t clock_check(void)
{
	static uint64_t buf[512];
	uint64_t x = 0x9e3779b97f4a7c15ULL, acc = 0;
	for (uint32_t i = 0; i < 512; i++)
		buf[i] = i * 0x100000001b3ULL;
	for (uint32_t i = 0; i < (1U << 20); i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		uint32_t k = (uint32_t)(x >> 55);
		buf[k] = buf[k] * 0xff51afd7ed558ccdULL + x;
		acc += buf[(k + i) & 511] ^ (x * 0xc4ceb9fe1a85ec53ULL);
	}
	return acc;
}
static volatile uint64_t gold_ref;

static void report_cpu(const char *label)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	con_hex2(label, "midr=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2(" ", "el=", (uint32_t)(rsr("CurrentEL") >> 2), "tg16=", (uint32_t)((mmfr0 >> 20) & 0xf));
	con_hex(" spin MHz", spin_mhz());
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
	if (gold_mhz_target != 800) {
		/* Three passes at the new clock must match Silver's 800 MHz result. */
		int bad = 0;
		for (int i = 0; i < 3; i++)
			bad |= clock_check() != gold_ref;
		if (bad) {
			(void)rcg_switch(RCG_GPLL0_DIV1);
			con_str("C1CHECK", "MISMATCH at 1401.6 MHz: Gold back on GPLL0 800 MHz");
		} else {
			con_str("C1CHECK", "3/3 checks match at 1401.6 MHz");
		}
		con_hex(" spin MHz", spin_mhz());
	}
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

	stage = "gold_clk";
	gold_clock_gpll0();
	gold_clock_pll();
	gold_ref = clock_check();
	dsb();

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
