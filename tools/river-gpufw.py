#!/usr/bin/env python3
"""Pack the Adreno 506 microcode into the RiverGPUFW container (local only).

  river-gpufw.py FW_DIR OUT

FW_DIR holds the files copied from the phone's own vendor/modem partitions
(../device-private/gpu-firmware). They are Qualcomm firmware: OUT must stay
outside git and is only appended to the RAM-only boot image
(mkpayload.py --blob RiverGPUFW=OUT; bsd/dev/river_gpu.c reads it).

Layout: "RIVGPUFW", u32 count, count x { char name[24]; u32 off; u32 size },
then the files, 16-byte aligned; offsets are from the start.
"""
import os, struct, sys

NAMES = ['a530_pm4.fw', 'a530_pfp.fw', 'a506_zap.mdt', 'a506_zap.b00', 'a506_zap.b01', 'a506_zap.b02']

def main(fw_dir, out):
    files = [(n, open(os.path.join(fw_dir, n), 'rb').read()) for n in NAMES
             if os.path.exists(os.path.join(fw_dir, n))]
    assert any(n == 'a530_pm4.fw' for n, _ in files) and any(n == 'a530_pfp.fw' for n, _ in files)
    off = (12 + 32 * len(files) + 15) & ~15
    table, data = b'', b''
    for n, d in files:
        table += struct.pack('<24sII', n.encode(), off + len(data), len(d))
        data += d + b'\0' * (-len(d) & 15)
    blob = b'RIVGPUFW' + struct.pack('<I', len(files)) + table
    blob += b'\0' * (off - len(blob)) + data
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blob)} bytes, ' + ', '.join(f'{n} {len(d)}' for n, d in files))

if __name__ == '__main__':
    main(*sys.argv[1:3])
