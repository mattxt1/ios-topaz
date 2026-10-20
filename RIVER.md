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
platform expert. The output is only an early boot scaffold.

Generate and parse-check the tree:

```sh
python3 tools/mkadt_river.py /tmp/river-early.adt
```

`xnu/patches/0127-river-gicv2-early.patch` is a **draft** kernel change for
`MACHINE_CONFIG=RIVER`. It reuses the SWEET platform code with a Cortex-A53
instruction baseline, switches off GICv3, maps river's GICv2 distributor and
CPU interface, and routes the virtual timer through `sleh_irq`. It applies
cleanly to the exact topaz build tree from CI run 39292140455. It has not
been compiled or booted; the current CI log does not contain verbose compile
commands, and local Xcode differs from the pinned CI toolchain. Review the
GICv2 register path and obtain a RIVER CI build before any XNU boot attempt.

## Next work

1. Compile and review the draft GICv2 kernel patch, then use a storage-free,
   Gold-only first boot with screen markers or a functioning early console.
   The handler acknowledges and EOIs via GICC and masks unrelated sources.
2. Adapt the shim to river's boot image and RAM placement. Do not reuse
   topaz's boot header, GIC, UFS, framebuffer, or memory addresses.
3. Bind an eMMC SDHCI driver only after early kernel execution is observable.

Temporary tests use `fastboot boot`; do not flash a partition for this
milestone. Keep live dumps, serial numbers, and ramdisk-containing images out
of git. The adjacent `outputs/river-xnu-bringup.md` report carries the
user-facing evidence and links to related projects.
