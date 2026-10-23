#!/usr/bin/env python3
"""Early-boot Apple Device Tree for Moto G7 river (SDM632).

Derived from mkadt.py's topaz tree. Hardware addresses are from the live
river FDT captured on 2026-10-19, with the river GICv2 and eMMC patches.
For iOS userland, place the Apple D22AP tree at research/ipsw-x/d22ap.adt
so mkadt.py can copy its /product identity properties.
"""

import argparse
import struct

import adt
import mkadt


# A single contiguous window with no fixed reserved-memory node in the live
# FDT: DRAM starts at 0x10000000; ABL's boot buffers (shim, DTB, ramdisk
# payload) end below 0x16000000; the first fixed carve-out is River's own GPU
# zap shader at 0x81800000 (then SMMU ptbl 0x82000000, firmware from
# 0x84300000). 1.65 GB; the old window 0xa0000000-0xee000000 was 1.25 GB.
PHYS_BASE = 0x18000000
MEM_SIZE = 0x69800000
# --full-ram: everything up to the end of DRAM at 4 GiB (3.6 GB range,
# ~3.38 GB usable). Only for kernels with 0205, which skips the firmware
# holes (osfmk/arm64/river_phys.h); an older kernel would hand them out.
MEM_SIZE_FULL = 0x100000000 - PHYS_BASE
GICD = 0x0B000000
GICC = 0x0B002000
FB = 0x90001000
FB_SIZE = 0x013FF000
FB_STRIDE = 1080 * 3
FB_BPP = 3
TIMER_HZ = 19_200_000
# live FDT sdhci@7824900 (sdhc_1, eMMC): hc_mem, core_mem; GCC page with the
# SDCC1 apps/ahb CBCRs at 0x1842018/0x184201c (gcc-msm8953.c).
EMMC_HC = (0x07824900, 0x500)
EMMC_CORE = (0x07824000, 0x800)
GCC_SDCC1 = (0x01842000, 0x1000)
VIRT_TIMER_INTID = 20  # FDT /soc/timer third interrupt: PPI 4 + 16


def child(node, name):
    matches = [item for item in node.children if item.name == name]
    assert len(matches) == 1, (node.name, name, len(matches))
    return matches[0]


def set_prop(node, name, value):
    matches = [i for i, (key, _, _) in enumerate(node.props) if key == name]
    assert len(matches) == 1, (node.name, name, len(matches))
    node.props[matches[0]] = (name, 0, value)


def make_tree():
    mkadt.PHYS_BASE = PHYS_BASE
    mkadt.MEM_SIZE = MEM_SIZE
    mkadt.GICD = GICD
    mkadt.GICR = GICC  # topaz builder's second GIC register range
    mkadt.GICD_SIZE = 0x1000
    mkadt.GICR_SIZE = 0x1000
    mkadt.FB = FB
    mkadt.FB_SIZE = FB_SIZE
    mkadt.FB_W = 1080
    mkadt.FB_H = 2270
    # ABL's splash scanout is 24bpp RGB with 1080 * 3 byte rows (inferred from
    # the diagnostic photos), not Android fb0's 32bpp/4352 layout. The SWEET
    # framebuffer console reads bytes-per-pixel from this node (patch 0127).
    mkadt.FB_STRIDE = FB_STRIDE
    mkadt.TIMEBASE = TIMER_HZ
    mkadt.VTIMER_INTID = VIRT_TIMER_INTID
    mkadt.SWEET_ECID = 0x5249564552000001  # synthetic 'RIVER' + 1

    root = mkadt.build(native=True)
    set_prop(root, 'compatible', mkadt.cstr('D22AP') + mkadt.cstr('iPhone10,3') +
             mkadt.cstr('motorola,river') + mkadt.cstr('qcom,sdm632') +
             mkadt.cstr('xiaomi,sweet') + mkadt.cstr('AppleARM'))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'firmware-version', mkadt.cstr('river-shim'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('river-shim'))
    # MobileGestalt's WiFiAddress first reads this CFData from /chosen.
    # Without it, UniqueDeviceID (IconServices startup) falls through to an
    # IONetworkController notification wait, although river has no Wi-Fi
    # driver yet. Stable locally administered identity, not the phone's MAC;
    # this publishes no network controller and enables no radio hardware.
    chosen.props.append(('mac-address-wifi0', 0, bytes.fromhex('025249564501')))

    cpus = child(root, 'cpus')
    for cpu in cpus.children:
        set_prop(cpu, 'clock-frequency', mkadt.u32(1_800_000_000))
        assert struct.unpack('<I', cpu.prop('reg'))[0] in range(0x100, 0x104)

    arm_io = child(root, 'arm-io')
    gic = child(arm_io, 'gic')
    set_prop(gic, 'compatible', mkadt.cstr('arm,gic-v2'))
    set_prop(gic, 'virtual-timer-intid', mkadt.u32(VIRT_TIMER_INTID))
    set_prop(gic, 'reg', mkadt.u64(GICD - mkadt.SOC_BASE, 0x1000,
                                  GICC - mkadt.SOC_BASE, 0x1000))
    disp = child(arm_io, 'disp0')
    # iOS has layout data for the iPhone X canvas. AppleCLCD presents that
    # mode and copies its central 1080x2270 window to ABL's 24bpp splash.
    set_prop(disp, 'canvas-width', mkadt.u32(1125))
    set_prop(disp, 'canvas-height', mkadt.u32(2436))

    fbcon = child(root, 'sweet-fbcon')
    fbcon.props.append(('bytes-per-pixel', 0, mkadt.u32(FB_BPP)))

    root.children = [item for item in root.children if item.name != 'sweet-ufs']
    # bsd/dev/river_emmc.c (patch 0128): read-only probe, only with rivermmc=1.
    root.children.append(mkadt.node('river-emmc', [
        ('compatible', mkadt.cstr('qcom,sdhci-msm')),
        ('reg', mkadt.u64(*EMMC_HC, *EMMC_CORE, *GCC_SDCC1)),
    ]))
    assert all(item.name != 'sweet-ufs' for item in root.children)
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output')
    parser.add_argument('--dump', action='store_true')
    parser.add_argument('--full-ram', action='store_true',
                        help='RAM to 4 GiB around the firmware holes (kernel 0205+)')
    args = parser.parse_args()
    global MEM_SIZE
    if args.full_ram:
        MEM_SIZE = MEM_SIZE_FULL
    blob = adt.serialize(make_tree())
    parsed, end = adt.parse(blob)
    assert end == len(blob)
    assert child(parsed, 'chosen').prop('mac-address-wifi0') == bytes.fromhex('025249564501')
    assert child(child(parsed, 'arm-io'), 'gic').prop('compatible') == b'arm,gic-v2\0'
    fbcon = child(parsed, 'sweet-fbcon')
    disp = child(child(parsed, 'arm-io'), 'disp0')
    assert struct.unpack('<I', disp.prop('canvas-width'))[0] == 1125
    assert struct.unpack('<I', disp.prop('canvas-height'))[0] == 2436
    assert struct.unpack('<QQ', fbcon.prop('reg')) == (FB, FB_SIZE)
    assert struct.unpack('<I', fbcon.prop('stride'))[0] == FB_STRIDE
    assert struct.unpack('<I', fbcon.prop('bytes-per-pixel'))[0] == FB_BPP
    assert FB_STRIDE * 2270 <= FB_SIZE
    emmc = child(parsed, 'river-emmc')
    assert struct.unpack('<6Q', emmc.prop('reg')) == EMMC_HC + EMMC_CORE + GCC_SDCC1
    with open(args.output, 'wb') as file:
        file.write(blob)
    if args.dump:
        adt.dump(parsed)
    print(f'{args.output}: {len(blob)} bytes')


if __name__ == '__main__':
    main()
