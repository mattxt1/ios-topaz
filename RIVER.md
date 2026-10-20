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
  Gold. Its expected GICv2 IRQ band was black. This is a failed diagnostic,
  not proof that the hardware cannot deliver the interrupt. A fourth image
  distinguishes execution level, GIC enable/group bits, PPI pending, and timer
  status; its full-screen photo is pending.
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

## Next work

1. Read the fourth diagnostic photo. If Gold is at EL1 and the timer is
   pending, inspect the GICv2 nonsecure Group 1 interface path. If it is not
   pending, check the PPI's group/configuration and timer routing.
2. Port the topaz kernel's GICv3 timer/IRQ path to GICv2 and INTID 20. Keep
   the first kernel boot storage-free and Gold-only, with screen markers or a
   functioning early console.
3. Adapt the shim to river's boot image and RAM placement. Do not reuse
   topaz's boot header, GIC, UFS, framebuffer, or memory addresses.
4. Bind an eMMC SDHCI driver only after early kernel execution is observable.

Temporary tests use `fastboot boot`; do not flash a partition for this
milestone. Keep live dumps, serial numbers, and ramdisk-containing images out
of git. The adjacent `outputs/river-xnu-bringup.md` report carries the
user-facing evidence and links to related projects.
