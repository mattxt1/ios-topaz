#!/usr/bin/env python3
"""Validate and optionally stage the Moto G7's private WCNSS split firmware.

Usage: river-wcnss-fw.py /path/to/private/wcnss-firmware [--stage PRIVATE_DIR]
Staging writes the PAS metadata and 7 MiB relocated carveout image only to
PRIVATE_DIR. Neither output authenticates or starts the radio.
"""
import argparse
from pathlib import Path
import hashlib
import json
import struct

CARVEOUT = 0x8e700000
CARVEOUT_SIZE = 0x700000
MDT_HASH = 2 << 24
MDT_RELOCATABLE = 1 << 27


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    parser.add_argument('--stage', type=Path, metavar='PRIVATE_DIR')
    args = parser.parse_args()
    root = args.firmware
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
            segments.append((index, addr, filesz, memsz, flags))

    if len(hashes) != 1 or not segments:
        raise ValueError('expected one hash segment and loadable segments')
    hash_data = (root / f'wcnss.b{hashes[0]:02d}').read_bytes()
    header_size = struct.unpack_from('<I', mdt, phoff + 16)[0]
    if header_size < phoff + entsize * count or header_size > len(mdt):
        raise ValueError('invalid PAS ELF header size')
    if mdt[header_size:header_size + len(hash_data)] != hash_data:
        raise ValueError('packed MDT hash differs from split hash segment')
    for index in range(count):
        part = root / f'wcnss.b{index:02d}'
        if not part.is_file() or index in hashes:
            continue
        digest_off = 40 + index * 32
        digest = hashlib.sha256(part.read_bytes()).digest()
        if hash_data[digest_off:digest_off + 32] != digest:
            raise ValueError(f'{part.name}: digest differs from embedded hash segment')
    lo = min(addr for _, addr, _, _, _ in segments)
    hi = max(addr + size for _, addr, _, size, _ in segments)
    if not all(flags & MDT_RELOCATABLE for _, _, _, _, flags in segments):
        raise ValueError('non-relocatable WCNSS segment')
    slide = CARVEOUT - lo
    if hi + slide > CARVEOUT + CARVEOUT_SIZE:
        raise ValueError('relocated firmware exceeds reserved WCNSS carveout')
    nv = root / 'WCNSS_qcom_wlan_nv.bin'
    if not nv.is_file() or nv.stat().st_size == 0:
        raise ValueError('missing River WLAN NV file')
    print(f'WCNSS: {len(segments)} loadable segments, hash index {hashes[0]}; file digests match')
    print(f'firmware address {lo:#x}..{hi:#x}; relocate by {slide:#x}')
    print(f'relocated address {lo+slide:#x}..{hi+slide:#x} within {CARVEOUT:#x}+{CARVEOUT_SIZE:#x}')
    print(f'WLAN NV: {nv.stat().st_size} bytes')

    if args.stage:
        occupied = []
        image = bytearray(CARVEOUT_SIZE)
        records = []
        for index, addr, filesz, memsz, _ in segments:
            start = addr - lo
            end = start + memsz
            if start < 0 or end > len(image):
                raise ValueError(f'segment {index} exceeds carveout')
            if any(start < other_end and other_start < end for other_start, other_end in occupied):
                raise ValueError(f'segment {index} overlaps another loadable segment')
            occupied.append((start, end))
            if filesz:
                image[start:start + filesz] = (root / f'wcnss.b{index:02d}').read_bytes()
            records.append({'index': index, 'physical': CARVEOUT + start,
                            'file_size': filesz, 'memory_size': memsz})
        metadata = mdt[:header_size] + hash_data
        args.stage.mkdir(parents=True, exist_ok=True)
        (args.stage / 'wcnss-pas-metadata.bin').write_bytes(metadata)
        (args.stage / 'wcnss-carveout.bin').write_bytes(image)
        (args.stage / 'wcnss-stage.json').write_text(json.dumps({
            'pas_id': 6, 'carveout_base': CARVEOUT, 'carveout_size': CARVEOUT_SIZE,
            'metadata_sha256': hashlib.sha256(metadata).hexdigest(),
            'image_sha256': hashlib.sha256(image).hexdigest(),
            'segments': records,
        }, indent=2) + '\n')
        print(f'staged PAS metadata and carveout image in {args.stage}')


if __name__ == '__main__':
    main()
