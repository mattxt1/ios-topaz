#!/usr/bin/env python3
"""Early-boot Apple Device Tree for Moto G7 river (SDM632).

Derived from mkadt.py's topaz tree. Hardware addresses are from the live
river FDT captured on 2026-10-19. This is an early XNU bring-up tree: the
existing SWEET kernel still needs a GICv2 port, and no eMMC driver is bound.
"""

import argparse
import struct

import adt
import mkadt


# A single contiguous window with no fixed reserved-memory node in the live
# FDT: above the 0x90000000 splash and below the 0xeefa1800 crash carve-outs.
# This is enough for early kernel work, not a final whole-RAM layout.
PHYS_BASE = 0xA0000000
MEM_SIZE = 0x4E000000
GICD = 0x0B000000
GICC = 0x0B002000
FB = 0x90001000
FB_SIZE = 0x013FF000
TIMER_HZ = 19_200_000
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
    mkadt.FB_STRIDE = 4352  # Android fb0; ABL's splash layout is unconfirmed
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
    set_prop(disp, 'canvas-height', mkadt.u32(2270))

    root.children = [item for item in root.children if item.name != 'sweet-ufs']
    assert all(item.name != 'sweet-ufs' for item in root.children)
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output')
    parser.add_argument('--dump', action='store_true')
    args = parser.parse_args()
    blob = adt.serialize(make_tree())
    parsed, end = adt.parse(blob)
    assert end == len(blob)
    assert child(child(parsed, 'arm-io'), 'gic').prop('compatible') == b'arm,gic-v2\0'
    with open(args.output, 'wb') as file:
        file.write(blob)
    if args.dump:
        adt.dump(parsed)
    print(f'{args.output}: {len(blob)} bytes')


if __name__ == '__main__':
    main()
