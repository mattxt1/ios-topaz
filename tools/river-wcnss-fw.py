#!/usr/bin/env python3
"""Validate the Moto G7's private WCNSS MDT split firmware before PAS work.

Usage: river-wcnss-fw.py /path/to/private/wcnss-firmware
The directory must stay out of git. This script only reads files.
"""
from pathlib import Path
import struct
import sys

CARVEOUT = 0x8e700000
CARVEOUT_SIZE = 0x700000
MDT_HASH = 2 << 24
MDT_RELOCATABLE = 1 << 27


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    mdt = (root / 'wcnss.mdt').read_bytes()
    if len(mdt) < 52 or mdt[:4] != b'\x7fELF' or mdt[4] != 1 or mdt[5] != 1:
        raise ValueError('expected little-endian ELF32 WCNSS MDT')
    phoff = struct.unpack_from('<I', mdt, 28)[0]
    entsize, count = struct.unpack_from('<HH', mdt, 42)
    if entsize != 32 or count == 0 or phoff + entsize * count > len(mdt):
        raise ValueError('invalid program-header table')

    segments = []
    hashes = []
    for index in range(count):
        kind, _, _, addr, filesz, memsz, flags, _ = struct.unpack_from(
            '<8I', mdt, phoff + index * entsize)
        if filesz:
            part = root / f'wcnss.b{index:02d}'
            if not part.is_file() or part.stat().st_size != filesz:
                raise ValueError(f'{part.name} missing or wrong size (expected {filesz})')
        if (flags & (7 << 24)) == MDT_HASH:
            hashes.append(index)
        elif kind == 1 and memsz:
            if filesz > memsz:
                raise ValueError(f'segment {index}: file exceeds memory')
            segments.append((index, addr, memsz, flags))

    if len(hashes) != 1 or not segments:
        raise ValueError('expected one hash segment and loadable segments')
    lo = min(addr for _, addr, _, _ in segments)
    hi = max(addr + size for _, addr, size, _ in segments)
    if not all(flags & MDT_RELOCATABLE for _, _, _, flags in segments):
        raise ValueError('non-relocatable WCNSS segment')
    slide = CARVEOUT - lo
    if hi + slide > CARVEOUT + CARVEOUT_SIZE:
        raise ValueError('relocated firmware exceeds reserved WCNSS carveout')
    nv = root / 'WCNSS_qcom_wlan_nv.bin'
    if not nv.is_file() or nv.stat().st_size == 0:
        raise ValueError('missing River WLAN NV file')
    print(f'WCNSS: {len(segments)} loadable segments, hash index {hashes[0]}')
    print(f'firmware address {lo:#x}..{hi:#x}; relocate by {slide:#x}')
    print(f'relocated address {lo+slide:#x}..{hi+slide:#x} within {CARVEOUT:#x}+{CARVEOUT_SIZE:#x}')
    print(f'WLAN NV: {nv.stat().st_size} bytes')


if __name__ == '__main__':
    main()
