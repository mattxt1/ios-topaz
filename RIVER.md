# Moto G7 river bring-up (local branch)

19 October 2026. This branch borrows the topaz XNU work as a starting point;
it has not booted XNU on river. Device dumps and boot images stay outside git
in the adjacent private `work/river-device` and `work/river-port` directories.

## Direct device observations

- XT1962-1, SDM632, LineageOS 22.2 (Android 15), unlocked fastboot, slot b.
- Silver MIDR `0x51af8014`, MPIDR `0`–`3`; Gold MIDR `0x51af8002`,
  MPIDR `0x100`–`0x103`.
- Two temporary `fastboot boot` images show that PSCI `CPU_ON(0x100)` reaches
  Gold, Silver lacks 16 KiB translation, and Gold supports it. XNU's existing
  16 KiB path should start on Gold and leave Silver parked initially.
- Live FDT: GICv2 `qcom,msm-qgic2`, distributor `0x0b000000`, CPU interface
  `0x0b002000`; `/soc/timer` frequency 19.2 MHz and virtual PPI 4 (INTID 20).
  Android `/proc/interrupts` also shows `arch_timer` on hardware IRQ 20.
- Third temporary image proves the virtual timer expires under polling on
  Gold. Its expected GICv2 IRQ band was black. A fourth image shows Gold at
  EL1 and GIC Group 1/enable bits set, but no final pending PPI/timer status.
  The first handler disarmed the timer for *any* IRQ, so a different pending
  interrupt could have caused that result. A fifth RAM-only image records and
  masks unrelated IRQs while leaving the timer armed. Its photo shows a white
  expected-IRQ band: Gold received virtual timer INTID 20 via GICv2.
- eMMC SDHCI `0x07824900`, not UFS. Live splash reservation `0x90001000` +
  `0x013ff000` is visibly scanned out after temporary boot. Android fb0 is
  1080×2270, 32 bpp, stride 4352; bootloader splash pixel layout differs.
- Boot image uses Android header v1 and 2048-byte packing, gzip ARM64 Image
  with a base DTB, plus separate DTBO partition. No partition has been flashed.

## Added code

`tools/mkadt_river.py` builds a small Apple Device Tree from the topaz
generator using a conservative 0xa0000000–0xee000000 RAM window, four Gold
cores, GICv2 registers, timer INTID 20, and framebuffer metadata. It removes
the inherited UFS node. The `D22AP`/`iPhone10,3` identity and `xiaomi,sweet`
compatible remain placeholders used by the existing XNU configuration and
platform expert. Its `sweet-fbcon` node describes ABL's splash as 24bpp
(`stride` 3240, `bytes-per-pixel` 3). The output is only an early boot
scaffold.

```sh
python3 tools/mkadt_river.py /tmp/river-early.adt
```

### Kernel patch 0127 (reworked 2026-10-20)

The first draft added `MACHINE_CONFIG=RIVER`. Review against the topaz build
tree showed that would not build: there is no `config/MASTER.arm64.river`
(doconf would drop SWEET's mockfs/HFS/pthread options) and
`bsd/conf/Makefile.template` gates SWEET's pthread include flags on
`CURRENT_MACHINE_CONFIG == SWEET`. River is now a **board variant of
MACHINE_CONFIG=SWEET**: `SWEET_BOARD=river` in the environment makes
`MakeInc.def` add `-DRIVER=1`, and the CI workflow has a `board` input.
The kernel artifact is still `kernel.development.sweet`.

`xnu/patches/0127-river-gicv2-early.patch`, all under `#if RIVER` unless noted:

| Area | Change |
| --- | --- |
| `SWEET.h` | 4 CPUs max, `HAS_GIC_V3 0`, vtimer INTID 20, GICv2 offsets; early-console framebuffer constants for both boards (topaz values unchanged) |
| `pe_fiq.c` | GICv2 init: maps GICD/GICC from `/arm-io/gic`, records each Gold core's CPU-interface bit from banked `GICD_ITARGETSR0`, enables vtimer + SGI1, Group 1, PMR 0xff; `river_gic_send_sgi()` |
| `sleh.c` | IRQ via `GICC_IAR`/`GICC_EOIR` (whole IAR, so SGI source ID round-trips); unhandled sources masked |
| `SweetCPU.cpp` | IPIs through `GICD_SGIR`. **The draft missed this**: `initCPU` sends a self-IPI even with `cpus=1`, and the old `ICC_SGI1R_EL1` write is UNDEFINED on a GICv2-only core |
| `start.s` | Stage-0 band and the 32 MB Normal-NC splash block use the board constants (river: 0x90001000 in block 0x90000000); EL2 secondary path skips `ICC_SRE_EL2` |
| `sweet_early.c`, `arm_vm_init.c` | Early console draws 3- or 4-byte pixels; the post-TTBR0 alias keeps the splash offset inside its block. **The draft would have drawn at topaz's 0x9c000000 with 32bpp/4320 stride** |
| `pe_serial.c` (both boards) | `sweet-fbcon` honours an optional `bytes-per-pixel` (default 4) and checks the node's size |
| `KernelConfigTables.cpp` | River drops AppleCLCD (topaz MDP/DSI + TopazModem), SweetBattery (topaz cpufreq/I2C/SPMI MMIO) and the topaz HID services |
| `sweet_ufs.c` | River `sweet_ufs_init` returns `ENODEV` before any mapping, which also keeps the UFS log, sampler, touch, keys and USB probes off |

Hardware audit, everything else SWEET-specific: `sweet_cpufreq`, `sweet_i2c1`,
`sweet_spmi`, `sweet_rpmh`, `sweet_touch`, `sweet_keys`, `sweet_usb` and
`TopazModem` are reachable only through SweetBattery, AppleCLCD or
`sweet_ufs_init`, all disabled above. IOWatchdog, AppleKeyStore,
IOCoreSurfaceRoot, AMFI, HFS, crash/log/sample code are software only.
`ICC_*` system registers no longer appear in any river object. Topaz's Gold
core (Snapdragon 685, Kryo 265) is the same Cortex-A73 class as river's
(Kryo 250), so the CPU-level SWEET code is already proven on that core.

Local compile check (`tools/river-cc.py`, the CI build tree's recorded flags,
local Apple clang 21): all ten changed sources compile for topaz and for
`-DRIVER=1`. For topaz, every changed object except `sweet_early.c` and
`pe_serial.c` (refactored pixel writes) is instruction-identical to the
baseline apart from `__LINE__` constants and an equivalent `MOV64`. This is
not a CI build or link; a newer clang can accept what Xcode 15 rejects.

```sh
tar xzf ../xnu-buildtree/buildtree.tgz -C /tmp/bt   # apply 0127 in /tmp/bt/xnu first
python3 tools/river-cc.py /tmp/bt -DRIVER=1 pexpert/arm/pe_fiq.c osfmk/arm64/sleh.c \
    osfmk/arm64/start.s iokit/Kernel/arm/SweetCPU.cpp
```

### River shim (`shim-river/`)

A new header-v1 boot shim; it reuses `shim/start.S`, `fdt.c` and the font.
Silver clears the splash to black, draws calibration bands, prints its
MIDR/MPIDR/EL/TGran16, PSCI version and the `/chosen` initrd range, then
`CPU_ON(0x100)` and parks. Gold prints its IDs, requires TGran16, and runs
`loader.c`, which validates the SWEETXNU payload (the boot image ramdisk) and
refuses any write outside 0xa0000000–0xee000000 before copying. Boot args
carry no boot video (`v_baseAddr = 0`): xnu then forces serial mode instead
of mapping a 24bpp buffer its video console cannot draw. Without a payload,
or on any error or exception, it holds the screen 60 s and then calls PSCI
`SYSTEM_RESET`, which returns to Android. It touches no GIC, clock, regulator
or storage register.

Qualcomm ABL (`QcomModulePkg` `UpdateBootParams`) places the ramdisk at the
end of a fixed kernel buffer, the DTB 2 MB below it, and decompresses the
kernel at the start; the payload must fit in that buffer (likely below
0x84300000 on river), and ABL refuses the boot rather than overlap. That is
a fastboot-side failure, not a phone-state risk.

```sh
python3 shim-river/build.py --boot ../device-private/boot_b.img \
    --dtb ../device-private/boot_b-kernel.dtb            # shim-only test image
REPO=<fork>/ios-topaz tools/kbuild-river.sh r1           # CI kernel + XNU image
```

Images land in `shim-river/out/` (ignored: they embed the phone's header and
DTB). Builds need `ld.lld` (`brew install lld`).

### Shim-only RAM test (2026-10-20, photo IMG_0321)

`fastboot boot` of the shim-only image (SHA-256 `a6f72316…c969e27b`)
returned OKAY; nothing was flashed. The photo shows crisp, unsheared text on
black, so **ABL's splash is confirmed 24bpp with 3240-byte rows at
0x90001000**, as patch 0127 and the ADT now assume. Read from the screen:

| Line | Value | Meaning |
| --- | --- | --- |
| SILVER | MIDR 0x51AF801x, MPIDR 0x80000000, EL1, TGran16 0 | as before |
| PSCI | version 0x00010000, DTB at 0x13400000 | PSCI 1.0 via SMC |
| INITRD | 0x13600000–0x1360002E | 46-byte test ramdisk |
| GOLD | MIDR 0x51AF8002, MPIDR 0x80000100, EL1, TGran16 1 | handoff and 16 KiB again confirmed |
| STOP / RESET | "no payload: shim-only test done" | expected end |

The phone then reset itself through PSCI `SYSTEM_RESET` and was back on
ADB in Android. ABL places the kernel buffer in **low RAM**, not near
0x80000000: the DTB sits exactly 2 MB below the ramdisk, matching
`UpdateBootParams`, so the buffer ends near 0x13601000. A payload
of roughly 45–50 MB should still fit (the shim needs only 80 KB), and the
initrd is far below the 0xa0000000 kernel window.

## Next work

1. **CI build** with `board=river` on a fork of ios-topaz (the upstream repo
   is MakrSas's). Check the log for `-DRIVER=1`, then `kbuild-river.sh`.
2. **First XNU RAM test** with `cpus=1` and no root device. Success is the
   SWEET EARLY stage bands at the bottom edge and fbcon text, ending in a
   root-device wait or panic. Hold power to return to Android if it hangs.
3. Bind an eMMC SDHCI driver only after early kernel execution is observable.
