#!/usr/bin/env python3
"""Pack development files for river_files.c (installed before PID 1 starts).

  river-files.py OUT SRC:DEST[:MODE] ...

DEST must be under /usr/local/, or (boot-arg riverfilessys=1, 0203) be
/System/Library/xpc/launchd.plist, /bin/sh or under /private/var/root/.ssh/;
the kernel refuses anything else. Layout:
"RIVFILES", u32 count, count x { char path[112]; u32 mode; u32 off; u32 size },
then the files, 16-byte aligned; offsets from the start. The container is
appended to the RAM-only boot image (mkpayload.py --blob RiverFiles=OUT).
"""
import struct, sys

SYSTEM = ('/System/Library/xpc/launchd.plist', '/bin/sh')

def allowed(dest):
    return (dest.startswith('/usr/local/') or dest in SYSTEM or
            dest.startswith('/private/var/root/.ssh/'))

def main(out, specs):
    files = []
    for spec in specs:
        parts = spec.split(':')
        src, dest = parts[0], parts[1]
        mode = int(parts[2], 8) if len(parts) > 2 else 0o755
        assert allowed(dest) and '..' not in dest and len(dest) < 112, dest
        files.append((dest, mode, open(src, 'rb').read()))
    off = (12 + 128 * len(files) + 15) & ~15
    table, data = b'', b''
    for dest, mode, d in files:
        table += struct.pack('<112sIIII', dest.encode(), mode, off + len(data), len(d), 0)
        data += d + b'\0' * (-len(d) & 15)
    blob = b'RIVFILES' + struct.pack('<I', len(files)) + table
    blob += b'\0' * (off - len(blob)) + data
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blob)} bytes, ' + ', '.join(f'{d} {len(x)}' for d, _, x in files))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
