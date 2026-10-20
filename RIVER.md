# Moto G7 river bring-up (local branch)

Started 19 October 2026. This branch borrows the topaz XNU work as a starting
point. River now runs iOS 16.7.16 services from userdata and displays the
Apple logo; graphical startup remains incomplete. Device dumps, photos,
logs, Apple artifacts and boot images stay in the adjacent private handoff
folders or ignored output directories.

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

### First river kernel build (2026-10-20)

Fork `mattxt1/ios-topaz`, branch `river-bringup` at `00ff667`, workflow run
39368830465 with `board=river`: **success**, no compile errors. The log shows
`-DRIVER=1`. `kernel.development.sweet` (17,776,752 bytes, SHA-256
`11b5b18ed4f3e5c78aa489f025d318fa9788c5ccb88ba53a2d43b04939cdfe18`) contains the `RIVER GIC` strings and no `ICC_*`/GICv3
system-register instructions. Its bytes include the river stage-0 constants
(0x906eb4b0, stride 3240, block 0x90000000) and none of topaz's 0x9c000000.
The payload (17,161,840 bytes, boot-args `debug=0xc serial=3 cpus=1
keepsyms=1 -noprogress wdt=-1`) and the image
`shim-river/out/river-r1-boot.img` were built locally; the kernel lives in
`../k-r1/`.

### First XNU boot on river (2026-10-20, photo IMG_0322)

`fastboot boot shim-river/out/river-r1-boot.img` (SHA-256 `cd97ebb9…0b918270`)
returned OKAY; nothing was flashed. **XNU booted on Gold CPU 0x100 to the
root-device wait.** The 24bpp framebuffer console (fbcon wraps, so the
newest line sits just above the blank gap) shows, among others:

- `SWEET CPU: cpu=0 mpidr=0x100 came alive; SGI1 OK` and `1/1 CPUs
  initialized, SGI1 verified`: GICv2 IPIs through `GICD_SGIR` work.
- Scheduler, timers and kernel threads run (`dlil` threads scheduled,
  `standard timeslicing quantum is 10000 us`, `mbinit`, `mcache: 1 CPU(s)`),
  so the virtual timer IRQ (INTID 20) drives XNU.
- `SWEET AMFI`, `SWEET AKS`, `SWEET WDT`, `SWEET SURF`, `SWEET Sandbox`,
  `SWEET HFS hfs-627.100.6`, MAC framework and the platform expert start.
- The green stage-marker band along the bottom edge (early console stages).
- Last line: `Waiting on <dict>…IOMedia…Apple_HFS…</dict>` then
  `Failed to write boot-picker-bringup-reason to NVRAM.`: no root device,
  exactly what a storage-free kernel without `rd=` should do.

The phone stays at that wait; hold power to reboot into Android.

### First userspace on river (2026-10-20, photo IMG_0323)

Same kernel and ADT, payload built with `--ramdisk ../k-r1/sweet-init` (the
static PID 1 from `tools/sweet-init.S` + `tools/mkinit.py`, built with local
clang `-target arm64-apple-ios16.0`) and boot-args `... cpus=1 ... rd=md0`.
Image `river-r1-md0-boot.img` SHA-256 `4f8aa497…6bfb311`, RAM-booted. Screen:
`Added memory device md0/rmd0 … at 0xFFFFFE0008568000`, `BSD root: md0,
major 1, minor 0`, `load_init_program: attempting to load /sbin/launchd`,
**`*** SWEET userspace: init running as PID 1 ***`**, then repeated
`SWEET init: still alive`. Exec, code signing (ad-hoc), syscalls, devfs
`/dev/console` and timed `select()` all work on river. No storage touched,
nothing flashed.

### All four Gold cores (2026-10-20, photo IMG_0324)

Same kernel, boot-args `debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress
wdt=-1`, image `river-r1-smp-boot.img` SHA-256 `66f2c3da…`, RAM-booted.
`SWEET AKS: requested=4 selected=4 (limit=4)`; for cpu=1/2/3 `CPU_ON
target=0x101/0x102/0x103 entry_pa=0xa14b8000`, `PSCI CPU_ON rc=0`, `mpidr=…
came alive; SGI1 OK`; then **`SWEET CPU: 4/4 CPUs initialized, SGI1
verified`** and `mcache: 4 CPU(s)`. Boot continues to the same root-device
wait. Cross-CPU GICv2 SGIs (each core's banked `GICD_ITARGETSR0`
interface bit) and secondary PSCI entry at EL1 work. Silver stays parked.

### HFS+ root from RAM, four cores (2026-10-20, photo IMG_0325)

An 8 MB case-sensitive, non-journaled HFS+ volume `SweetRoot` made with
macOS `hdiutil` (`tools/mkroot-macos.sh`; `/sbin/launchd` = sweet-init,
plus `/dev`, `/private/var`, `/tmp`), passed with `--ramdisk` and boot-args
`… cpus=4 … rd=md0`. Image `river-r1-hfs-boot.img` SHA-256 `e5ee5782…`.
Screen: 4/4 CPUs, `BSD root: md0, major 1, minor 0`, **`hfs: mounted
SweetRoot on device b(1, 0)`**, `load_init_program: attempting to load
/sbin/launchd`, `*** SWEET userspace: init running as PID 1 ***`, `SWEET
init: still alive`. The built-in HFS works on river; this is the same
milestone that preceded iOS userland on sweet.

### eMMC read-only probe, first run (2026-10-20, photo IMG_0326)

Patch 0128 (`bsd/dev/river_emmc.c`, CI run 39371894070, kernel in
`../k-r2/`), ADT `/river-emmc`, boot-args `… cpus=4 … rd=md0 rivermmc=1`,
image `river-r2-emmc-boot.img` SHA-256 `b0c371ab…`. Screen:

    RIVER eMMC: GCC SDCC1 apps CBCR 0x00004ff1, ahb CBCR 0x2000cff1
    RIVER eMMC: MSM core HC_MODE 0x0 is not SDHCI mode; stopping

Both SDCC1 clock branches are running (CLK_OFF clear). But the MSM core is
**not in SDHCI mode** when XNU starts (`HC_MODE_EN` at core_mem+0x78 is 0),
so ABL/XBL either drives sdhc_1 in legacy MCI mode or resets it on exit. The
probe stopped as designed before any SDHCI register access or card command.
Boot continued: HFS RAM root, PID 1, 4 CPUs, as before.

Consequence: river cannot reuse ABL's controller state the way sweet reuses
UFS. A read-only driver must initialise the host like Linux's sdhci-msm:
set `HC_MODE_EN` (and `FF_CLK_SW_RST_DIS`), reset the host, answer the MSM
power-control handshake (`CORE_PWRCTL_*` in core_mem), set the SDHCI clock
divider from the GCC rate ABL left, then enumerate the card (CMD0, CMD1, CMD2,
CMD3, CMD7, optionally CMD6 for 4/8-bit bus width) before CMD17 reads. CMD0
resets the card's state machine and CMD6 changes a volatile EXT_CSD field;
neither writes stored data, but both go beyond reading.

### eMMC read-only driver: host init and card enumeration (2026-10-20)

Second 0128 revision (CI run 39373264676, photo IMG_0327): SDHCI mode, reset,
power-control handshake (`pwrctl 0x6` = BUS_ON + IO_LOW, acked), 1.8 V,
GCC sdcc1_apps reprogrammed. ABL had left the RCG at **cfg 0x205 = GPLL4/3 =
384 MHz (HS400)**. Result: `CMD1 … error, status 0x00018000` (command
timeout); the card did not answer.

Third revision (CI run 39374127195, `eee65eb`, image `river-r4-emmc-boot.img`
SHA-256 `5c477eb3…`, photos IMG_0328/IMG_0329): before lowering the clock,
restore the sdhci-msm vendor register (hc_mem+0x10c) to its POR value
0xa9c and clear the power-save DLL bit, as Linux's probe and default-mode
select do. ABL had left `vendor_spec 0x00358b1c`, i.e. MCLK select = HS400
(divided DLL clock), which explains the silent CMD1. **The eMMC now reads:**

    RCG now cfg 0x2017 m 0x1 n 0xfc (400 kHz from XO), CBCR 0x4ff1
    pwrctl 0x6 acked; host 0x00000d00 clock 0x000e0005
    OCR 0xc0ff8080 ready, sector mode
    CID manufacturer 0x13 product "S0J9N8"            (Micron)
    selected RCA 1, status 0x00000900 (state 4, 4 = tran)
    GPT 64 entries of 128 bytes at LBA 2; last LBA 122142719 (59640 MiB)
    backup GPT header OK at LBA 122142719
    p1 sbl1_a LBA 256; p2 sbl1_b LBA 1280; p41 boot_a LBA 798720 +32 MiB;
    p42 boot_b LBA 897024 +32 MiB; p61 vendor_b LBA 3932160 +848 MiB;
    p63 system_b LBA 11436032 +2816 MiB; p64 userdata; 64 partitions
    boot_b starts with ANDROID! (header v1 found)
    read-only probe done

Then the HFS RAM root mounted and PID 1 ran on four CPUs as before. Only
CMD0/1/2/3/7/13/17 were issued; no data was written and nothing flashed.
The card is left in 1-bit 400 kHz transfer state; Android re-initialises
it on the next boot.

### Read-only eMMC block device at 25 MHz (2026-10-20)

0128 now raises sdcc1_apps to **25 MHz** (GPLL0_DIV2/16, RCG cfg 0x41f, still
1-bit default speed, no CMD6) after enumeration, checks CMD13, and falls back
to 400 kHz on failure. It registers a read-only bdevsw/cdevsw:
`/dev/emmc0`/`/dev/remmc0` (whole user area) and `/dev/emmc0sN`/`/dev/remmc0sN`
for each GPT entry (mode 0440; write opens and write I/O return EROFS).
`tools/river-init.S` is a PID 1 test that reads boot_b through both nodes and
tries a write open.

- r5 (IMG_0330): kernel data abort in the raw read: physio() passes the
  caller's user address. Fixed with `copyout()` (r6).
- r6 (IMG_0331/0333): 25 MHz, `1 MiB in 738 ms (1386 KiB/s)`, nodes created,
  `/dev/remmc0s42: first bytes ANDROID!`, write open refused, but
  `/dev/emmc0s42: read failed`: specfs `set_blocksize()` uses `d_psize` as the
  block size; we returned the partition length. Fixed (r7).
- **r7** (CI run 39376791491, `d0f2375`, image `river-r7-emmc-boot.img`
  SHA-256 `6aec1ef3…`, photos IMG_0334/IMG_0335):

      RIVER eMMC: 25 MHz default speed, 1-bit
      RIVER eMMC: 1 MiB in 737 ms (1389 KiB/s)
      RIVER eMMC: read-only /dev/emmc0, /dev/remmc0 and 64 partitions
      /dev/remmc0s42: first bytes ANDROID!
      /dev/emmc0s42: first bytes ANDROID!
      RIVER init: open for writing refused (read-only) OK
      RIVER init: still alive

  Raw and block reads from user space work, writes are refused, on 4 CPUs
  with the HFS RAM root. Nothing was written to the eMMC; nothing flashed.

### First boot with root on eMMC (2026-10-20, photo IMG_0338)

0128 adds `rd=emmc0sN` (read-only root on GPT entry N, `river_emmc_rootdev`);
CI run 39377752332 (`0b8230d`), kernel `../k-r8/`. The user approved wiping
the phone. `fastboot flash userdata` was **refused by the Moto bootloader**
(`flash permission denied`; `partition-type:userdata raw`, size
0xc827fbe00), so nothing was written there. Partition map from Android:
boot_a p41, boot_b p42, vendor_a p60, vendor_b p61, system_a p62, system_b
p63, userdata p64 (LBA 17203200, 51239 MiB).

Instead the 8 MiB HFS+ test root (`../k-r8/root-hfs.img`, SHA-256
`6a3d0e7b…`, `/sbin/launchd` = `tools/river-init.S`) was written with
`fastboot flash system_a` (**inactive slot a**, raw, 0xb0000000 bytes;
`Writing 'system_a' OKAY`; slot b stays current, so Android still boots).
Then `fastboot boot shim-river/out/river-r8-sys62-boot.img` (no RAM disk,
boot-args `… cpus=4 … rd=emmc0s62 rivermmc=1`, SHA-256 `60cad53e…`):

    BSD root: emmc0s62, major 1, minor 62
    hfs: mounted SweetRoot on device b(1, 62)
    SWEET: root volume is not authenticated (error 25); continuing unsealed
    load_init_program: attempting to load /sbin/launchd
    *** RIVER userspace: eMMC test init running as PID 1 ***
    /dev/remmc0s42: first bytes ANDROID!
    /dev/emmc0s42: first bytes ANDROID!
    RIVER init: open for writing refused (read-only) OK
    RIVER init: still alive

**XNU now boots its root filesystem from the phone's eMMC**, read-only, on
four Gold cores, with the kernel loaded from RAM. Slot a's system image is
gone (a LineageOS update or `fastboot flash system_a` restores it).

## Next work

1. Faster I/O if needed: multi-block CMD18 needs CMD12 STOP (also a
   non-writing command), and 4/8-bit or HS modes need CMD6 (a volatile
   EXT_CSD write, so the user's approval first).
2. iOS userland: r9 enables userdata-limited CMD24 writes; r10 adds the
   software splash display and reaches the Apple logo. r11 saves a console
   log and process samples to the unused userdata tail. Investigate the
   LaunchServices wait and PosterBoard first-time wallpaper assertion below.
   Input and USB logging still need river-specific drivers.

## iOS 16 root preparation (2026-10-20)

Chose **root ADB in the installed Lineage recovery + `dd`** for the first
userdata image. `adb root` also works in Android, but `/data` is mounted
there; recovery reports `uid=0`, `/dev/block/by-name/userdata` points to
`/dev/block/mmcblk0p64` (53,729,017,344 bytes), and p64 is unmounted.
Rebooting into recovery did not flash any partition. A binary ADB stream into
recovery RAM was tested before writing the partition. The Moto fastboot
bootloader still refuses `fastboot flash userdata`.

`tools/mkiosroot.sh` now dispatches to `tools/mkiosroot-macos.sh` on macOS.
The macOS builder mounts the three APFS source images natively with owners
enabled, copies their numeric owners, groups, hard links, symlinks and extended
attributes, then makes a **bare**, case-sensitive journaled HFS+ volume. The
output has no partition table or DMG footer (`HX` at offset 1024), and the
size defaults to 16 GiB. It adds the cryptex shared-cache links, logd markers
and system keybag from the Linux builder. It requires administrator privileges
to preserve iOS owners; macOS privacy rules required staging this one build
under `/private/tmp`, then moving its output back to the private handoff folder.

The source is Apple's iPhone X iOS 16.7.16 (20H392) IPSW. The D22AP Erase
identity in `BuildManifest.plist` selects `087-86698-044.dmg` (OS),
`087-86593-047.dmg` (SystemOS) and `087-86715-047.dmg` (AppOS). Only these
three entries were extracted with `tools/remotezip.py`, each passing its ZIP
CRC-32, because this Mac did not have room for the full archive beside a
16 GiB root. The output is `../ios16-private/ios16-root.img` (local, outside
git): 16 GiB, about 11 GiB used. `tools/sweet-root-tweaks.py` was run on it;
`/sbin/launchd` and the keybag are owner 0:0. Its activation tweak reported
no existing mobileactivationd container, so it skipped the data ark.

The Apple `DeviceTree.d22ap.im4p` was also extracted into ignored
`research/ipsw-x/d22ap.adt`. A new `mkadt_river.py` output therefore includes
the real D22AP `/product` properties (61 properties) for iOS userland.
`../ios16-private/river-ios16.adt` and the git-ignored
`shim-river/out/river-ios16-boot.img` use the existing r8 kernel with
`rd=emmc0s64 rivermmc=1`, `ignition_level=1` and the sweet userland boot args.
No new kernel build is needed to read the root from p64, but the current eMMC
driver still mounts it **read-only**. A writable iOS userland will need a
separate, userdata-limited XNU write path in a later step.

`tools/flash-iosroot-userdata.sh IMAGE` checks that exactly one recovery ADB
device is connected (or uses `RIVER_SERIAL`), then verifies the river model,
root shell, p64 link and length, unmounted state,
HFSX signature and image size before its only write. It streams the image to
p64, then SHA-256 compares a readback of exactly the written length.

The recovery write completed: 17,179,869,184 bytes to p64 at about 30 MB/s.
The full readback hash matched the image,
`fa28e3a7f80bfe0ac55a1ce8f46aed2c1b4fab2a72f6131c0c25273ebd096ba9`.
This replaces Android's `/data` filesystem; slot b boot, bootloader, modem,
persist and recovery were not flashed. From recovery, the phone rebooted to
fastboot (`product: river`, `current-slot: b`), then `fastboot boot` accepted
`shim-river/out/river-ios16-boot.img` (SHA-256
`4ef56353e0ffb2b942b1b695af495af018edd070b4679117a16b7834f8f3633c`).
This only loaded the kernel from RAM. **IMG_0341** shows the iOS root did
mount: `launchd` ran as PID 1, loaded dyld shared-cache mappings, printed its
boot args, entered on-demand mode and ran `fsck` and `mount-phase-1`.
`mount_hfs` then reported a read-only filesystem while trying to mount `/`,
and XNU panicked because the `mount` boot task exited 1. There is still no
river display driver beyond the framebuffer console.

Patch 0129 adds polled CMD24 single-block writes behind `rivermmcrw=1`.
The GPT must identify p64 as `userdata` with this phone's exact LBA range;
the only write-capable device node is `/dev/emmc0s64` (and its raw twin).
Every CMD24 is independently checked against that LBA range. The driver
polls CMD13 until each written block is back in transfer state. This is
needed for the iOS `mount` task to make the root writable. Until tested,
boot without `rivermmcrw=1` retains the previous read-only behavior.

The r9 GitHub Actions kernel build (`39383612244`) passed, and
`tools/kbuild-river.sh` packaged `shim-river/out/river-r9-boot.img`
(SHA-256 `0fa15f83fe2c4c5eb543364af56729e6ed26d786c3c30328cfdf21a4e7817303`).
The compiled kernel contains the CMD24 userdata guard. On 2026-10-20,
`fastboot boot` accepted this image from slot-b fastboot; it did not flash a
partition. Boot args include `rivermmc=1 rivermmcrw=1 rd=emmc0s64 rootrw=1`.
After 42 minutes, the screen still showed repeated `exec xpcproxy`,
`fairplayd.H2` and `bluetoothd` lines, plus `STR terminating due to uncaught
exception of type NSException`. It also printed `STR Version 16.7.16 (Build
20H392)` and `STR <KCDisplayMode 0 x 0 fmt: range:undefined>`. The earlier
read-only root remount panic was absent, and iOS service launch advanced well
beyond it. This is a service restart loop, not a completed graphical boot.
The user was advised to stop the run; persistent CMD24 writes could otherwise
continue without useful progress. The next investigation should identify the
first failing service and collect a bounded boot log, rather than leave it
running indefinitely. The photo remains in the private handoff folder only.

## River graphical framebuffer (r10 preparation)

Private photos IMG_0343–0345 narrow the r9 loop: `CRASH SpringBoard[393]
sig 6`, with `NSInternalInconsistencyException` and reason `We expect a valid
bounds for configuration`. The configuration names `Wireless`/`wireless0`,
with a 0x0 mode and `bounds = {{0, 0}, {0, 0}}`. This is a specific
SpringBoard failure after the root mount, not evidence that a longer wait
will finish booting. The user returned the phone to fastboot.

Patch 0130 enables AppleCLCD's existing iOS IOMobileFramebuffer interface on
river with a separate hardware path. It gets the measured splash geometry
from the river board constants (1080x2270, 24bpp, stride 3240 at 0x90001000),
allocates a cached BGRA IOSurface for CPU composition, and strips alpha when
copying a completed frame to the existing splash scanout. It never maps or
programs topaz MDP/DSI, invokes TopazModem, or enables its battery/input
drivers. There is no scanout address change, panel command, clock or power
programming. Colour byte order is still to be verified on river; early
console testing used grayscale only.

The device tree now advertises an iPhone X 1125x2436 canvas, matching the
driver's default logical mode and this userland's UIKit layout data. The
central 1080x2270 window is copied to the Moto panel (offset 20,83), so this
first implementation crops the edges rather than scaling. Touch and buttons
still need river drivers. Use `sweetfb_noconsole=1 sweetfb_stayon=1
sweetfb_hz=5` for the first GUI test: show the GUI after its first swap and
keep it awake while input is absent. A fallback `sweetfb_console=1` leaves
the text console visible while publishing the display service.

Local compilation passed for river and topaz for SweetFramebuffer.cpp,
SweetSurface.cpp and KernelConfigTables.cpp. The C++ checker was corrected to
combine recorded CXXFLAGS and CFLAGS, as XNU's build rule does; its previous
CXXFLAGS-only invocation omitted target and board defines. The river
framebuffer object references `ml_io_map_wcomb` and the buffered-surface
allocator, with no TopazModem or scanout-geometry initializer reference.
Commit `a5a5fd7`, CI run `39394024834`: passed. The downloaded river kernel
contains the new `RIVER FB` messages and the existing userdata-only CMD24
guard. `fastboot boot` accepted `shim-river/out/river-r10-boot.img` on
2026-10-20 (SHA-256
`987eea90a1d321188c6b741a6e180a67970bda9ad81176709a7f3464e1e8a66d`).
This was a RAM boot; no partition was flashed for the display test.
The user observed an Apple logo, which remained after eight minutes. This
establishes graphical output but does not identify which service prevents
startup completing. After the user returned to fastboot,
`fastboot boot` accepted `river-r10-console-boot.img` (SHA-256
`041cd31244fbf164323e7b009d8e09b08d945e043f44f1b8b101de772959e51d`).
The user reports progressing `exec` messages without an identified error.
`river-r10-console-boot.img` is a local fallback
with the same kernel and published display service but swaps suppressed.

## Persistent river console (r11 preparation)

Patch 0131 reuses the existing 4 MiB SWEET console ring and decoder with an
eMMC backend, separately enabled by `riverlog=1` (also requires
`rivermmcrw=1`). The backend checks the p64 HFS+/HFSX volume header before
enabling any log writes. Its block size must be a valid power of two and
the volume's declared byte length must end before the log region. The log
occupies the final 4 MiB + 4 KiB of userdata, aligned down to 4 KiB. This is
far beyond the current 16 GiB root inside the 53,729,017,344-byte partition.
There are independent log-region and low-level userdata LBA bounds checks.
The same eMMC mutex serializes filesystem and log writes. A header is
written after each flush, every ten seconds; only one logger is started.
This also starts the existing software-only SpringBoard/backboardd thread
sampler (first at 90 seconds, then every 120 seconds) to diagnose a logo
without a crash. It does not enable the topaz hardware probes.

`tools/river-log-pull.sh PRIVATE.txt` reads the tail from root ADB in the
installed recovery, checking recovery state, river model, exact userdata
device and length. It saves the binary ring and decoded text with private
permissions. It writes nothing to the phone. Logs must stay outside git.
No log can be retrieved from earlier r9/r10 boots, which did not enable this
backend.

Local builds passed for river's eMMC and shared logger, plus the topaz
logger. A host harness compiled the actual eMMC log guard functions and
verified opt-in/write gating, HFS overlap/signature/block-size rejection,
valid first/last blocks and rejection of zero, oversized or out-of-range
writes. Decoder tests covered unwrapped, wrapped, multi-wrap and truncated
header input. Commit `064a5e7` passed CI `39399757005`. The resulting
`river-r11-boot.img` (SHA-256
`37f8582e51ed7a89ff412bde8659c77f1c7a8e0bfc4b60e153bd1dbc4f291bf1`)
was accepted by `fastboot boot`, with `riverlog=1`, on 2026-10-20.
**Persistence verified:** root ADB in the installed recovery retrieved a
valid version-1 ring with 540,839 bytes of console text, flush sequence
2037, including early boot and all 40 process samples. The binary and text
are in `../logs-private/r11.txt[.bin]`, outside git. The phone stayed on the
Apple logo. `fastboot reboot recovery` is not supported by Moto ABL
(`Unknown command`); the user selected Recovery Mode from its menu. ADB
`reboot bootloader` works from recovery.

The log confirms AppleCLCD is published as 1125x2436, backboardd opens it,
creates BGRA surfaces and submits frames; no SpringBoard/backboardd crash
was recorded. Backboardd's main thread settles in the CFRunLoop. Samples
5–38 show SpringBoard waiting in LaunchServices context initialization
while ControlCenterServices filters modules by installed app availability.
`lsd` is flagged for >50% CPU over 180 seconds and >32 threads. Later,
SpringBoard advances and submits more frames. PosterBoard then traps in
`-[PBFPosterExtensionDataStore _stateLock_setupSwitcherConfigurationForFirstTime]`;
local disassembly identifies its assertion `!_stateLock_switcherConfiguration`.
This identifies investigated dependencies, not a proven fix for the logo.

A same-kernel focused diagnostic image,
`river-r11-services-boot.img` (SHA-256
`b52cacf29d7651a6a648881bfe0da557dfa14eae5d53601dc88c8ff9ec07e9b7`),
was RAM-booted from recovery via `adb reboot bootloader`, with
`sweetsample=SpringBoard,backboardd,lsd,PosterBoard`, and matching `sweetscan`
and `sweetnull` lists to capture the missing exception text and database
worker stacks. Focused log retrieval pending.
