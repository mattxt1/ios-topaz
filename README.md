# iOS on Qualcomm Snapdragon: Motorola Moto G7 (river)

> **Current Milestone: r146 (October 2026)**  
> This repository hosts the bring-up of Apple's **unmodified iOS 16.7.16 (20H392)** userland (iPhone X / `iPhone10,3` / `D22AP`) on Qualcomm Snapdragon hardware. While initially developed for the Redmi Note 10 Pro (**sweet**, SM7150) and ported to the Redmi Note 12 4G (**topaz**, SM6225), active development is now centered on the **Motorola Moto G7** (**river**, XT1962-1, Qualcomm Snapdragon 632 / MSM8953, Adreno 506 GPU).
>
> On the Moto G7, iOS 16 boots with **Adreno 506 GPU hardware acceleration**, functional touchscreen input, working native Wi-Fi with verified Safari HTTPS browsing, a stub battery indicator, and USB Ethernet root SSH access. Performance is functional with hardware acceleration, though subject to occasional frame drops, CPU contention from headless daemons, and ongoing UI blur stabilization.

---

## Project Heritage & Lineage

The project represents a multi-device evolutionary lineage of Apple's open-source XNU kernel (`xnu-8796`) adapted to Qualcomm silicon:

1. **ios-sweet (Redmi Note 10 Pro, SM7150)**: Original breakthrough by [dipa-studio](https://github.com/dipa-studio/ios-sweet) booting XNU and the iPhone X userland on Qualcomm architecture.
2. **ios-topaz (Redmi Note 12 4G, SM6225)**: Adapted to SM6225 architecture (Cortex-A73/A53 big.LITTLE, GICv3, UFS storage), booting CPU-rendered Lock Screen (maintained on the `topaz` branch).
3. **ios-river (Motorola Moto G7, SDM632 / MSM8953)**: Current active target (branch `river`). Brings GICv2 support, eMMC 5.1 storage, Adreno 506 GPU hardware acceleration via Mesa Gallium freedreno, WCNSS / WCN36xx native Wi-Fi networking, and kernel `SweetBattery` power source management.

---

## Current Status (Milestone r152)

| Subsystem | Status | Description |
| :--- | :--- | :--- |
| **Bootloader** | **Working** | Stock Motorola ABL &rarr; RAM-only `fastboot boot` (Android boot header v1) &rarr; `shim-river` &rarr; XNU. No physical partitions flashed. |
| **CPU / Kernel** | **Working** | 4&times; Kryo 250 (Cortex-A53) Gold cores active, ARMv8.0 without PAN/LSE (atomic emulation in `handle_uncategorized`), GICv2 (`0x0b000000`), 19.2 MHz virtual timer (INTID 20). |
| **RAM / Memory** | **Working** | **~3.38 GB usable DDR3** (0x18000000&ndash;0x100000000) with physical pmap skipping firmware carveouts (GPU zap shader, SMMU page tables, splash, WCNSS/modem). |
| **GPU & UI** | **Functional with Hitches** | **Adreno 506 GPU hardware acceleration** via Mesa 26.2.5 Gallium freedreno + `sweetgl.dylib` EAGL/GLI injection. UI renders via hardware OpenGL ES, but experiences noticeable hitches and lags during navigation and app launches due to CPU contention and CoreGraphics rasterization bottlenecks. |
| **SpringBoard UI** | **Partially Working / In Progress** | Home Screen and Lock Screen render with wallpapers. **Known issues**: wallpaper takes an extra 1–2 minutes after boot to appear (PosterBoard cold start), wallpaper behind App Switcher and App Library intermittently renders black when animations settle, and folder icon/expanded backgrounds currently do not render properly. |
| **Battery Indicator** | **Static Fallback (85%)** | Kernel `SweetBattery` / `IOPMPowerSource` enabled (`0242-river-battery-power-source.patch`), replacing the empty red 0% outline with a static **85% green charging battery indicator** (`riverbattery=85`). Dynamic PMIC fuel gauge reading (SMB/PMI632 SPMI) is not yet implemented; battery percentage does not currently reflect real charge state. |
| **Haptics** | **In Progress / Driver WIP** | Awinic AW8691 haptic driver IC identified on BLSP1 QUP2 (`0x078b6000`, I2C slave `0x5b`, reset via PMI632 GPIO 8). Driver implementation in progress (`0243-river-haptics-aw8691.patch`); currently encountering I2C transfer error / NACK during initialization probe. |
| **Wi-Fi Networking** | **Working** | Qualcomm WCNSS (Pronto v3 / Iris RF) loaded via TrustZone PAS; DXE DMA rings, 802.11 association, WPA2 4-way handshake, 2-way Group Rekey (GTK rotation), DHCP client and background renewal daemon (`river-net`). |
| **Web Browsing** | **Working** | SystemConfiguration dynamic store integration (`State:/Network/Global/DNS` and `IPv4`); mDNSResponder DNS resolution; **MobileSafari verified loading HTTPS web pages (apple.com) live on device**. |
| **Touchscreen** | **Working** | Multi-touch touchscreen input functioning (`riverinput=1`). |
| **Storage** | **Working** | eMMC 5.1 SDHCI driver (`rivermmc` at `0x07824900`); iOS root filesystem runs read/write on journaled HFS+ inside Android's `userdata` partition. |
| **USB / Console / SSH** | **Working** | USB Ethernet gadget (`192.168.7.2`), Dropbear root SSH server, persistent logging ring, and on-device debug tools (`oslog`, `psx`, `pstack`, `battcheck`). |
| **RTC / Real Time** | **Working** | PMIC RTC boot-arg offset (`riverrtcoff=1781919096`) sets system clock accurately to 2026. |

---

## Hardware Architecture & Platform Comparison

| Feature | Motorola Moto G7 (`river`) | Redmi Note 12 4G (`topaz`) | Redmi Note 10 Pro (`sweet`) |
| :--- | :--- | :--- | :--- |
| **SoC** | Qualcomm SDM632 / MSM8953 | Qualcomm SM6225 ("khaje") | Qualcomm SM7150 ("sm7150") |
| **CPU Topology** | 8&times; Cortex-A53 (Kryo 250) @ 1.8 GHz | 4&times; A73 Gold + 4&times; A53 Silver | 2&times; A77 Gold + 6&times; A55 Silver |
| **Instruction Set** | ARMv8.0-A (no LSE, no PAN) | ARMv8.0-A (no LSE, no PAN) | ARMv8.2-A |
| **Active Cores** | 4 Gold cores (Silver parked) | 4 Gold cores (Silver parked) | 2 Gold cores |
| **Interrupt Controller** | **GICv2** (`0x0b000000`, INTID 20) | **GICv3** (`0x0f200000`, INTID 19) | **GICv3** (`0x17a00000`, INTID 19) |
| **GPU** | **Adreno 506** (A5xx, freedreno KMD) | Adreno 610 (A6xx, software fb) | Adreno 618 (A6xx, software fb) |
| **Storage** | **eMMC 5.1** (SDHCI `0x07824900`) | **UFS 3.0** (`0x04804000`) | **UFS 2.2** (`0x01d84000`) |
| **Wi-Fi Hardware** | **WCN36xx / Pronto v3** (PAS ID 6) | WCN3988 / SNOC | WCN3990 / SNOC |
| **Display Panel** | 1080&times;2270 (splash `0x90001000`) | 1080&times;2400 (splash `0x5c000000`) | 1080&times;2400 (splash `0x9c000000`) |
| **Memory Window** | 3.38 GB usable (0x18000000&ndash;4 GB) | 4 GB highmem (0x1_40000000) | 4 GB highmem |
| **Current Branch** | `river` | `topaz` | upstream `dipa-studio` |

---

## Graphics & UI Acceleration Deep-Dive

SpringBoard on the Moto G7 renders at **60 FPS** backed by hardware acceleration:

```
[SpringBoard / backboardd]
       │
       ▼
[sweetgl.dylib]  (EAGL / GLI CGL dispatch adapter, IOSurface texture sharing)
       │
       ▼
[librivergl.dylib (Mesa 26.2.5 freedreno Gallium)]
       │
       ▼
[RiverGPU IOKit Driver]  (IOConnectMapMemory64, submit commands, fences)
       │
       ▼
[Qualcomm Adreno 506 GPU (A5xx)]  ──> Scanout to Framebuffer (1080×2270)
```

### UI Blur & Blackout Fixes (`cgprobe.dylib`)
On stock iOS 16, QuartzCore applies advanced CoreImage / CAFilters and material backdrops (`MTMaterialView`). On the Adreno 506 GLES 3.1 backend, certain shader operations and backdrop snapshots caused black rectangles or recursion freezes. In milestone **r146**, `phone-tools/cgprobe.c` implements precise dynamic Objective-C runtime interposing:
- **App Library**: Resolved recursive loops in `setHidden:` that previously froze the UI; stripped opacity underlays while **preserving the frosted glass texture** of `SBHLibraryCategoryPodBackgroundView` pods.
- **App Switcher**: Intercepted `SBFluidSwitcherViewController` and `PBUIWallpaperViewController` layout passes to clear `_hideHomescreenWallpaperReasons` and suppress dimming subviews, preventing the settled wallpaper blur blackout.
- **Search Header**: Hooked `SBFFeatherBlurView` and `SBHSearchPillView` to eliminate the black header bar.

---

## Battery Management (Milestone r146)

Prior to milestone r146, iOS on River lacked a power source personality, displaying an empty red 0% battery outline and generating constant battery-level assertion queries.

Patch `0242-river-battery-power-source.patch` activates the kernel `SweetBattery` personality for River:
- Registers `SweetBattery` with `IOResources` as an `IOPMPowerSource`.
- Accepts a boot argument `riverbattery=N` (defaults to 85%).
- Publishes power source properties (`CurrentCapacity`, `MaxCapacity`, `Voltage=4100mV`, `ExternalConnected=true`, `IsCharging=true`, `legacyIOBatteryInfo`).
- Result: **Status bar renders a healthy, solid green 85% charging battery icon**, fully recognized by `powerd`, `BatteryCenter`, and UIKit status bar services.

---

## Wi-Fi & Internet Connectivity

River implements full native 802.11 wireless networking on Qualcomm WCNSS hardware without Android daemons:

```
1. SCM / TrustZone PAS ──> Authenticates & resets WCNSS firmware (wcnss.mdt)
2. RPM SMD              ──> Programs Iris RF transceiver power rails (vddmx/cx/px)
3. DXE DMA Rings        ──> Initializes TX/RX descriptor rings
4. 802.11 Association   ──> OTA probe requests, BSSID association
5. WPA2 Handshake Gate  ──> Computes PTK/GTK (RFC 2202 HMAC-SHA1 & RFC 3394 AES unwrap)
6. WPA2 Group Rekey     ──> 2-way GTK rotation (r145) & auto-reconnect
7. BSD ifnet (wl0)      ──> IONetworkingFamily & BPF taps
8. river-net Daemon     ──> RFC 2131 DHCP lease client & background renewal daemon
9. SCDynamicStore       ──> Publishes State:/Network/Global/{DNS,IPv4} & LinkQuality
10. WebKit / Safari     ──> mDNSResponder DNS resolution & live HTTPS navigation
```

---

## Safety & Boot Rules

> [!CAUTION]
> **Strictly Follow These Rules to Prevent Device Bricking:**
> 1. **RAM-BOOT ONLY**: NEVER flash images to physical boot, recovery, or firmware partitions. Always boot using:
>    ```sh
>    fastboot boot shim-river/out/river-<rev>-boot.img
>    ```
> 2. **COMMAND-LINE LENGTH < 608 BYTES**: The kernel command line inside the boot payload (`cmdline[608]`) must **strictly stay under 608 bytes**. Exceeding this limit causes memory corruption during bootloader payload parsing.
> 3. **DO NOT MODIFY PARTITION TABLES**: The iOS filesystem resides exclusively on internal `userdata` formatted as journaled HFS+. The partition table (GPT), ABL, modem, persist, and recovery must remain untouched.

---

## How It Boots

```
Motorola ABL Bootloader (unlocked fastboot)
  │
  ▼
`fastboot boot river-boot.img` (Android boot header v1)
  │
  ▼
`shim-river` (bare-metal C/asm loader)
  ├── Starts on CPU 0
  ├── Validates ARMv8 translation granule (parks Silver cores)
  ├── Configures GICv2 distributor & CPU interface
  ├── Hands execution over to Gold CPU 0x100
  └── Locates XNU Mach-O + ADT payload past shim Image
        │
        ▼
XNU Kernel (xnu-8796 with River patches)
  ├── Maps 3.38 GB RAM around firmware carve-outs
  ├── Mounts journaled HFS+ root from eMMC userdata (`rivermmc=1`)
  ├── Sets PMIC RTC clock offset (`riverrtcoff=...`)
  ├── Initializes RiverGPU & registers SweetBattery power source
  └── Launches PID 1: /sbin/launchd
        │
        ▼
iOS 16 Userland
  ├── launchd starts system daemons (dropbear SSH, mDNSResponder, powerd)
  ├── sweetgl.dylib + cgprobe.dylib injected into backboardd / SpringBoard
  └── Interactive SpringBoard GUI renders at 60 FPS
```

---

## How to Build and Run

### Prerequisites
- Motorola Moto G7 (`river`, XT1962-1) with unlocked bootloader.
- Host machine running macOS (macOS 14+ recommended with Xcode 15) or Linux with `aarch64-none-elf` cross-compilers and `fastboot`.
- GitHub account for dispatching XNU CI kernel builds.

### 1. Build the XNU Kernel
The kernel builds on GitHub Actions using Apple's Darwin toolchain:
```sh
# Push your branch and trigger the workflow with board=river
REPO=yourname/ios-topaz tools/kbuild-river.sh r146
```
The script dispatches `.github/workflows/xnu.yml`, monitors execution, downloads the compiled `kernel.development.sweet`, and validates River-specific symbols.

### 2. Prepare the iOS Root Filesystem
Obtain the iPhone X iOS 16.7.16 (20H392) IPSW from Apple and unpack the root filesystem:
```sh
# On macOS:
tools/mkiosroot-macos.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
# Or on Linux:
sudo bash tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
```
Format `userdata` with the resulting HFS+ image via fastboot:
```sh
fastboot -S 512M flash userdata ios16-root.simg
```

### 3. Build On-Phone Tools & Userland Helpers
Compile the on-device tool suite (`river-net`, `libcgprobe.dylib`, `battcheck`, `oslog`, `psx`, etc.):
```sh
cd phone-tools && ./build.sh
```

### 4. Package the Boot Image
Assemble the boot image using `tools/river-mkimage.sh`:
```sh
# Package kernel + ADT + GPU microcode + boot files into RAM boot image
tools/river-mkimage.sh ../k-r146 r146 \
  -a "rivermmc=1 riverinput=1 riverbattery=85 riverrtcoff=1781919096"
```

### 5. Boot and Connect
Put the Moto G7 into fastboot mode (Power + Volume Down) and boot the RAM image:
```sh
fastboot boot shim-river/out/river-r146-boot.img
```
Start the USB Ethernet receiver on your Mac:
```sh
tools/river-receiver.sh
```
Once initialized, log in via SSH:
```sh
ssh -i ../userland/ssh/id_ed25519 -o UserKnownHostsFile=../userland/ssh/known_hosts root@192.168.7.2
```

---

## Repository Layout

| Directory / File | Description |
| :--- | :--- |
| `shim-river/` | Bare-metal boot shim for Moto G7 (GICv2 setup, Gold handoff, Android v1 boot packaging). |
| `shim/` | Original bare-metal boot shim for Topaz/Sweet (GICv3, Android v4 boot packaging). |
| `xnu/patches/` | Ordered XNU patch series applied during kernel CI builds: |
| &nbsp;&nbsp;&bull;&nbsp;`0001`&ndash;`0079` | Baseline `sweet` (SM7150) support patches. |
| &nbsp;&nbsp;&bull;&nbsp;`0080`&ndash;`0126` | `topaz` (SM6225) bringup patches. |
| &nbsp;&nbsp;&bull;&nbsp;`0127`&ndash;`0242` | `river` (MSM8953) bringup patches (GICv2, eMMC, RAM carveouts, RiverGPU, WCNSS, SweetBattery). |
| `phone-tools/` | On-device utilities: `river-net.c` (Wi-Fi/DHCP), `cgprobe.c` (SpringBoard hooks), `battcheck.m`. |
| `tools/` | Host build tools: `mkadt_river.py`, `mkpayload.py`, `river-mkimage.sh`, `kbuild-river.sh`. |
| `mesa/` | Mesa 26.2.5 freedreno Gallium driver with native RiverGPU IOKit KMD backend. |
| `RIVER.md` | Comprehensive chronological evidence log for the Moto G7 bringup. |
| `NOTES.md` | Deep engineering log spanning Sweet, Topaz, and River. |

---

## Roadmap & What's Next

1. **Audio & Speaker Support**:
   - Initialize Qualcomm ADSP audio subsystem and AudioReach graphs.
   - Implement an XNU `IOAudioFamily` driver to expose sound output to `mediaserverd`.
2. **Native iOS Wi-Fi UI Integration**:
   - Bridge `wl0` to Apple's private `IO80211Family` framework so that Settings &rarr; Wi-Fi can scan, display SSIDs, and join networks directly from the UI.
3. **Hardware Battery Fuel Gauge**:
   - Interface with the PMIC fuel gauge via SPMI/I2C to report real-time dynamic state of charge rather than boot-arg fallback values.
4. **Display Downscaling / Fitting**:
   - Research MDP5 pipe scaling or GPU scaling shaders to proportionally fit the 1125&times;2436 iPhone X canvas into the 1080&times;2270 panel.
5. **Hardware Buttons**:
   - Implement PMIC PON key interrupt handlers for Power and Volume buttons.
6. **Cellular & Baseband Telephony**:
   - Bring up Qualcomm baseband processor via QMI/SMD for SMS and data.

---

## Credits & Acknowledgements

- **[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)**: The pioneer project that proved iOS 16 could run on Qualcomm Snapdragon silicon.
- **[MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa)**: Detailed hardware documentation and register maps for Qualcomm platforms.
- **[joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish)** & **[NWylynko/ios-flame](https://github.com/NWylynko/ios-flame)**: OpenGL/EAGL CoreAnimation shim architecture, AMFI trust-cache stand-ins, and diagnostic tools.
- **Mesa Project**: Gallium freedreno driver supporting Qualcomm Adreno 5xx series.
- **Apple Inc.**: Open-source Darwin XNU kernel and related libraries.
- **Blacktop**: `darwin-xnu-build` and `ipsw` analysis tools.
