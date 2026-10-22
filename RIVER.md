# Moto G7 river bring-up (local branch)

Started 19 October 2026. This is a chronological evidence log, not the
current action plan. Read `../HANDOFF.md` for the current r49 status. River
now boots iOS 16.7.16 to an interactive home screen; the Adreno 506 passed
Gallium and offscreen GLES tests, but CoreAnimation still uses its CPU
renderer. Older sections below retain observations and proposed next steps
from their dates; those proposals are historical. Device dumps, photos,
logs, Apple artifacts and boot images stay in adjacent private folders or
ignored output directories.

## Early direct device observations (historical)

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
  with a base DTB, plus separate DTBO partition. The early diagnostic boots
  used RAM images only; later work wrote an iOS root to storage as recorded
  in the dated sections below. Current test boots remain RAM-only.

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
worker stacks. Recovery readback retrieved 188,635 bytes, flush #141,
with samples 0–11. LaunchServices reaches its normal run loop; PosterBoard
is alive without a crash in this capture. SpringBoard samples 7–11 instead
wait in MobileGestalt during `-[ISDefaults init]` in IconServices.

## Identity wait test (same r11 kernel)

Local shared-cache symbolication/disassembly identifies the blocked public
query as `UniqueDeviceID`. Its nested WiFiAddress query first reads
`IODeviceTree:/chosen` property `mac-address-wifi0`; after missing that and
the sdio/wlan/marconi-wifi local-mac-address paths, it waits for an
IONetworkController notification. River has no network controller driver.
`tools/mkadt_river.py` now supplies a stable synthetic, locally administered
six-byte `mac-address-wifi0` in /chosen. This provides identity metadata,
not a Wi-Fi service or radio driver. The existing kernel is unchanged.
The generated ADT was serialized and parsed back with its property checked.

`river-r11-identity-boot.img`, SHA-256
`f910397d971e7eec82cfb55f4f12c39d80c4369180bfbb56cc8154c7412f51f0`,
was accepted by `fastboot boot` from recovery on 2026-10-20. Same focused
sample/scan/stdout lists and userdata logging as the previous test. Screen
result verified by photo IMG_0351: **iOS lock screen**, with date, large
clock and camera button. The Wi-Fi identity metadata change clears the
previous Apple-logo stall. This does not verify touch, unlocking, apps,
wallpaper correctness or clock accuracy (the photo shows Wednesday,
December 31). Keep this boot available as a known graphical baseline.
Retrieve its log as `../logs-private/r11-identity.txt` on the next recovery
visit. Next: river-specific input hardware discovery and a touch/button
driver; do not enable the topaz MMIO drivers.
No protected partitions were flashed. The symbol source DMG is detached
and restored to its original private path.

## River input implementation (r12 preparation)

Recovery inventory identifies `NVTCapacitiveTouchScreen` / `NVT-ts` on
QUP I2C3 (0x078b7000), address 0x62, firmware 11, 1080x2270. GPIO10/11
are BLSP I2C3 function 2; interrupt/reset are GPIO65/64. Volume-up is
active-low TLMM GPIO85; power and volume-down use PM8953 PON SID0 0x810.
SPMI core/observer are 0x0200f000 / 0x02c00000 (8 MiB observer region).
The successful identity boot log was retrieved privately (359,962 bytes,
flush #224); no new root image was written.

Patch 0132 enables software HID personalities and adds a separate river
branch in the already-built sweet_keys.c, opt-in `riverinput=1`. It votes
BLSP1 AHB, sets QUP3's clock to XO / 1, enables its branch, and selects
I2C3 on GPIO10/11. A bounded polled QUPv2 FIFO transfer only addresses firmware endpoint
0x01; 0x62 is the DT hardware-command address and is deliberately unused.
volatile event-page selection and firmware info reads must validate the
firmware complement and exact panel bounds before events are accepted.
No touch firmware flashing, bootloader commands, GPIO reset or PMIC config
writes are present. Buttons read the older SPMI v2/v3 observer channels;
volume-up is accepted only if its pin is already a GPIO input. Touch HID
coordinates use river's 2270-pixel panel height and the displayed canvas crop.
Touch reports and buttons are polled at 20 ms. Hardware behavior is unverified.

References: matching [LineageOS river kernel](https://github.com/LineageOS/android_kernel_motorola_sdm632/tree/lineage-23.0)
`drivers/i2c/busses/i2c-msm-v2.c`, `drivers/spmi/spmi-pmic-arb.c`,
`drivers/input/touchscreen/nova_mmi/nt36xxx.c`, and upstream Qualcomm
`gcc-msm8953.c` / `pinctrl-msm8953.c`. Local reference downloads and device
inventory stay outside git. Local compilation encountered a blocked header
read; CI compilation/link and a phone test remain required.

Corrected commit `130c72e` passed river CI **39468990896**. The full kernel
compiled and linked successfully. The actual frame-decoder host harness
also passed corner coordinates, finger ID 10, release, and invalid ID/X/Y
checks under address/undefined sanitizers. Patch reverse-application check
passed. No topaz object change is intended outside preprocessor guards.
`river-r12-boot.img` SHA-256:
`0149a52229379a0b8c93055740261dd889133a6f2f71fd02069ff40f4aa8ee05`.
Its payload embedding, `riverinput=1 riverlog=1` arguments and river/input
kernel strings were checked before `fastboot boot` returned OKAY.
Phone interaction result pending (swipe up and short volume press requested).

Before r12 was sent, while CI was still running, the user reported a
bootloader-screen loop. Neither ADB nor fastboot initially enumerated;
the user restored stable fastboot with the buttons. r12 was then booted
from that verified fastboot state. No image had been sent during the loop;
its cause is not established. The r11-identity GUI image remains the fallback.
Recovery debugfs was mounted as a virtual filesystem for read-only pin/clock
inventory; no partition was flashed. The next log name is
`../logs-private/r12-input.txt` when the user returns to recovery.


### r12 test failure (reported by user)

The user reports a frozen kernel-panic screen ending with "handshake
structure not initialized". This is a panic-output diagnostic, not the
original panic reason: Apple's matching xnu-8796.141.3
`osfmk/kdp/output_stages/out_shmem.c:499`, in `panic_spin_shmcon`, prints it
when the hardware shared-memory dump handshake is absent. The actual panic
line/backtrace is not yet available. Requested a full-screen photo before
reset and recovery entry to read `../logs-private/r12-input.txt`.
No code fix or cause is established. Do not treat the handshake message as
a touch-controller handshake failure. Phone has no ADB/fastboot enumeration
while frozen; waiting for recovery. r11-identity remains the GUI fallback.

Recovery pull saved r12-input.txt, but its header (359,962 bytes, flush #224)
and entire decoded text are byte-identical to r11-identity.txt. Thus no r12
log was persisted; an early failure before its first flush is suspected,
not proved. Same-kernel console image prepared and RAM-booted to expose the
initial panic without framebuffer swaps:
`river-r12-console-boot.img`, SHA-256
`0c006d3c04f33b9f7df6542ca4ed666dfd85c4422dbb26e2d563e36a966bf911`.
It keeps riverinput=1 and riverlog=1; screen/panic photo pending.


### Panic cause identified from IMG_0352; patch 0133

The original panic is `kmem_alloc(..., 1048576, 0x100021): failed with 3`
at vm_kern.c:178. The displayed caller resolves to `__kmem_failed_panic`.
Flags decode as KMA_NOFAIL | KMA_PAGEABLE | KMA_PERMANENT, exactly the
allocation performed by `osfmk/arm/io_map.c` for ordinary `ml_io_map`.
That file limits its permanent `io_submap` to **8 MiB**. The framebuffer's
3240×2270 splash mapping rounds to 7,356,416 bytes, leaving only 1,032,192
bytes even before other mappings. r12's first input probe requested a
1,048,576-byte TLMM window, which cannot fit. Its later whole 8 MiB SPMI
observer window would also have been invalid for this pool. This is virtual
I/O mapping exhaustion, not exhaustion of the phone's physical RAM.
It happens before the first RIVER INPUT probe print and before eMMC logging.

Patch 0133 replaces broad register windows with nine 16 KiB pages (144 KiB
maximum): GPIO10/11, GPIO64/65, GPIO85, SPMI core and just the selected PON
observer channel, three GCC pages, and QUP3. It uses
`ml_io_map_unmappable(..., VM_WIMG_IO)`, which maps through kernel_map rather
than the small permanent I/O pool. Physical register addresses and device
memory attributes are preserved; no change to register writes or touch
protocol. Each page is announced before mapping for diagnostic visibility.
Local object checks passed for river and topaz. A bounds/address check
verified all fixed register accesses and all 256 possible PON channel
page offsets. Reverse patch application passed. Phone is in fastboot;
replacement r13 CI and hardware test pending. Photo remains private.


Commit `1aea7be` passed CI **39473085016** (full kernel build and link).
`river-r13-boot.img`, SHA-256
`ec61397d2392a54512b383044795c92144654cb97a60cdc7d9ed58c6e405f466`,
was verified to embed the matching payload, riverinput=1/riverlog=1 args,
and sparse mapping code; fastboot accepted the RAM boot. User was asked
to report panic/lock-screen result and test swipe-up plus short volume.
Hardware verification pending. Next private log: `../logs-private/r13-input.txt`.

## r13 input evidence and river USB preparation

Recovery readback retrieved a fresh r13 ring (221,832 bytes, flush #142).
The mapping fix is verified on hardware: all sparse pages map, PM8953
SPMI v2 reports 0x20010000, PON observer offset 0x140000 reads successfully,
and Novatek firmware 11/complement 244 validates at event page 0x21c00,
1080x2270. Backboardd opens both HID services. Touch down/up and power /
volume-down events reach the HID user client. The user still sees the
Apple logo, so interactive unlocking is not verified. SpringBoard sample
11 now waits synchronously in MediaRemote's audio routing discovery
(`MRAVConcreteRoutingDiscoverySession availableOutputDevices`), a later
startup dependency. No root-cause fix for that stall yet.

Patch 0134 prepares USB logging using the existing polled DWC3 CDC-ECM
implementation. River's live DT gives DWC3 0x07000000, QSCRATCH
0x070f8800, QUSB2 revision 1 at 0x00079000, and USB GDSC 0x0183f078.
Clock offsets are checked against matching downstream
`msm-clocks-hwio-8953.h`: master/sleep/UTMI 0x3f000/4/8, PCNOC AXI
0x3f038, PHY config AHB 0x3f080, reference enable 0x41030 (GCC base
0x01800000). Matching dwc3-msm.c confirms HS VBUS bits 20/28; matching
phy-msm-qusb.c confirms PLL lock bit 5 at 0x38 and powerdown at 0xb4.
All register mappings use the general kernel map with I/O attributes.

`riverusb=0` (default) is off; `=1` reads state only; `=2` attempts the
existing high-speed gadget only after GDSC, all five clocks, reference
clock, PHY PLL/power, DWC3 ID and device-mode checks pass. It does not power
up gated blocks, reprogram PHY tuning, or access the SS PHY. The shared
USB thread starts after root selection (network stack initialized). It
uses synthetic identity and the existing 192.168.7.2 -> 192.168.7.1 UDP/5140
log stream. Live logging is not yet tested on river.

`tools/river-usbnet-macos.py PRIVATE.txt [--configure]` waits for the exact
synthetic USB MAC, optionally assigns 192.168.7.1/24 to that interface,
then receives only on the USB address and accepts only the phone's IP.
It refuses log paths inside git and existing output files, and creates
logs mode 0600. Configuring the host interface may require sudo. Logs stay
on the Mac; no default route is added.

Local river/topaz objects compile. A harness exercised the actual USB
probe's opt-in, gated-power/clock/reference/PLL/powerdown/ID/device-mode
rejections and verified read-only mode never starts the gadget. Host
interface parser check selects the USB MAC without selecting the normal
network interface. Patch reverse application passed. CI and USB hardware
testing pending; phone is in recovery after the r13 log pull.

### r14 ready for handoff; NOT phone-tested

Commit `890463d` passed CI **39479040517**. The user requested finishing
host preparation and handing off before the next phone boot. r14 has **not**
been sent to the phone. Kernel/payload/ADT embedding, requested boot args
and river USB/sparse-map strings were verified locally. Three private
same-kernel images are ready under shim-river/out/:

| Image | Mode | SHA-256 |
| --- | --- | --- |
| river-r14-boot.img | riverusb=2, graphical; guarded gadget attempt | 7a53db17af4813db2c21cee7db3242ef9101337af5bf4ffb4adbc6759abb2d59 |
| river-r14-probe-boot.img | riverusb=1, graphical; read-only USB state | 2fc534b97f6edf12f176accad8c876f0f1f48be81db8c5d34c8d36259ec66d66 |
| river-r14-console-boot.img | riverusb=2, sweetfb_console=1 | 163b81df5ed4949bb997260e3d3a0b6163089e5ed195040281fe9678b868df32 |

All include riverinput=1, riverlog=1 and focused samples for SpringBoard,
backboardd, mediaremoted and mediaserverd. The waiting host receiver was
stopped (it had not seen a USB interface or written r14-live.txt). No host
USB IP address was configured. Phone verified in root ADB recovery, model
river, recovery binary present. Its ro.bootmode reports normal even here;
use the ADB transport's `recovery` state, as river-log-pull.sh does.
Next chat should verify the image, restart the receiver, RAM-boot the main
r14 image, and inspect enumeration/probe output. Only configure the matching
synthetic USB interface; macOS may require administrator authorization.
If no interface appears, return to recovery and inspect r14-usb.txt before
changing power/clock/PHY setup. Keep all raw logs local and outside git.

## r14 live USB, Gold clock fix, storage bottleneck (2026-10-20, Claude session)

### r14 live USB log: works
`river-r14-boot.img` RAM-booted 15:21:45. The DWC3 CDC-ECM gadget enumerated
at +50 s (`0x1209/0001`, 480 Mbps). macOS first blocked it ("device
functionality blocked by transport restrictions": Apple Silicon accessory
security; the user clicked Allow). The host receiver then got the live
console over UDP. The `SWEET USB: event` lines are a bounded 200-line burst
(capped in code), not a feedback loop. The live log also caught the user's
touch and Power (0x30) / Volume-down (0xea) presses reaching iOS HID.

### Boot-time diagnosis: Gold ran at 19.2 MHz
The iOS clock starts at 0 on boot, so launchd timestamps are uptime; host
stamps (`tools/river-log-stamp.py`) agree. r14: root at ~+49 s, launchd ~+92 s,
SpringBoard exec +254 s, still in dyld init at +378 s. Kernel/CPU evidence:
`pmap_startup` 345 ms; FIPS RSA 747 ms, ECDSA 1415 ms, PBKDF 2490 ms.
ABL runs only on Silver and never programs the Gold (perf) cluster clock;
Linux does it in `clock-cpu-sdm632.c` `cpu_clock_init()` (early_initcall,
before any regulator/CPR work): APCS_ALIAS1 CMD_RCGR `0x0b011050`, CFG
`0x401` (GPLL0 sel 4, div 1 = 800 MHz), update, core CBCR enable. The shim
now does exactly that before CPU_ON (`--no-gold-clk` restores old behaviour)
and prints the RCG before/after plus a spin-loop MHz.

### r15clk (r14 kernel + new shim): eMMC panic, photo IMG_0355
`IOFindBSDRoot: river eMMC root emmc0s64 unavailable` after
`CMD7 arg 0x10000 timeout, status 0x00000002`. CMD3 -> CMD7 at 400 kHz were
issued back to back; the slow CPU had always provided the >= 8 card clock
gap. Patch 0135 waits 50 us (400 kHz) / 1 us (25 MHz) before each command and
retries card init once with a 1 ms gap.

### r15 (CI 39485698710, commit 0635c91): verified 43x CPU
`river-r15-boot.img` sha256 a6012c54...48e2, booted 15:49:08. eMMC init
first try. FIPS: RSA 17 ms, ECDSA 33 ms, PBKDF 58 ms (43x; 800/19.2 = 41.7).
USB at +35 s, launchd +61 s, SpringBoard exec +141 s, backboardd +157 s.
eMMC unchanged at 1216 KiB/s (bus-bound). SpringBoard then sat at the same
pc/lr/sp (dyld notifyDtrace, forEachSection+0x34) across samples 1-2 (as in
r14) and moved on between +364 and +604 s; at +724 s still in dyld ObjC
category loading. Other processes progressed. Interpretation (not yet
proven): page-ins at 1.2 MB/s dominate. The shared region is fine: "not in
trust cache" is a soft warning that falls back to the root-owner check, and
all processes share one slide.

### r16 (patches 0136, 0137; commit 4979786): built, testing
0136: EXT_CSD read, CMD6 BUS_WIDTH=2 + host 8-bit, CMD6 HS_TIMING=1 + host
high speed + 50 MHz, each verified by EXT_CSD re-read with fallback;
CMD23+CMD18 multi-block reads up to 64 KiB, verified against single-block
reads. CMD6 gated to those two volatile bytes. 0137: sampler prints thread
state, block hint, wait event, user FAR/ESR and kernel backtrace.

### r16 result: lock screen in ~5 minutes (CI 39488596584)
`river-r16-boot.img` RAM-booted 16:08:32. eMMC: EXT_CSD rev 8, card_type
0x57; **8-bit bus, 50 MHz, multi-block reads on**; 1 MiB single-block 403 ms
(2535 KiB/s), multi-block 88 ms (11569 KiB/s, ~9.5x r15). Timeline (host
seconds after fastboot boot): launchd +38, early boot tasks done +47
(r15: +128), SpringBoard exec +49 (r15 +141, r14 +254), HID touch/buttons
opened by backboardd +103/+105, **lock screen reported by user ~+300 s**.
Sample 1 (+244): backboardd, mediaserverd, mediaremoted idle in run loops
(the r13 MediaRemote wait was not a hang); SpringBoard loading Control
Center module metadata. New sampler fields worked (e.g. mediaremoted at
+124 s: instruction-fetch page fault, ESR 0x82000007, waiting on a kernel
mutex). PosterBoard crashed at +292 s (ns 2 code 0x5), not yet analysed.
Remaining throughput limit is PIO (CPU copies every word); ADMA is the next
storage step.

### r17 (0138, 0139; CI build of 7a87126): **iOS home screen, interactive**
`river-r17-boot.img` sha256 6759b48d...51bd7, RAM-booted 16:24:12.
0138 reads the Novatek report every 10 ms (its INT is a per-frame pulse,
Linux IRQ_TYPE_EDGE_RISING, so the old level-gated poll missed nearly all
move frames); 0139 ends a lift at the last position (was the top-left
corner) and logs the move count. Swipes now carry 14-78 moves.
Swipes first did nothing: the lock screen was drawn at +338 s and iOS
requested display off at +344.7 s (normal lock-screen auto-sleep);
`sweetfb_stayon` keeps the last frame visible, so the panel looked awake
while iOS ignored touches. Tap-to-wake needs an Apple touch-controller wake
gesture we do not provide. **Short Power press at +610.7 s -> `display on`;
swipe up at +613.9 s -> unlocked to the home screen (user report).**
Two SpringBoard crashes in this run (+166 s, +295 s, before any touch):
SIGBUS with pc == far in SpringBoard's own malloc heap ("Data/Stack
execution not permitted"), in FBSApplicationLibrary load and MediaRemote
routing discovery. Not in r14-r16. Suspected eMMC multi-block data errors
(or an SMP race); r18 (0140, rivermmcverify=1) tests that.
Usage notes: wake with a short Power press, then swipe within ~5 s; set
Auto-Lock to Never once unlocked.

### r18 (0140, rivermmcverify=1): no eMMC read errors
`river-r18-boot.img` sha256 078fb462...ff84e, RAM-booted 16:40:41. Every
multi-block read re-read block by block: **0 bad blocks in 14,336 multi
reads** by +410 s (SpringBoard exec +100 s with verify on). No SpringBoard
crash in this run (only the usual driverkitd). The r17 heap-execution
crashes are therefore not explained by eMMC multi-block data errors; next
suspects are an SMP race (test: same kernel with cpus=1, payload rebuild
only) or a rare userland issue. r16 (0 SpringBoard crashes) and r17 (2)
used identical eMMC code.

### r19 / r20: display power, touch-to-wake, RTC clock, eMMC recovery
- **r19** (0141, CI build of cf2e0d9), booted 21:43: without `sweetfb_stayon`
  the screen fades to black when iOS turns the display off; a touch while
  off sends one Power press and is swallowed (`touch 0 wakes the display`
  -> `display on` 0.4 s later). Backlight stays lit (no PMI632 WLED driver).
  Clock still 1969: the RTC was set during IOKit start, then
  `IOKitInitializeTime()` waited for an IORTC service that never appears
  and `clock_initialize_calendar()` reset it to the platform's zero.
- **r19 cpus=1** (same kernel, local payload): SpringBoard died with SIGBUS
  code 10 (KERN_MEMORY_ERROR, failed page-in) after `CMD18 ... data timeout,
  status 0`; the card was left mid-transfer, so CMD17 and two CMD24 then got
  command timeouts (0x18000). Not an SMP race. Likely cause of the bogus
  timeout: the poller preempted between status read and deadline check.
- **r20** (0142, 721b835): wait loop re-reads status after the deadline;
  CMD12 + CMD13 recovery after any data error; RTC applied after
  `clock_initialize_calendar()` and the 30 s IORTC wait skipped on river.
  Root mount / USB at +25 s (was +35 s). PM8953 RTC: enabled, raw 10629378,
  counts from ~2026-06-20 (Android kept the offset on /data). Boot-arg
  `riverrtcoff=<private offset>` (stored in device-private, refined to +-2 s
  from launchd log time vs host receive time; iOS time zone is US Pacific).
  **r20-rtc: correct date/time on the lock screen (user), SpringBoard exec
  +20.6 s.** Two r20 boots: no eMMC errors, no SpringBoard crash.
- Still unexplained: the two r17 SpringBoard heap-execution crashes (no eMMC
  errors in that run).

### r21 performance survey (0143, riverperf=1; CI build of 0099dde)
- spin 798 MHz on Gold cpu 0 (shim GPLL0 switch confirmed).
- DRAM: copy 2764 MB/s, write 5718 MB/s, read 3731 MB/s; 64 KiB cached copy
  4762 MB/s. Memory is not starved; RPM/DDR votes are not the bottleneck.
- RCG: Gold 0x401 and **Silver 0x401** (both GPLL0 800 MHz), CCI 0x403
  (GPLL0 /2 = 400 MHz, CCI corner c2). All three APCS PLLs off (MODE 0).
- Shared APC rail PM8953 S5: FTS2.5 (subtype 9), range 0, VSET 0x9e = 870 mV.
- TSENS 32-37 C.
- DT Gold (c1) corners: 633/902/1094/1401.6/1555/1804/2016 MHz -> c1..c7,
  CPR ceilings 720/790/865/865/920/990/1065 mV. **870 mV covers the c4
  ceiling: Gold 1401.6 MHz needs no voltage change** (APCS C1 PLL L=73 from
  XO, mux src 5). Above that needs CPR/S5 voltage control (+ APM threshold
  875 mV and mem-acc levels).

### Gold at 1401.6 MHz (shim eba7a6f, r21 kernel): works
`river-r21-1401-boot.img` sha256 c6a7f508...32fe8 (fallback same payload at
800 MHz: `river-r21-800-boot.img`). APCS C1 PLL mode 0x80000007 (locked,
output on), L 73, Gold RCG 0x501; S5 unchanged at 870 mV. spin **1397 MHz**
(was 798). 64 KiB cached copy 8190 MB/s (was 4762), DRAM read 5408 MB/s
(was 3731), copy/write unchanged (~2.7 / ~5.5 GB/s). FIPS KATs pass: RSA
10 ms (17), ECDSA 19 ms (33), PBKDF 33 ms (58) = 1.75x. Hottest TSENS
40.9 C at boot (was 36.9). No eMMC errors; driverkitd only crash so far.
Next for CPU: c5+ (1555-2016 MHz) needs S5/CPR voltage control (APM 875 mV
threshold, mem-acc levels); CCI still GPLL0/2 400 MHz.

### r22 (0144 + Gold 1401.6 MHz shim, sweetfb_hz=30; CI build of 029c1e9)
spin 1401 MHz. riverFlush (8-pixel 64-bit packing, unchanged-row skip):
**avg 11.3 ms per flush, ~1166 of 2270 rows written** over the first 256
flushes (about a third of one core at 30 Hz, conversion only; the BGRA
compose memcpy before it is not timed). Timeline: SpringBoard exec +20.7 s,
**lock screen (first display on/swap) +123 s** (r16 at 800 MHz ~+300 s).
User: "definitely a bit smoother". Hottest TSENS 40.5 C. No eMMC errors;
driverkitd only crash.

### r23 (0145 eMMC ADMA2; CI build of 56a624f): lock screen at +70 s
SpringBoard exec +13.5 s (r22 +20.7 s), **lock screen +70 s** (r22 +123 s).
No ADMA2 failure messages, no eMMC errors, driverkitd only crash. The probe
lines fell outside the 64 KiB USB replay (boot is faster now), so 0146
repeats the eMMC and MDP summaries in the input probe.

### r24 (0146 MDP flip scanout, riverscan=1; CI build of 0e894bc): works
ABL state (read-only dump): splash on **RGB0** (src 0x90001000, size/out
0x8de0438 = 1080x2270, ystride 3240, SRC_FORMAT 0x2243f, UNPACK 0x20001 =
B,G,R, OP 0x80000000); CTL0 LAYER0 0x200 (RGB0 on LM0), TOP 0x1f20;
VIG0/RGB1/DMA0 unused. Scanout via RGB0, CTL_FLUSH 0x20008, two contiguous
BGRA flip buffers (0xa3be8000, 0xa4554000), BGRX 0x236ff/0x03020001,
stride 4352. **User: home screen shown correctly.** Display off -> splash
fade and touch wake -> flip buffers both work (+140.9 / +142.3 / +142.8 s).
The 24bpp conversion no longer runs per frame. eMMC (ADMA2) bench in the
live log: 1 MiB multi-block **39495 KiB/s** (PIO was 11569).

### r25 (0147 DCS backlight, riverbl=1; CI build of 50c7902): works
DSI0 CTRL 0x1f3 (video mode, 4 lanes), TRIG 0x4, CMD DMA ctrl 0x14000000.
`panel level 0: done` at display off (+86.5 s); user: backlight turns off
and back on with the display (lock screen, wake). The off/on blink between
the two Apple logos during boot is iOS's own display power sequence, now
visible because the backlight follows it.

### r26 (0148 GIC SPIs, eMMC hc_irq, touch latch; CI build of a7ab291)
`hc_irq on`; **every ADMA2 read slept on the interrupt: 24576 waits =
24576 interrupts by +63 s**, none timed out. GPIO65 INTR_CFG 0xf6 (raw
status, rising edge; ABL's target bits kept). Lock screen +76 s (same as
r23-r25). User: touch and swipes unchanged. But "12288 touch reads in
12288 polls": the INT-low trigger is always true (INT idles low on this
panel), so the latch saved nothing. 0149 drops that trigger (latch,
finger down, or every 10th poll) and counts reads per reason.

### r27 (0149; CI build of d38f17a): touch latch confirmed
"2048 touch reads in 15814 polls (latch 517, finger 4, idle 1527; INT low
in 15814 polls)": Novatek I2C reads down to 13 % of polls (r26: 100 %),
the GPIO65 rising-edge latch catches the frame pulses, and INT is low on
every poll (idles low). User: touch and swipes unchanged.

### State at end of 2026-10-20 Claude session
Default image: `river-r27-boot.img` (shim Gold 1401.6 MHz; kernel 0135-0149;
args riverperf=1 riverscan=1 riverbl=1 riverrtcoff=<private> sweetfb_hz=30).
Lock screen ~70-76 s after fastboot boot; correct date/time; tap/Power wake;
backlight off with the display; MDP flip scanout; eMMC 8-bit 50 MHz ADMA2
~39 MB/s with hc_irq; touch via GPIO65 edge latch. Open: r17 SpringBoard
heap-execution crash (not seen since), PosterBoard/driverkitd crash each
boot, CPU above 1.4 GHz (needs S5/CPR control), GPU (Adreno 506), USB
still polled, Wi-Fi/audio/battery/sensors absent, zero-copy directPresent.

### r28 / r28-zc (0150; CI build of 5da592f)
Present (display thread wall time, includes preemption by CA rendering):
avg 17.5 / 13.6 ms with row-skip, 1266 / 733 of 2270 rows copied;
riverzc=1: 15.6 / 17.7 ms and **0 direct** presents: the scanout pool
never supplied a contiguous surface (cause not established: contiguous
11 MiB allocation failure or another create path). User: no tearing,
looks normal, **still slow**. Conclusion: the remaining kernel copy is not
what limits smoothness; CoreAnimation's CPU software renderer is. Zero-copy
deprioritised (riverzc stays an off-by-default experiment).
**Crash pattern:** appleaccountd SIGBUS at +102 s in r28 with the r17
SpringBoard signature (pc == far in its malloc heap, "Data/Stack execution
not permitted", tag 35). Three such crashes in two processes, from r17 on
(before ADMA2/interrupts): likely user-memory corruption from a kernel
path (suspects: VM compressor/swap, page zeroing/COW, cache maintenance).
Also r28: PosterBoard SIGTRAP/SIGABRT, Preferences SIGTRAP at +538 s.

## r29 crash isolation (2026-10-21)

The r17 SpringBoard and r28 appleaccountd logs each show `pc == far` inside
their own writable malloc mapping (two tag 35, one tag 11). This is an
instruction fetch from heap, not the separate r19 single-core `code 10`
failed-page-in crash after an eMMC timeout. The latter boot did not provide
a clean SMP comparison.

Source survey before changing the kernel: ordinary user mappings from
`pmap_cache_attributes()` use MAIR writeback (0xff) and outer-shareable
PTEs (`SH_OUTER_MEMORY=2`, defined as inner+outer shareable). The ARM64 TLB
paths use inner-shareable broadcast invalidations (`...is`). No wrong PTE
attribute or local-only TLBI was found in that path. River's ADMA2 reads
use a dedicated contiguous bounce buffer and copy into the requested kernel
buffer; ADMA2 and IRQs postdate the first two crashes. This does not yet
exclude cross-core cache behavior, swap writes, or another memory writer.

Local r28-kernel payload variants prepared (not new kernel builds):
- `river-r28-c1-boot.img` SHA-256 `23e1f5897cecd1b166785b1c2b895e13ca5a71b56d83156bcb2df8a2a25f4d91`:
  `cpus=1`, otherwise r28 args. RAM boot accepted; live test in progress.
- `river-r28-noswap-boot.img` SHA-256 `ffb1318267a106aacf47e127f367a8cba9ed8a6a92dfeae89e83379394324cf0`:
  `cpus=4 vm_compressor=2`, otherwise r28 args. Not booted yet. XNU
  `vm_fault.c` accepts 2 as `VM_PAGER_COMPRESSOR_NO_SWAP` and prints the
  selected mode at boot.

Patch 0151 extends the existing SWEET crash line only for SIGBUS with
`pc == far`: ESR, instruction at LR-4, x0-x28, and 64 bytes at x0/x8/x16
when readable. It compiled locally for river and topaz. A phone test remains;
crash memory stays only in private local logs.

0151 CI `39545871141` passed at source `e2a9997`. The packaged diagnostic
image with the normal four-core root args is `river-r29-diag-4c-boot.img`
(SHA-256 `71a10fafb9bef5c8c158d793347b53b3f67545e6d81595e482efe171d6e008da`);
the same kernel plus `vm_compressor=2` is `river-r29-diag-noswap-boot.img`
(SHA-256 `8230247d849336a551bfd4b9f8b13a4d3fc4028f0b92d8d44cf362c48c3aa34e`).
The first single-core r28 boot used the still-running receiver's
`logs-private/r19-live-15.txt` (despite its old prefix): `1/1 CPUs
initialized`; lock screen, unlock, Settings, Phone and Safari launches,
active touch for about eight minutes per the user. No heap-execution crash
or eMMC error; Preferences had a separate SIGABRT and driverkitd repeated
its known exit. The user returned to fastboot. This is one clean heap-crash
observation, not evidence of a fix. The four-core `r29-diag-noswap` image
then returned `fastboot boot` OKAY; its phone result is pending.
Its live log is `logs-private/r19-live-16.txt`: `"vm_compressor_mode" is 2`
and `4/4 CPUs initialized`. The generic `VM Swap Subsystem is ON` line
still appears, but `vm_config_init()` leaves `swap_is_active` false in mode
2, and `vm_compaction_swapper_do_init()` skips opening the swap file.
The user unlocked and used apps for about twelve minutes with no freeze or
SpringBoard restart; 193 touch log lines, no heap-execution crash or eMMC
error in the final local log. This is one no-swap boot, so no causal claim.

Patch 0152 (prepared, not yet phone-tested) adds `rivermmcwriteverify=1`:
after each successful userdata CMD24 reaches transfer state, issue CMD17
for the same LBA and compare all 512 bytes, returning EIO on mismatch. It
also counts verified and bad blocks. The switch defaults off and remains
under the existing userdata-only write guard and eMMC mutex. A local river
object compile passed. This is a separate storage experiment after the
CPU-count and no-swap boots.
0152 CI `39546394860` passed at source `7db6aa2`. The first artifact
download timed out; retry succeeded. Local normal-args image with
`rivermmcwriteverify=1`: `river-r30-writeverify-boot.img` SHA-256
`ddb63ff1b6846771719908964357db5fc3b1f222affe930d92dcd847b5f4bb02`.
The kernel string check found both readback diagnostics. `fastboot boot`
returned OKAY. Its live log is `logs-private/r19-live-17.txt`: the user
reached the home screen and used apps, with no freeze or SpringBoard restart
reported. At least **212,992 CMD24 blocks (104 MiB) read back with 0 bad**;
no eMMC errors or heap-execution crash in the final log. This argues against
immediate PIO write/read corruption in this run, but not a wrong source
buffer or rare later failure. The user returned to fastboot. Next, the
normal four-core `r29-diag-4c` image (0151 crash context, no write verifier)
was accepted by `fastboot boot`. Its live log is
`logs-private/r19-live-18.txt`: about ten minutes of active use and more
than 125 logged touch events, no heap-execution crash or eMMC error.
User: Settings loads/scrolls; Safari works with keyboard/typing; Messages
loads/scrolls; Phone loads, dials and scrolls. Photos opens to a black
screen; App Store remains at loading. `MobileSlideShow` and `AppStore`
executed without a logged crash, so these are separate app issues, not
the heap-execution signature. User returned to fastboot for a repeat
of the same diagnostic image.

### Heap-branch crash captured with 0151 context (r29-diag-4c repeat, r19-live-19.txt)
locationd[61] SIGBUS code 2, pc == far 0x1e76d84e0 (malloc tag 35 region
0x1e6000000-0x1e8000000), ESR 0x8200000f; LR-4 insn 0x94004e9e = `bl`
(direct call, i.e. into objc_msgSend). x0 receiver 0x11a973760 whose isa
0x0000a5a1e76d7e55 -> class 0x1e76d7e50 (= x15): **the class object itself
is in the malloc heap** (runtime-created class, e.g. KVO/Swift metadata).
x11 0x3f00011a01d400 = cache mask 0x3f, buckets 0x11a01d400; x12 0x3c,
x13 0x11a01d7a0 = probed bucket; x1 = x9 = SEL 0x187df590e. Branch target
x16 = x17 = 0x1e76d84e0, 0x690 bytes after the class, and its memory holds
pointers (data, not code). With the non-PAC IMP ^ class encoding the stored
value would be ~0xfab0, not a valid encoded IMP: the **method-cache bucket
content is wrong** (or read with the wrong encoding/format for this class).
Next: dump the bucket at x13 (and x10 buckets / class cache_t) on the next
occurrence; check objc4 (iOS 16) cache formats (preoptimized vs dynamic,
CACHE_IMP_ENCODING, bucket write ordering) against anything in our kernel
that could make another core see a stale bucket (or a stale page).

### r31 GPU survey (0153, rivergpu=1): all off, as expected
GDSC gx 0x08225801 / cx 0x00222801 (collapsed, PWR_ON clear); GFX3D RCG
cmd 0x80000000 cfg 0; CBCR gfx3d/ahb/bimc_gfx/timer/aon all CLK_OFF; GFX LDO
ctrl 0x80000038 (clamped, PDN, SW ctrl) status 0. GPU registers not read.
A506 needs a530_pm4.fw, a530_pfp.fw, a506_zap (adreno-gpulist.h).

### 0154 (prepared, NOT built or phone-tested): rivergpu=2 power-on
LDO init (clamp off, CPR auto-bypass, msm8953_ldo_config) + enable_bhs_mode,
GFX3D 133.33 MHz (CFG 0x505), gdsc_oxili_cx, gfx3d root clock, gdsc_oxili_gx,
branches ahb/bimc_gfx/bimc_gpu(0x59030)/timer/aon; then RBBM status and
ALWAYSON_COUNTER (0x4d2) read twice as proof of life. No RPM CX vote
(assumes CX >= low SVS). Next: build r32 with rivergpu=2, boot, read
"RIVER GPU:" lines; then pull firmware (recovery, vendor partition,
private) and start the CP.

### r32 (0154 rivergpu=2; CI build of 2dd3b64): CX on, GX not
LDO -> BHS (ctrl 0x20000038, clamp released). **gdsc_oxili_cx powered on
(0xa0282800).** gdsc_oxili_gx did not (0x8282800, LDO status 0 = no BHS
ack); GFX3D RCG cmd stayed 0x80000000 (ROOT_OFF) with cfg 0x505 and the
gfx3d branch enabled (0x80004f01). Boot continued normally. GX's
core_root_clk is clk_gfx3d_clk_src (dt-bindings 0x917f76ef), which Linux
force-enables (rcg_set_force_enable: CMD_RCGR ROOT_EN bit 1) before the
GDSC; 0155 does the same.

### r33 (0155; CI build of c64a597): root clock on, GX still off
`GFX3D root forced on, cmd 0x2` (ROOT_EN, ROOT_OFF clear). gdsc_oxili_gx
still 0x8282800, GFX LDO status 0 (no BHS_EN_REST_ACK). 0156 powers the
head switch under software control as switch_mode_to_bhs() does and logs
whether BHS_EN_REST_ACK arrives, before enabling the GX GDSC.

### r34 (0156; CI build of 4f5481b): BHS under SW control, no ack
`BHS under SW control, status 0x4 after 50 x 10 us (NO ACK)`: the GFX LDO
reports LDO_READY (its input, VDD_CX = PM8953 S2 via RPM, is present) but
the head switch never acks; GX stays 0x8282800. DT ldo-enable-corner-map is
all zero, so Linux always uses BHS (handed to the GDSC). Mainline
gcc-msm8953.c: oxili_gx_gdsc has CLAMP_IO with clamp_io_ctrl GCC+0x5b00c,
deasserted (bit 0) before power-on. 0157 returns to enable_bhs_mode and
releases that clamp before enabling GX.

### r35 (0157; CI build of dcf434e): SError panic at the GX clamp (photo IMG_0361)
`GFX3D root forced on, cmd 0x2`, `GX clamp_io 0x1101 -> 0x1100`, then
`panic(cpu 2 ...): Unhandled implementation specific error` (SError) before
the GX GDSC result. Releasing the GX IO clamp while GX was unpowered, with
the GFX LDO reprogrammed and gfx3d root/branch clocks running, produced an
asynchronous bus error. The clamp does sit on the GPU domain's interfaces.
0158 follows mainline order: no GFX LDO writes, RCG configured without
forcing the root, CX on, clamp released immediately before GX, clamp
re-asserted if GX does not power on, branch clocks only after both GDSCs.

### r36 (0158 mainline order; CI build of c92f820): **GPU powered and clocked**, register read SErrors (photo IMG_0362)
`GX powered, clamp_io 0x1101 -> 0x1100`; after power-on GDSC gx 0xa0282800,
cx 0xa0282800 (both PWR_ON); GFX3D RCG cmd 0 cfg 0x505 (root running,
133.33 MHz); CBCR gfx3d 0x4f01, ahb 0x20008001, bimc_gfx 0x4111, bimc_gpu /
timer / aon 0x1 (all running); GFX LDO untouched, status 0x3 (BHS acks, under
GDSC control). Then the first Adreno register read (RBBM_STATUS at
0x1c013d4, Device mapping) -> `panic(cpu 1): Unhandled implementation
specific error` (SError). Mainline msm8953 has no GPU BCR reset; downstream
a5xx_regulator_enable() for A506 just writes RBBM_CLOCK_CNTL after
power/clocks, so registers should be accessible in this state. Unknown
difference (candidates: bus/NoC votes via RPM/msm-bus, TZ XPU/secure
access, clamp bits 8/12 in 0x5b00c, timing). Next: capture Linux's
working register state in recovery (kgsl force_rail/clk/bus_on + devmem).

### Linux reference in recovery (kgsl force_rail/clk/bus_on, debugfs)
Recovery kernel 4.9.337 has kgsl ("Adreno506v1", 320 MHz). Idle values
match our survey; VDD_CX (pm8953_s2_level) is at RPM level 256 (nominal).
With the GPU forced on (firmware load failed: /vendor not mounted):
GFX3D CMD 0 CFG 0x104 (320 MHz); CBCR gfx3d **0x6f01** (ours 0x4f01),
ahb 0x20008001, bimc_gfx 0x4111, bimc_gpu/timer/aon 0x1 (= ours); both
GDSC regulators enabled; GFX LDO mode BHS, corner 3, **PWRSWITCH_CTL
0x20000038** (ours in r36: 0x80000038, LDO_CLAMP_IO still set),
PWRSWITCH_STATUS 0x3, CFG0 0x5008600, CFG2 0xc3fc, VREF_TEST 0x4b1102,
ATEST 0x203. Firmware copied privately to device-private/gpu-firmware
(vendor_b: a530_pm4.fw, a530_pfp.fw; modem_b /image: a506_zap.mdt/.b00-.b02).
0159: LDO probe init (clamp off, EN_LDOAP, init table) + enable_bhs_mode
before the r36 order, and gfx3d CBCR bit 13 as Linux.

### r37 (0159; CI build of 248894b, console-visible rerun): same SError (photo IMG_0363)
`LDO clamp off, BHS to GDSC, ctrl 0x20000038`, `GX powered, clamp_io 0x1101
-> 0x1100`, after power-on: gx/cx 0xa0282800, RCG cmd 0 cfg 0x505, CBCR
gfx3d 0x6f01, ahb 0x20008001, bimc_gfx 0x4111, bimc_gpu/timer/aon 0x1, LDO
ctrl 0x20000038 status 0x3 — every GCC/LDO value we can see now matches
Linux with kgsl forced on. The first GPU register read (0x1c013d4) still
SErrors (`panic(cpu 2): Unhandled implementation specific error`). So the
missing condition is outside these registers. Candidates: msm-bus / RPM
bandwidth votes (kgsl votes the GPU path on power-up), TZ XPU / secure
access to the GPU register space, remaining clamp bits 8/12 in GCC+0x5b00c.
Next: in recovery, start kgsl fully (firmware available) and inspect
msm-bus-dbg / rpm_master_stats / tzdbg while the GPU runs; and test reads
of other GPU-side addresses (GPU SMMU 0x1c40000) to see if the whole
region faults.

### Recovery follow-up: GPU path is not a bus-vote problem
DT: `slv-gpu-cfg` (CPU -> GPU register port) is a slave of `fab-pcnoc`, the
same NoC as disp-ss-cfg, sdcc-1, tlmm and blsp, which XNU already uses, so a
missing RPM/msm-bus vote cannot explain an SError on GPU register reads (the
grp3d vote 26 -> 512 is GPU -> DDR bandwidth). RPM bus clocks in recovery are
identical with kgsl forced on or off (bimc 931/178 MHz, pcnoc/snoc 100 MHz,
sysmmnoc 66 MHz). GFX LDO is fed from VDD_CX (pm8953_s2_level, level 256);
gfx_mem_acc_corner (0x194415c bit 0) only selects SRAM timing. With
/vendor and /firmware mounted kgsl loaded PM4/PFP, failed the zap shader
(not on recovery's search path) and then refused opens with EAGAIN; it wrote
GPU registers in a5xx_start before that without an SError, so HLOS access is
allowed by TZ in this power state. Recovery has no /dev/mem, so GCC+0x5b00c
cannot be read there.
0160: after power-on, barrier + 10 ms and DAIF in the log, then read the
CX-side GPU SMMU (0x1c40000 sCR0/IDR0/IDR1), then RBBM_STATUS, with a log
line before each access, to localise the fault.

### r38 (0160; console-visible, sha256 32ad130d...): whole GPU block faults (photo IMG_0364)
Power-on as r37, then `power-on writes settled (DAIF 0x200)` (SError
unmasked, nothing pending from the GCC/LDO writes), `reading GPU SMMU sCR0`
-> `panic(cpu 2): Unhandled implementation specific error`. The CX-side GPU
SMMU faults as well, so the whole 0x1c00000 block is closed, not just GX.
Mainline qcom_iommu.c restores the block's TZ secure configuration whenever
OXILI_CX_GDSC comes back (runtime resume and first attach):
qcom_scm_restore_sec_cfg(qcom,iommu-secure-id 18, 0) (SVC_MP 0x0c, cmd 0x02,
SMC64 SIP std call 0x42000c02). XNU never calls it; MDSS works because ABL
never collapses it.
0161: issue that SCM call (with an IS_CALL_AVAIL query logged) after power-on
and before any GPU access; skip the reads if TZ returns an error.

### r39 (0161; console-visible, sha256 3a819859...): **Adreno 506 alive** (r39-live-2.txt)
`TZ restore_sec_cfg available 0/1; restore(18,0) status 0 result 0`, then
`SMMU sCR0 0x200436`, `IDR0 0x78014404 IDR1 0x20008004`, `RBBM_STATUS 0x1`
(only the AHB-busy bit of our own access), STATUS3 0, INT_0_STATUS 0,
CP_RB_BASE 0, `ALWAYSON_COUNTER 0x47b5b -> 0x4d28e after 1 ms (GPU alive)`
(~22k ticks: 19.2 MHz XO plus delay() slack). The missing step was the TZ
secure-config restore after OXILI_CX_GDSC leaves collapse. Boot continued to
SpringBoard with the GPU left powered. Next: CP bring-up (a5xx_start
register setup, PM4/PFP microcode load, then the zap shader or
RBBM_SECVID_TRUST_CNTL to leave secure mode, a minimal ring).

### 0162 (prepared): rivergpu=3 command processor bring-up
bsd/dev/river_gpu.c follows mainline a5xx_hw_init (A506 branches, offsets
grepped from Linux v6.6 a5xx.xml.h; no HWCG/preemption/GPMU) with one
contiguous 64 KiB buffer (PM4 microcode minus its first dword, PFP, 4 KiB
ring, scratch), surveys the GPU SMMU (SMR/S2CR, CB0 SCTLR: physical if M=0),
clears ME_HALT, sends CP_ME_INIT (A506 workaround 0x0b), then a PKT4 write of
CP_SCRATCH_REG0 and a CP_MEM_WRITE to memory, logging RPTR/RBBM/fault state.
Still in secure mode (no zap shader yet), so the memory write may be refused.
Microcode reaches the kernel only through the local RAM-only boot image:
tools/river-gpufw.py packs ../device-private/gpu-firmware, mkpayload.py
--blob RiverGPUFW=... appends it and names it in /chosen/memory-map
(RIVER_GPUFW=1 tools/kbuild-river.sh). Nothing Qualcomm enters git or CI.

### r40 (0162; second CI build, first skipped 0162): microcode reaches the kernel
The first r40 build lacked 0162: the patch re-created bsd/conf/files as a new
file (it was untracked in the scratch tree) and dxb skipped it with a warning;
fixed with a context hunk (7c28640). Rebuilt r40: GPU alive as r39, then
`firmware container at 0xa2590000+0xd930 incomplete (pm4 19652, pfp 17108)`:
the ADT blob arrived intact; the 16 KiB PFP slot was too small. 0163: PM4 and
PFP slots of 24 KiB (PFP at +0x6000).

### r41 (0163; console-visible, sha256 1e32e8e3...): **Adreno 506 command processor runs** (r39-live-5.txt)
```
microcode pm4 19652 bytes [0x5ff066 0x12b0 0x88020003], pfp 17108 bytes [0x5ff11a 0x1034 0x113001]
SMMU SMR 0x80010000 0x80000002 0x80000003 0 S2CR 0x30000000 0x30000001 0x30000003 0x10000
SMMU CB0 SCTLR 0 TCR 0x80000000 FSR 0x200
buffers at pa 0xa4f34000 (pm4 +0 pfp +0x6000 ring +0xc000 scratch +0xf000)
hw_init done; RBBM_STATUS 0x1 CP_PFP_ME_CNTL 0x303 rptr 0
CP_ME_INIT consumed (rptr 9), RBBM_STATUS 0x1
CP wrote CP_SCRATCH_REG0 = 0x5060a5e (expect 0x5060a5e)
CP_MEM_WRITE consumed; memory now 0x1c0ffee5 (expect 0x1c0ffee5, was 0xdeadbeef); SMMU CB0 FSR 0x200 GFSR 0
```
The CP runs the PM4/PFP microcode, fetches the ring from DRAM, executes
CP_ME_INIT, writes a register (PKT4) and writes DRAM (CP_MEM_WRITE) at a
physical address. GPU SMMU: stream matches 0/2/3 (valid SMRs) go to
S2CR type 0 (translate) CB0/CB1/CB3; CB0 (gfx3d_user) has SCTLR.M = 0, so
GPU addresses are physical; FSR 0x200 is only the FORMAT field (no fault),
GFSR 0. The memory write succeeded without a zap shader, so secure mode does
not block plain CP memory writes; the zap shader is still needed before
shader execution (Linux's CP_SET_SECURE_MODE 0 path).
Next: zap shader through TZ PIL (pas_init_image / mem_setup /
auth_and_reset, PAS id 13, from the RiverGPUFW container), then
CP_SET_SECURE_MODE 0; then a first 2D blit/clear into a framebuffer, and a
userland path (freedreno/GLES or a Metal-shaped driver).

### 0164 (prepared): zap shader through TZ PIL, then CP_SET_SECURE_MODE 0
Zap layout (parsed locally): a506_zap.mdt = phdr0 (ELF header + 3 phdrs,
0x94) + hash segment (type 2, 0x1988) packed, i.e. the whole .mdt is the
pas_init_image metadata; one PT_LOAD (a506_zap.b02, 0x1454 bytes, p_paddr
0x5000, relocatable, align 1 MiB), so TZ range = 0x2000. 0164 votes the
crypto clocks (GCC 0x45004 bits 0-2; crypto_clk_src 0x16004 = GPLL0/10 =
80 MHz, as qcom,kgsl-hyp's scm clocks), copies metadata and segment into
physically contiguous memory (segment 1 MiB aligned), then SCM PAS calls
for id 13: is_supported (2/7), init_image (2/1, ARGS(2,VAL,RW)), mem_setup
(2/2), auth_and_reset (2/5); on success CP_SET_SECURE_MODE 0 and another
CP_MEM_WRITE to confirm the CP still runs.

### r42 (0164; sha256 2909690a...): TZ accepts the zap metadata, refuses the region (r39-live-6.txt)
`crypto clocks: rcg cmd 0 cfg 0x113 vote 0x1e407 cbcr core 0x4ff0 axi 0 ahb
0x20008000`; `PAS 13 supported status 0 result 1`; `pas_init_image status 0
result 0` (signature/hash accepted); `pas_mem_setup status -2 result
0xfffffffffffffff0` for 0xa5000000+0x2000 (kernel RAM). Mainline
msm8953.dtsi uses a fixed no-map zap@81800000 (0x2000), inside kgsl-hyp's
gpu_region alloc range 0x80000000-0x90000000; on river that span has only
dynamic pools below the first carve-out at 0x84300000 and is outside XNU's
RAM. 0165: load the segment at 0x81800000 through a Device mapping.

### r43 (0165; sha256 a27b739f...): region accepted, auth_and_reset refused (r39-live-7.txt)
`pas_init_image status 0`, `pas_mem_setup status 0 result 0` (0x81800000 +
0x2000 accepted), `auth_and_reset status -2 result 0xfffffffffffffff1`.
Locally: sha256(a506_zap.b02) is entry 2 of the hash segment (b01 at 0x68),
.mdt = b00 + b01, so the loaded content is right.

### Linux reference: zap shader loads in recovery
RAM-only: vendor_b and modem_b mounted read-only just to copy a530_pm4/pfp
and a506_zap.* into a tmpfs over /vendor (/odm/firmware -> /vendor/odm/
firmware), relabelled vendor_firmware_file for ueventd, then kgsl force
flags + open: `a506_zap: loading from 0x8f800000 to 0x8f802000`, `a506_zap:
Brought out of reset`. msm-bus client scm_pas votes crypto master 55 -> EBI
512 (ab 393.6 MB/s, ib 3.936 GB/s) only around init_image and
auth_and_reset. Linux's IOMMU driver also donates the GPU SMMU secure
page-table pool once per boot (qcom_iommu_sec_ptbl_init), which XNU never
did; TZ needs it to map the zap into the secure context bank, matching
"first three calls pass, auth_and_reset fails". Afterwards: force flags 0,
tmpfs unmounted, partitions unmounted.
0166: IOMMU_SECURE_PTBL_SIZE / _INIT (SVC_MP 3/4) with the pool at
0x82000000 (unused low span, zeroed via a Device mapping) before the PAS calls.

### r44 (0166; console-visible): **zap shader loaded, GPU out of secure mode** (r39-live-8.txt)
```
iommu secure ptbl size status 0 size 0x21000 result 0
iommu secure ptbl init at 0x82000000 size 0x21000 status 0 result 0
zap: PAS 13 supported status 0 result 1; metadata 0x1a1c bytes, segment 0x1454 bytes at 0x81800000 (TZ range 0x2000)
zap: pas_init_image status 0 result 0
zap: pas_mem_setup status 0 result 0
zap: auth_and_reset status 0 result 0
out of secure mode; CP_MEM_WRITE consumed, memory 0x2a506a5e (expect 0x2a506a5e); RBBM_STATUS 0x1 INT_0 0
```
The missing piece was the GPU SMMU secure page-table pool (132 KiB, now at
0x82000000); no crypto bus vote was needed. The full mainline a5xx_hw_init
sequence now runs under XNU: power, TZ restore_sec_cfg, hw_init, microcode,
CP_ME_INIT, zap shader, CP_SET_SECURE_MODE 0, CP still executing.
Fixed physical layout in the unused span below the 0x84300000 carve-out:
0x81800000 zap (0x2000), 0x82000000 SMMU secure ptbl (0x21000).
Next: first rendering work (CP_BLIT / 2D clear into a scanout buffer the
MDP shows), then decide the userland path (freedreno vs Metal-shaped).

## GPU driver approach (2026-10-21): Metal front end on Mesa (option C)
Decision with the user: QuartzCore on iOS 16 renders through Metal (or the
CPU), so only a Metal driver accelerates SpringBoard/UIKit. Plan C keeps
that goal but reuses Mesa for the hard GPU-specific parts: AIR (LLVM) ->
SPIR-V -> NIR -> ir3 for shaders, Gallium freedreno for command streams and
memory; we write the Metal API subset QuartzCore uses and the XNU kernel GPU
service. Order: (1) kernel GPU service: GPU address space (SMMU CB0 page
tables), GPU memory objects, submission + fences + GPU IRQ (SPI 33), a
/dev/rivergpu interface for Mesa; (2) Mesa freedreno on it, proven by a GLES
test drawing on the panel; (3) Metal front end until QuartzCore renders.
Apple binaries and anything derived from them stay off git and off the
machine; only interface facts go into notes.

### 0167 (prepared): GPU SMMU translation with our own page tables
Mainline qcom_iommu_init_domain + io-pgtable ARM_32_LPAE_S1 (CB0 FSR FORMAT
01 = AArch32 long descriptor): 3-level 4 KiB tables from a 64 KiB pool,
TCR 0x80802020, TCR2 0x38002, MAIR0 0xf404ff44, PTE 0xe43 (RO 0xec3), ASID
0, SCTLR 0x1067 (mainline minus CFCFG: terminate faults, no stall). The
64 KiB GPU buffer is mapped at GPU VA 0x01000000 (microcode read-only) and
the CP gets GPU addresses only. Last test: CP_MEM_WRITE to unmapped
0x02000000 should fault in CB0 (FSR.TF, FAR).

### r45 (0167): **GPU runs on our page tables; translation enforced** (r39-live-9.txt)
`GPU SMMU CB0 translating: TTBR0 0xa4f2c000 SCTLR 0x1067 TCR 0x80802020`;
the whole r44 sequence (ME_INIT, scratch register, memory write, zap, secure
mode off, memory write) now runs with the CP fetching microcode and ring and
writing memory through GPU VAs. `unmapped write consumed: CB0 FSR 0x202 FAR
0x02000000 FSYNR0 0x12`: translation fault (TF) at exactly the unmapped GPU
address, FSYNR0 WNR (write), terminated without stalling; the CP kept going
(RBBM_STATUS 0x1). The GPU can no longer reach arbitrary physical memory.
Next (kernel GPU service): GPU memory objects with a GPU VA allocator, a
submission path with fences (CP_EVENT_WRITE / CACHE_FLUSH_TS) and the GPU
IRQ (SPI 33) instead of polling, then the /dev/rivergpu interface for Mesa.

### 0168 (prepared): GPU memory objects, fenced submission, GPU IRQ
river_gpu.c service layer: buffer objects = wired kernel pages
(kmem_alloc, not contiguous) mapped 16 KiB at a time into the GPU VA heap
0x01100000-0xf0000000 (first fit), cleaned/invalidated around GPU access;
page-table pool grown to 1 MiB (256 tables) with per-table dirty cleaning;
gmmu_unmap + kmem_free on release. Submission as mainline a5xx_submit minus
preemption: CP_INDIRECT_BUFFER_PFE per command buffer, seqno to
CP_SCRATCH_REG(2), CP_EVENT_WRITE CACHE_FLUSH_TS | IRQ to a fence word; the
4 KiB ring wraps (space from CP_RB_RPTR). GPU IRQ GIC SPI 33 (INTID 65,
level) -> RBBM_INT_0_STATUS, clear, wake waiters; mask = CP_CACHE_FLUSH_TS,
CP_HW_ERROR, UCHE_OOB_ACCESS. Boot self-test: 200 submits of a
CP_MEM_WRITE command buffer each (ring wraps ~2x), one fence wait, check all
200 words, free, re-allocate (heap reuse).

### r46 (0168): **GPU buffer objects + fenced submission + GPU IRQ work** (r39-live-10.txt)
```
service up: GPU IRQ INTID 65 registered, ring 1024 dwords, BO heap 0x1100000-0xf0000000
BOs: target 0x10000 bytes at GPU 0x1100000, IBs 0x4000 bytes at GPU 0x1110000
200 submits (ring wrapped 2 times): fence signalled at 200 (want 200) in 1371 us, 200/200 writes ok, 198 IRQs, 0 error IRQs, last INT 0x100000
BOs freed; reallocation got GPU 0x1100000 (heap reuse ok)
```
Non-contiguous BOs through our page tables, 200 indirect-buffer submissions
(~6.9 us each incl. CPU cache maintenance), ring wrap, CACHE_FLUSH_TS fences
and the SPI 33 interrupt (198 IRQs for 200 fences: some coalesced), no GPU
error interrupts, BO free + GPU VA reuse. Unmapped-write check still faults
(FAR 0xf0000000).
Next: the userland interface (BO create/map into a task/free, submit with
BO list + fence, wait) so Mesa freedreno can run in an iOS process.

### 0169 (prepared): RiverGPU IOKit user client + development file drop
- iokit/Kernel/arm/RiverGPU.cpp: `RiverGPU` (matches IOResources once
  river_gpu.c publishes `RiverGPUReady` after its self-test) and
  `RiverGPUUserClient`: 0 GET_PARAM, 1 BO_NEW, 2 BO_FREE, 3 SUBMIT (struct:
  ncmd, nbos, {handle, offset, size_dw}[], handles[]), 4 WAIT, 5 CPU_PREP;
  IOConnectMapMemory64(type = handle) maps a buffer (cached; SUBMIT cleans
  named buffers, CPU_PREP waits + invalidates). msm DRM model for Mesa.
  Bring-up limits: one GPU address space for all clients, no command parsing.
- bsd/dev/river_files.c + bsd_init hook: files packed by tools/river-files.py
  (`RIVER_FILES="SRC:DEST[:MODE] ..."` in kbuild-river.sh, DEST under
  /usr/local/ only, parents created via a RIVER-only mkdir1at wrapper) are
  written to the rootrw root just before PID 1.
- kern_exec.c: boot-arg `riverinit=/path` runs that program as PID 1 instead
  of launchd (launchd if it fails).
- tools/rivergpu-test.c (+ build-rivergpu-test.sh, ad-hoc signed): waits for
  RiverGPU, reads params, maps two BOs, one submit of 256 CP_MEM_WRITEs as two
  command buffers + wait + cpu_prep + check from the user mapping, 500
  submit+wait round trips, refused bad requests, free, close; never exits.

### r47 (0169; riverinit test boot): service and dev loop work, user results only on screen
Kernel: full GPU bring-up + self-test (200 submits in 790 us), then
`RiverGPUReady published for user clients`, `RiverGPU service registered`.
river_files.c installed /usr/local/bin/rivergpu-test (its log line fell
before the receiver's replay) and `riverinit` ran it as PID 1: libignition
"hello from rivergpu-test.1", the shared cache mapped as for launchd (the
"not in trust cache" lines appear for launchd too), then a sandbox check
from rivergpu-test. Its own printf output goes to the screen console only,
not the USB log; 0180 adds a LOG selector (6) that the test now also uses.

### Upstream MakrSas/ios-topaz (18 commits since 590f001) imported
Upstream reports CoreAnimation's **OpenGL ES renderer** on Mesa freedreno
drawing SpringBoard/Settings on its Adreno 610 (HackiOS patches, not
public). Imported (renumbered; both forks used 0127-0139): 0170 = topaz
0128 IOKit quiesce, 0171-0177 + 0179 = topaz 0131-0137, 0139 SweetSurface
for the GL renderer, 0178 = topaz 0138 empty IOAcceleratorES (QuartzCore
polled for it 300 x 100 ms at every backboardd start; Metal skips one
without MetalPluginName/MetalPluginClassName). All apply in order on river
and compile. Cherry-picked 3eacb48 (mkadt: artwork-display-gamut sRGB,
blank Safari pages). Not taken: 0127 sweetbusy (diagnostic, conflicts),
0129/0130 Wi-Fi and the wlan ADT node (different Wi-Fi chip on river).
Plan update: GL first (freedreno behind CA's GLES renderer, as upstream),
Metal later.

### r48 (0170-0180 imports, GUI boot): IOAcceleratorES still missing on river
backboardd logged `IOK backboardd: matching IOAcceleratorES -> 0 found`
repeatedly: topaz 0138's registration landed inside AppleCLCD::start's
`#if !defined(RIVER)` sweet MDP/DSI block, so river never published it (no
"empty IOAcceleratorES published" line). 0181 moves the block above that
#if. The file drop and GPU service ran normally in this boot.
User report on r48: home screen reached; swiping into the App Library works
better with no visual glitches, and the keyboard now works and draws fully:
the imported SweetSurface fixes (0171-0177, 0179) help the CPU renderer too.

### r49 (0181): empty IOAcceleratorES published on river: ~38 s faster boot
Measured from iOS log timestamps (first launchd line -> first SpringBoard
log line): r39-r48 boots 118-142 s (typ. ~135 s), each with 302
`IOAcceleratorES -> 0 found` polls; r49 97 s with none (backboardd finds it
at once, then looks it up again per CA context: 293 quick `matching1`
lookups). User stopwatch, first boot line to lock screen: 1:49.

### r49t (r49 kernel + riverinit, packed locally): **user space drives the GPU** (r39-live-14.txt)
```
RIVER FILES: /usr/local/bin/rivergpu-test (51024 bytes, mode 755): installed (0)
load_init_program: attempting to load /usr/local/bin/rivergpu-test
RIVER GPU [rivergpu-test]: IOServiceOpen -> 0
RIVER GPU [rivergpu-test]: param chip-id = 0x5000600, gpu-id = 0x1fa (506), gmem-size = 0x22000, va-start = 0x1100000
RIVER GPU [rivergpu-test]: BOs target 1 gpu 0x1100000 cpu 0x100a04000 (0), cmd 2 gpu 0x1110000 cpu 0x100a14000 (0)
RIVER GPU [rivergpu-test]: submit 0 fence 201 wait 0 prep 0 in 0.195 ms: 256/256 words written by the GPU
```
A normal process opened RiverGPU, mapped two GPU buffers into itself, wrote
a 256-command stream from user space, submitted it as two command buffers,
waited on the fence and read all 256 GPU-written words through its own
mapping (0.195 ms). The 500 round-trip and bad-request checks were invalid:
the test sent its fixed two-command struct with a 24-byte length, so the
kernel read the empty second command slot as the buffer handle and refused
every one-command submit. Test fixed (packs ncmd/nbos/commands/handles);
rerun pending.
Rerun with the fixed test (r39-live-15.txt): `submit 0 fence 201 ... in
0.288 ms: 256/256 words written by the GPU`, `500 submit+wait round trips:
500 ok, 120.3 us each` (IOKit call + kernel submit + GPU + IRQ + wake +
second IOKit call), `refused bad handle / out-of-range offset / bad prep:
0xe00002c2` (kIOReturnBadArgument each), `BO_FREE 0`. The kernel GPU
service is complete enough for a userland driver: next, Mesa freedreno on a
small libdrm-style shim over RiverGPU (GEM_NEW/INFO/SUBMIT/WAIT/CPU_PREP).

## Mesa freedreno for river (started 2026-10-21; see mesa/README.md)
Mesa 26.2.5 cross-compiles for iOS arm64 with a new `river` freedreno
kernel backend over RiverGPU (patch mesa/patches/0001). Built so far:
libfreedreno_drm, libfreedreno_ir3, gallium libfreedreno.a. Next: link and
run `river_gallium_test` on the phone (sign with codesign -s -, pack with
RIVER_FILES to /usr/local/bin, boot with riverinit=), expect a GPU clear
through Mesa's blitter (ir3 shaders + draw) to read back as 00 80 ff ff.

### r49g (r49 kernel + river_gallium_test, locally packed): screen created, context crash
Signed the linked iOS arm64 test, packed it as RiverFiles in a RAM-only image,
and booted it twice. The second boot had the USB receiver already running;
private log `logs-private/r49-gallium-live-2.txt` captures the result. The
kernel GPU self-test passed (200/200 writes, 0 error IRQs). PID 1 opened
RiverGPU and Mesa created screen `FD506` by `freedreno` in 32.5 ms. Mesa
logged unsupported `FD_TIMESTAMP` (id 6) and `FD_UCHE_TRAP_BASE` (id 13)
queries, both nonfatal during screen creation. Immediately after the screen
log, PID 1 exited with namespace 2 subcode 0xb (SIGSEGV), then XNU panicked
because init exited. No GPU clear was submitted. Photo: private IMG_0366.
Hypothesis: crash inside `fd5_context_create` or the call leading into it;
the exact site is not yet observed. Mesa tree has temporary `RIVER DIAG`
console markers around context setup, built and signed. Diagnostic RAM image:
`shim-river/out/river-r49-gallium-diag-boot.img` (SHA256
5a7ca58d2084304f1c3a7eaa111f6cf11239a82af8fa1ac038afec35c72f2f80).
Next: when phone is in fastboot, RAM-boot this image with the receiver already
running, inspect the last `RIVER DIAG` marker, fix the actual fault, remove
temporary markers, then rerun the clear test.

Diagnostic boots with the USB receiver running:
- `r49-gallium-live-3.txt`: process enters `fd5_context_create`, then SIGSEGV.
- `r49-gallium-live-4.txt`: allocation and draw/compute/GMEM/texture init
  pass; SIGSEGV during `fd5_prog_init`.
- `r49-gallium-live-5.txt`: `ir3_cache_create` and `ir3_prog_init` pass;
  SIGSEGV during `fd_prog_init` (built-in shader creation). These are observed
  last markers, not yet a proven exact instruction.
- A fourth diagnostic image with markers around TGSI parsing, NIR conversion,
  and shader creation is ready at
  `shim-river/out/river-r49-gallium-diag4-boot.img` (SHA256
  d9724b240bb94ecc5855a814ae0495194681dd888eede2e53c8b6049cd59e4c6).
  Waiting for the phone to return to fastboot; no fourth diagnostic boot yet.

Further observations (all private USB logs):
- `r49-gallium-live-6.txt`: `fd_prog_init` reaches `ir3_shader_state_create`
  for the built-in fragment shader, then SIGSEGV.
- `r49-gallium-live-7.txt`: TGSI parses and NIR builder completes; SIGSEGV
  inside `ttn_finalize_nir`.
- `r49-gallium-live-8.txt`: the first ten NIR lowering passes and
  `nir_lower_tex` finish; SIGSEGV before the uniform-lowering completion.
- `r49-gallium-live-9.txt`: clip-distance check completes, then
  `nir_lower_uniforms_to_ubo` is called (option=1) and SIGSEGV occurs inside.
  The exact operation in this function is still unknown.
Next signed diagnostic RAM image has markers inside that pass:
`shim-river/out/river-r49-gallium-diag8-boot.img` (SHA256
17e897cfd1a880d380ec8350dbe8849c04d1ec8d3739d6350935b21991a8aa94).
It is built but not yet booted; phone needs fastboot. Temporary `RIVER DIAG`
markers in the Mesa tree must be removed after the fault is fixed.

### r49g GLSL cache fix: Mesa GPU clear succeeds
The diag8 boot reached `nir_lower_uniforms_to_ubo`; the phone photo
`IMG_0383.HEIC` (private) showed `uniform instructions pass progress=1`
then `before glsl_array_type uniforms=1` followed by a PID 1 SIGSEGV. The
direct Gallium test bypassed Mesa's GLES state tracker and had never called
`glsl_type_singleton_init_or_ref()`, so `glsl_array_type()` had no cache
allocator. Initialize the GLSL type singleton before `fd_screen_create()`
and decref after the test. No kernel change was needed.

The locally repacked, signed RAM image
`shim-river/out/river-r49-gallium-glslinit-boot.img` (SHA256
`1e991677a99a4487d1944f0f29671db96222304ed886fa9cb74823ca1daba10f`)
was booted with the USB receiver already running. Private log
`logs-private/r49-gallium-live-11.txt` records `context_create complete`,
both blitter shaders converting and compiling, then:
```
GPU clear of 64x64 RGBA8 (0, 0.5, 1, 1): fence signalled in 34.42 ms, 4096/4096 pixels right, pixel 0 = ffff8000
```
This verifies Gallium context setup, shader compilation, a real A506 draw,
submit, fence IRQ and CPU readback. The two unsupported param queries
(`FD_TIMESTAMP`, `FD_UCHE_TRAP_BASE`) were nonfatal. Temporary `RIVER DIAG`
source markers were removed after this boot and the Mesa patch regenerated.

Next: GLES behind CoreAnimation. Public Pixel 1 reference:
https://github.com/joranv1/ios-sailfish/tree/main/tools/sweetgl . Its
`sweetgl.c` creates Mesa GLES contexts through `st_api_create_context()`;
`eagl.c` adapts CoreAnimation's EAGL/GLI dispatch and imports IOSurfaces;
`sweet_drm.c` emulates MSM DRM over its SweetGPU IOKit service. River can
reuse the frontend concepts with its existing freedreno river KMD. The
Pixel `mesa.patch` only adapts mmap and IOSurface pitch; it does not fix
the direct Gallium test's GLSL cache issue.

### r49 GLES state tracker: offscreen shader draw succeeds
Built Mesa's GLES state tracker and a 19 MB iOS arm64
`river_gles_test`. Mesa's iOS Meson path needed `BUILDING_MESA`, as the
macOS path already sets, to compile Mesa's shader object handle types.
The first RAM-only GLES image booted and created screen `FD506`; private
`r49-gallium-live-12.txt` then shows PID 1 SIGSEGV during
`st_api_create_context`, before GL calls. Mesa 26.2.5 calls the frontend
`get_param` callback near the end of that function; our minimal frontend
had left it null. The test now provides a zero-returning callback, and a
signed second RAM image contains markers around context initialization:
`shim-river/out/river-r49-gles-diag1-boot.img` (SHA256
`9b19b4585068f92986a45112c6419c66f75f48320a54070067a79923d787ac0a`).
The second RAM boot passed. Private `r49-gallium-live-13.txt` records all
state tracker markers through `get_param completed`, then Mesa reports
`OpenGL ES 3.1` with renderer `FD506`. The test linked its shaders, rendered
an orange triangle on blue into a 64×64 FBO, and read back the expected
pixels: near `ff4000ff`, far `0000ffff`, framebuffer complete, GL error 0,
`PASS`. The source was cleaned of temporary markers after making that image.
Next: a GLES dylib for backboardd, the CoreAnimation EAGL/GLI bridge, and
IOSurface imports. The Pixel port injects its dylib with a boot-arg selected
`DYLD_INSERT_LIBRARIES`; River needs a comparable controlled injection path.

### r49 GLES shared library: dynamic load and draw pass
Refactored the working Mesa state tracker frontend into an iOS arm64
`librivergl.dylib` with context create/bind/destroy exports, and linked the
GLES triangle test against it. The dylib carries Mesa's GL dispatch entry
points and has install name `/usr/local/lib/librivergl.dylib`. Both files
were ad-hoc signed and installed from RiverFiles in a RAM-only image:
`shim-river/out/river-r49-gles-dylib-boot.img` (SHA256
`db4dbf379ce6412a95c3206a3935fc46253efd217f3a77dc67fe3a6de3bc4674`).
Private `r49-gallium-live-14.txt` shows PID 1 loaded the library, made a
GLES 3.1 context on FD506, linked shaders, and rendered the expected two
pixel colors into its FBO with GL error 0 and `PASS`. This is a dynamic
linking and offscreen rendering result; CoreAnimation has not been hooked
yet. Next: EAGL/GLI adapter, IOSurface sharing, and backboardd injection.

### r50 (0182; CI run 39704395933): **library injected into backboardd** (r49-gallium-live-15.txt)
0182 adds boot-arg `riverenv=NAME:VAR=VALUE`: exec appends VAR=VALUE to the
environment of executables named NAME. Without an AMFI kext, dyld's policy
query `__mac_syscall("AMFI", 0x5a)` failed, so dyld ignored every DYLD_*
variable. A RIVER-only MAC policy named "AMFI" now answers 0x7b (all but a
custom shared cache) for NAME only, and is registered only when riverenv is
set. Every other process still gets ENOPOLICY. Test library:
`tools/river-inject-probe.c` (logs via RiverGPU LOG, changes nothing).
RAM image `shim-river/out/river-r50-probe-boot.img` (r49 GUI boot-args +
`riverenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libriverprobe.dylib`,
SHA256 32487f5fe9d2255ae347dd12e76d81199721ed26d8fd2dcfab5548666f90840a):
```
RIVER ENV: backboardd gets DYLD_INSERT_LIBRARIES=/usr/local/lib/libriverprobe.dylib
RIVER AMFI: dyld policy for backboardd[56]: in 0x0 out 0x7b
RIVER GPU [backboardd]: probe: loaded in backboardd pid 56 uid 501, DYLD_INSERT_LIBRARIES=..., 301 images so far
```
After that, the probe saw backboardd load IOSurface, IOSurfaceAccelerator, Metal,
IOAccelerator, QuartzCore, OpenGLES (libGFXShared, libGLImage,
libCoreVMClient, libCVMSPluginSupport), Metal Performance Shaders, and later
`OpenGLES.framework/GLEngine.bundle/GLEngine` and libGLProgrammability. So
CoreAnimation in backboardd does bring up Apple's GL engine on river; that is
the layer the EAGL/GLI adapter must replace. SpringBoard started and no
backboardd exit was logged. Home screen not yet checked visually.
Next: EAGL/GLI adapter in a River-written dylib (log first calls, fall back
to the original EAGL), then IOSurface import or copy.

### r51 / r51b: EAGL observer in backboardd (r49-gallium-live-16/17.txt)
r51 (`tools/river-eagl`, hooks installed from an `objc_addLoadImageFunc`
callback) crashed backboardd with SIGTRAP right after the constructor's first
log line, before any class was hooked; launchd restarted it nine times and the
UI never came up. Cause not symbolicated; the callback is the suspect. r51b
links OpenGLES and hooks in the constructor: no crash, SpringBoard started.
Image `shim-river/out/river-r51b-eagl2-boot.img` (r50 kernel, SHA256
9e38f65f3e4f4bb4fb91d3710f1fc00f7fcd67b0e00ce068a5bcffb8fec8f7d4).

Interface observed on 16.7.16 (type encodings logged at runtime):
- EAGLContext: +setCurrentContext: `B24@0:8@16`, +currentContext; 25 instance
  methods incl. initWithAPI:properties: `@32@0:8Q16@24`,
  commonInitWithAPI:properties:, initWithAPI:sharegroup:,
  initWithAPI:sharedWithCompute:, getMacroContextPrivate and
  GetMacroContextPrivate `^{EAGLMacroContext=}16@0:8`,
  texImageIOSurface:target:internalFormat:width:height:format:type:plane:
  [invert:] (`B68...`/`B72...`), attachImage:toCoreSurface:invertedRender:
  `B36@0:8Q16^{__IOSurface=}24B32`, renderbufferStorage:fromDrawable:,
  presentRenderbuffer:[atTime:|afterMinimumDuration:], setParameter:to: /
  getParameter:to: `Q28@0:8I16^i20`, setBlockFence:onQueue:, API,
  sharegroup, setMultiThreaded:/isMultiThreaded, debugLabel.
- EAGLSharegroup: initWithAPI:[sharedWithCompute:], getGLIShared
  `^{__GLISharedRec=}16@0:8`, loadGLIPlugin:sharedWithCompute:
  (`^{__GLDPixelFormatRec=...}`), APIs, debugLabel.

Observed calls: CoreAnimation calls `-[EAGLContext initWithAPI:properties:]`
with API 3 (GLES3) then API 2 (GLES2); each goes through
commonInitWithAPI:properties: and EAGLSharegroup initWithAPI:sharedWithCompute:,
then the sharegroup and context are deallocated at once (init fails, returns
nil). This repeats on one thread more than 1000 times in about 2 minutes. No
getMacroContextPrivate, present, IOSurface or currentContext calls ever happen.
So Apple's GLEngine cannot create a context on river, and CA stays on its CPU
renderer while retrying. The GLI dispatch table could not be measured from
Apple's object because none is created.
Next (stage 2): for API 2/3, make initWithAPI:properties: produce a context
backed by librivergl (Mesa) and give CA an EAGLMacroContext whose GLI dispatch
table calls Mesa. The table layout must match this build's GLEngine (Pixel
port reference: 1036 slots for the same 16.7.16 userland; verify independently).

### r52 / r52b (0183 kernel, CI 39712501994; sweetgl from the Pixel port) (r49-gallium-live-19/20.txt)
Switched to the Pixel 1 port's tools/sweetgl (EAGL shim, GLI dispatch, IOSurface
handling), built inside our Mesa tree as /usr/local/lib/sweetgl.dylib and
injected with riverenv. River glue river_drm.c replaces its msm emulation;
kernel 0183 adds RiverGPU BO_IMPORT (IOSurface pages as a GPU buffer) and
BO_INFO. r52: sweetgl installed and created screen FD506 in backboardd, then
SIGSEGV at pc 0 in st_api_create_context (Mesa 26 calls the frontend
get_param; sweetgl left it NULL), 9 backboardd restarts. r52b with a
get_param callback: **no crash; CoreAnimation's EAGLContext API 3 runs on
Mesa** (`EAGLContext ... API 3 on Mesa (GLI ...)`), GL flushes/finishes reach
the GPU (finish 14-44 ms), no GPU faults or error IRQs. QuartzCore texture-
function fixes refused (code did not match their expectation). Four small
texImageIOSurface textures (A008 438x79, LA08 23x23, RGhA 256x1 x2) failed
kernel import (reason not logged). r52c: those fall back to a CPU copy, and
import failures log their reason. Screen output of r52b not yet reported.
**r52b screen (user report, private photos IMG_0384/0385): GPU-rendered UI.**
Home screen and Settings draw through Mesa and run; transparency and blur
are fully correct with no glitches. Wrong: some text labels are solid boxes
in the text colour (home "Photos" label, search pill, several Settings row
titles, the account name in blue), Settings row icons are black squares, two
widgets show white "Waiting..." tiles. Hypothesis (not verified): the boxes
are mask/atlas textures whose IOSurface import failed (A008 438x79 looks like
a glyph atlas, LA08 23x23 a template icon), so CA sampled unbacked textures;
r52c copies those instead. The refused QuartzCore fixes are colour-matrix
and blur functions, not masks.

### r52c (copy fallback for failed imports) (r49-gallium-live-21.txt): **labels and icons correct**
User: with Settings open, the text labels and row icons now draw correctly
(the r52b boxes are gone). So the boxes were textures whose IOSurface import
failed. The log names the reasons for about 25 surfaces (A008 glyph masks up to 640x358,
LA08 icons, RGhA ramps, BGRA 87x87): "no GPU mapping" (pmap_find_phys on the
surface's kernel address found no page: memory not wired) and "no surface or
no kernel mapping" (kernelAddress() 0). No crashes, no GPU faults; up to 4.2
flushes/s while interacting, finish 10-35 ms. Next (0184): wire the surface
memory (prepare) and import by physical segments, like the Pixel kernel,
cache maintenance by physical address, user maps through the surface memory.

### r53 (0184; CI 39715545829) (r49-gallium-live-22.txt): IOSurface imports work
No BO_IMPORT failures. Of all texImageIOSurface calls logged in the first
~2 minutes, one fell back to the CPU copy: LA08 23x23 pitch 64 "mesa layout
failed" (kernel import succeeded; Mesa's linear layout for R8G8 at that
width does not accept pitch 64). No backboardd crashes, no GPU error IRQs
(the CB0 FSR 0x202 line is the boot-time unmapped-write self-test). Image
`shim-river/out/river-r53-sweetgl-boot.img`, SHA256
603721b1c78022dc47bf789e83b5022a011e2a181c079e914547ee5d41658936.
User check on r53: screen looks right (labels, icons, blur, transparency).
Known gaps: QuartzCore texture-function fixes refused (address check fails
on this cache), LA08 small-stride layout, ~0.1-4 flushes/s and 10-45 ms GPU
finishes (no FPS measurement yet), GPU clock still conservative.

### r53 frame timing (r49-gallium-live-22.txt): **CPU-bound, not GPU-bound**
sweetgl logs per 10 s window: flushes/s (CA renders one flush per frame;
sweetgl turns each flush into glFinish), time blocked in glFinish (GPU), CPU
time between frames. The user scrolled Settings continuously for ~30 s:

| window | frames/s | GPU wait avg (max) | CPU between frames |
| --- | --- | --- | --- |
| scroll | 4.4 | 17.4 ms (23.8) | 208 ms |
| scroll | 4.3 | 17.1 ms (22.5) | 215 ms |
| scroll | 4.3 | 16.6 ms (21.6) | 218 ms |
| scroll | 5.7 | 11.1 ms (21.4) | 164 ms |
| scroll | 4.0 | 17.4 ms (22.5) | 235 ms |
| scroll | 4.3 | 18.1 ms (25.4) | 213 ms |

GPU (at 133 MHz) is ~8% of a frame; ~92% is CPU work in backboardd between
frames (CoreAnimation + Mesa/freedreno CPU + sweetgl). Even a free GPU gives
~5-6 fps, so raising the GPU clock is not worth it yet. Kernel panel copy
(`RIVER FB`): ~21 ms per present, ~1700 rows, at sweetfb_hz=30 (CPU).
0185 (rivergpuclk=133|200|266|320, default 133; CI 39717137089, kernel in
k-r54) is built but NOT booted. Next: profile backboardd's CPU time per frame
(split CA vs Mesa vs sweetgl); also CPU clock is still capped at 1.4 GHz.

### r54p CPU profile of backboardd's render thread (r49-gallium-live-23.txt)
sweetgl now times every GLI call, IOSurface binds, render-thread and process
CPU. Continuous Settings scroll, busiest windows (~4-5 frames/s), per frame:
wall ~195-255 ms = GPU wait ~15-20 ms + ~180-235 ms between; render-thread
CPU ~160-235 ms = **inside GL calls ~140-215 ms** + CoreAnimation ~15-20 ms +
sweetgl IOSurface binds ~1 ms; ~330-580 GL calls per frame; top call by far
**glDrawElements, 120-210 ms per frame** (then glUseProgram, glClear,
glLinkProgram in some windows). So CoreAnimation and the GPU are cheap; the
cost is CPU per draw (~0.5 ms+ per draw). Two windows have bogus thread-CPU
figures (the render thread changed between reports). glDrawElements is a
sweetgl hand-written slot (draw_prep: texture-unit shadowing, copy_refresh
re-uploads of copied IOSurfaces, fbfetch emulation) before Mesa's draw, so
r54q splits sweetgl prep / texture re-copies / Mesa draw.

### r54q draw split (r49-gallium-live-24.txt)
Continuous scroll, busiest window 4.9 frames/s, per frame: sweetgl
draw_prep ~94 ms (texture re-copies 0), Mesa draw calls ~82 ms,
CoreAnimation ~21 ms, GPU wait ~19 ms; backboardd 115% of a core. Other
windows ~94-101 ms prep / ~52-87 ms Mesa draw. Suspect: draw_prep calls
_mesa_TextureBarrierNV before every fbfetch draw (CA blend modes /
vibrancy) and every draw that samples its own render target, and
freedreno's fd_texture_barrier is a full fd_context_flush, i.e. a GPU
submit (kernel cleans every listed buffer's cache, including ~10 MB
screen surfaces). r54r counts and times barriers and River submits
(count, kernel SUBMIT time, buffers per submit) per frame.

### r54r barriers and submits (r49-gallium-live-25.txt): **kernel SUBMIT ~10 ms each**
Continuous scroll, per frame (~2-4 frames/s): 4-9 texture barriers (each a
full fd_context_flush) costing ~70-180 ms; 13-31 GPU submits; ~130-325 ms
inside the kernel SUBMIT call, i.e. ~10 ms per submit (the r49t round trip
was 0.12 ms), 15-19 buffers per submit. Cause: river_gpu_submit cleans and
invalidates every listed buffer whole, including ~10 MB screen IOSurfaces,
on every submit. That dominates the frame (and shows up in both sweetgl's
draw_prep, via barriers, and Mesa's draws, via flushes).
0186: Mesa's own buffers are allocated with BO_NEW flag 1 and mapped
write-combined (msm's default): no cache maintenance; the kernel alias is
invalidated once at free. Imported IOSurfaces are cleaned only when their
seed (bumped on every writable unlock) changed since the last clean, and are
marked gpuTouched; the panel copy invalidates a gpuTouched source before
reading it. rivergpuflush=1 restores clean-everything-per-submit.

### r55 (0186; CI 39722830907) (r49-gallium-live-26.txt): **~3x faster, but flickers**
User: "definitely much smoother", but the screen flickers heavily.
Continuous scroll, per frame: **7-16 frames/s** (r54r 2-4); kernel SUBMIT
**~1.5-2.7 ms per frame** (~0.1 ms per submit; r54r 130-325 ms); texture
barriers ~2-3 ms (r54r 70-180 ms); render-thread CPU ~17-26 ms (Mesa ~15-20,
CA ~4-9); GPU wait ~12-24 ms; the rest (~40-110 ms) the render thread is not
running (waiting), now the largest share. Panel copy ~19-23 ms per present.
Flicker hypothesis: the panel copy invalidated a gpuTouched surface once and
cleared the mark while the GPU could still be drawing the next frame into it,
so later copies read stale cached lines; also writers that skip the seed.
0187: the invalidate before the panel copy is sticky (any surface the GPU
ever used), and imports <= 1 MiB are always cleaned before GPU use.

### r56 (0187; CI 39724110440) (r49-gallium-live-27.txt): fast, still flickers
User: still flickers. Speed kept: 14-27 frames/s while scrolling, kernel
SUBMIT ~0.6-2.6 ms per frame (~0.1 ms each), 2-10 barriers ~0.6-2.6 ms.
So the once-per-mark invalidate race was not (only) the cause. Next: boot
`river-r56-flushall-boot.img` (same kernel, rivergpuflush=1) to tell a
cache problem (e.g. app backing stores written without a seed bump) from a
timing problem (GPU vs the 30 Hz panel copy).

### r56 with rivergpuflush=1 (r49-gallium-live-28.txt): no flicker, slower
User: no flicker when scrolling Settings, fewer frames. So the r55/r56
flicker is CPU cache coherency from 0186's reduced cleaning. Leading
suspect: Mesa's own buffers. 0186 assumes their user mapping is really
write-combined, but they are wired kernel_map memory the kernel maps cached,
and XNU may keep a cached user mapping despite kIOMapWriteCombineCache; then
command streams/uploads stay dirty in the CPU cache and the GPU reads stale
data. r57 (`river-r57-cachedbo-boot.img`, r56 kernel): Mesa buffers mapped
cached without BO_NEW flag 1 (kernel cleans them per submit), imports keep
seed-based cleaning (the expensive large render targets stay uncleaned).

### r57 (r56 kernel, Mesa buffers cached + kernel-cleaned) (r49-gallium-live-29.txt)
User: only a little flicker left. 12-15 frames/s while scrolling; kernel
SUBMIT ~24-40 ms per frame (~1.5 ms per submit, cleaning Mesa's buffers).
So most of the r55/r56 flicker was Mesa's own buffers (the "write-combined"
user mapping did not stop CPU caching, apparently). Remaining flicker: large
imported surfaces written by the CPU without a seed change. 0188: large
imports are also cleaned when their last clean is older than 16 ms (about
once per displayed frame), and the kernel logs every 10 s how many import
cleans were small / by seed / by age / skipped.

### r58 (0188) (r49-gallium-live-30.txt): seeds never move
Still a little flicker; 6-8 frames/s; kernel SUBMIT 43-108 ms per frame. The
import counters were `0 by seed` in every 10 s window (hundreds by age): apps
write IOSurfaces without the kernel unlock trap, so seed tracking is useless
here. 0189: sweetgl marks every surface it binds as a render target
(attachImage) GPU-only via new RiverGPU selector 9 BO_SET_FLAGS; the kernel
never cleans GPU-only imports (still gpuTouched for the panel copy) and
cleans every other import before each GPU use.

### r59 (0189) (r49-gallium-live-31.txt): still flickers
12-16 frames/s; kernel SUBMIT ~12-24 ms per frame. Counters: every large
import is a render target (~1000-1600 render-target uses skipped per 10 s,
~0 large non-RT cleans); small imports cleaned. Flicker unchanged, and
flush-all (which also cleaned render targets) had none. Hypothesis: render
targets carry dirty CPU cache lines from their creation (the CPU zero fill)
or earlier CPU drawing; never cleaned, they are later written back over GPU
pixels (on eviction or by the panel copy's clean+invalidate). 0190: every
import is cleaned once when imported, and again when first marked GPU-only.

### r60 (0190) (r49-gallium-live-32.txt): still flickers
Cleaning imports once at import did not help, so dirty lines from surface
creation are not the cause. Pattern so far: flicker grows with frame rate
(flush-all slow: none; r58 6-8 fps: a little; r57 12-15: a little; r59/r60
12-16: yes; r55/r56 15-27: heavy), so a timing race is as likely as cache.
0191 (fix attempt + measurement): the kernel stores each submit's fence on
the imported surfaces it used; before the panel copy reads a GPU-drawn
surface it waits for that fence (50 ms max). Every 10 s it logs copies, how
many found the GPU still busy (timeouts, time waited), and how many saw the
invalidate change sampled data (stale cache lines).

### r61 (0191) (r49-gallium-live-33.txt): panel copy is not the cause
Still flickers. Panel-copy diagnostics every 10 s: `0 found GPU busy`,
`0 saw stale cache` (e.g. 188 GPU-surface copies). So the copy shows what
the GPU rendered; the flicker is in the rendered frames: the GPU reads
something not written back from the CPU cache. Only per-submit cleaning of
render targets (flush-all) has removed it, which suggests CPU writes to
surfaces CA also attaches. (The stale-line check cannot see dirty lines.)
Flush-all ran at only 3-6 frames/s (live-28) because it cleans every buffer
on each of 13-31 submits per frame. The Pixel port also cleans every listed
buffer on every submit (its 0151) and still reaches 15-35 fps.
New approach (r62): clean everything per submit (r61 kernel with
rivergpuflush=1: correct by construction) and cut the submits: Mesa a5xx
texture barrier with forced sysmem now emits PC_CCU_FLUSH_COLOR/DEPTH_TS +
UCHE invalidate + WFI in the current batch instead of a whole context flush.

### r62 (r61 kernel + rivergpuflush=1, in-batch a5xx barrier) (r49-gallium-live-34.txt)
User: **no flicker; blur and transparency right.** So the in-batch texture
barrier (PC_CCU_FLUSH_COLOR/DEPTH_TS + UCHE invalidate + WFI) renders
correctly, and barriers now cost ~15-60 us per frame instead of flushes.
But speed stays at 4-6 frames/s: still 6-18 GPU submits per frame (so most
submits were never barriers: CA flushes/fences, and freedreno flushing a
batch when a later draw samples what it rendered), each ~11 ms in the
kernel cleaning ~20 listed buffers (65-195 ms per frame).
State: correct baseline = r62. Fast but flickering = r56 (14-27 fps).
Open: which buffers really need per-submit cleaning (Mesa's own buffers are
cached because the write-combined user mapping did not take; render
targets need cleaning for a reason not yet found), or making them truly
uncached (e.g. IOBufferMemoryDescriptor with a write-combined cache mode,
for both the kernel and the user mapping).
