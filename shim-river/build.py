#!/usr/bin/env python3
"""Build the river shim and a temporary Android boot-header-v1 image.

  build.py --boot BOOT_IMG --dtb BASE_DTB [--payload PAYLOAD] [-o OUT]

BOOT_IMG is the phone's own boot_b.img backup: its 2048-byte header (v1,
cmdline, os_version) is the template, so ABL treats the result like the
installed kernel. BASE_DTB is the single DTB from that image's kernel field;
ABL picks it and applies the active dtbo overlay. The kernel field becomes
gzip(shim Image) + BASE_DTB. The ramdisk is PAYLOAD (tools/mkpayload.py with
tools/mkadt_river.py's ADT), or a tiny empty cpio for a shim-only test.

Inputs and the image contain device data: keep them out of git (out/ is
ignored). Nothing here talks to adb/fastboot.
"""
import argparse, gzip, hashlib, io, os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRCS = ['../shim/start.S', 'river.c', 'console.c', 'loader.c', '../shim/fdt.c']
CFLAGS = ['--target=aarch64-none-elf', '-O2', '-ffreestanding', '-nostdlib', '-fno-pic',
          '-mgeneral-regs-only', '-mstrict-align', '-fno-stack-protector', '-Wall', '-Wextra',
          '-Werror', '-mcpu=cortex-a53']
R_AARCH64_ABS64 = 257
PAGE = 2048


def tool(name, env):
    path = os.environ.get(env) or shutil.which(name) or (
        '/opt/homebrew/opt/lld/bin/' + name if os.path.exists('/opt/homebrew/opt/lld/bin/' + name) else None)
    if not path:
        sys.exit(f'{name} not found (brew install lld, or set {env})')
    return path


def elf_sections(data):
    assert data[:4] == b'\x7fELF' and data[4:6] == b'\x02\x01', 'ELF64 LE expected'
    shoff, = struct.unpack_from('<Q', data, 40)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 58)
    secs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    names = secs[shstrndx]
    strtab = data[names[4]:names[4] + names[5]]
    return [(strtab[s[0]:].split(b'\0', 1)[0].decode(), s) for s in secs]


def check_no_abs64(obj):
    """The Image runs wherever ABL puts it with no relocation pass."""
    data = open(obj, 'rb').read()
    for name, s in elf_sections(data):
        if s[1] == 4:	# SHT_RELA
            for off in range(s[4], s[4] + s[5], 24):
                _, info, _ = struct.unpack_from('<QQq', data, off)
                if info & 0xffffffff == R_AARCH64_ABS64 and not obj.endswith('start.o'):
                    sys.exit(f'{obj}: absolute relocation in {name}')


def flat_binary(elf):
    """PT_LOAD segments laid out from address 0, like objcopy -O binary."""
    data = open(elf, 'rb').read()
    phoff, = struct.unpack_from('<Q', data, 32)
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    out = bytearray()
    for i in range(phnum):
        p_type, _, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if p_type != 1:
            continue
        end = vaddr + memsz
        if len(out) < end:
            out.extend(b'\0' * (end - len(out)))
        out[vaddr:vaddr + filesz] = data[off:off + filesz]
    return bytes(out)


def symbol(elf, want):
    data = open(elf, 'rb').read()
    secs = elf_sections(data)
    symtab = next(s for n, s in secs if s[1] == 2)
    strtab = secs[symtab[6]][1]
    names = data[strtab[4]:strtab[4] + strtab[5]]
    for off in range(symtab[4], symtab[4] + symtab[5], 24):
        name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', data, off)
        if names[name:].split(b'\0', 1)[0].decode() == want:
            return value
    sys.exit(f'{want} not in {elf}')


def build_image(out, extra=()):
    cc = tool('clang', 'RIVER_SHIM_CC')
    lld = tool('ld.lld', 'RIVER_SHIM_LD')
    objs = []
    for src in SRCS:
        obj = os.path.join(out, os.path.basename(src).rsplit('.', 1)[0] + '.o')
        subprocess.run([cc] + CFLAGS + list(extra) + ['-c', os.path.join(HERE, src), '-o', obj], check=True)
        check_no_abs64(obj)
        objs.append(obj)
    elf = os.path.join(out, 'shim.elf')
    subprocess.run([lld, '-static', '-T', os.path.join(HERE, '../shim/shim.ld'), '--build-id=none',
                    '-N', '-o', elf] + objs, check=True)
    image = flat_binary(elf)
    end = symbol(elf, '_end')
    image = image.ljust(end, b'\0')	# ABL copies the file: .bss and stacks must exist
    assert image[56:60] == b'ARM\x64', 'arm64 Image magic'
    assert struct.unpack_from('<Q', image, 16)[0] == len(image), 'image_size == file size'
    assert struct.unpack_from('<Q', image, 24)[0] == 0xa, 'Image flags'
    open(os.path.join(out, 'Image'), 'wb').write(image)
    return image


def empty_cpio():
    trailer = b'070701' + b'00000000' * 11 + b'0000000B' + b'00000000' + b'TRAILER!!!\0'
    trailer += b'\0' * (-len(trailer) % 4)
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', mtime=0) as g:
        g.write(trailer.ljust(512, b'\0'))
    return buf.getvalue()


def align(n, a=PAGE):
    return (n + a - 1) // a * a


def pack(template, dtb, image, ramdisk):
    assert template[:8] == b'ANDROID!', 'template is not an Android boot image'
    kernel_size, kernel_addr, rd_size, rd_addr, second_size, _, tags_addr, page, version, _ = \
        struct.unpack_from('<10I', template, 8)
    assert (version, page, second_size) == (1, PAGE, 0), 'river boot_b: header v1, 2 KiB pages'
    assert struct.unpack_from('<I', template, 1632)[0] == 0, 'no recovery DTBO'
    assert dtb[:4] == b'\xd0\x0d\xfe\xed', 'base DTB magic'
    kernel = gzip.compress(image, mtime=0) + dtb
    header = bytearray(template[:PAGE])
    struct.pack_into('<I', header, 8, len(kernel))
    struct.pack_into('<I', header, 16, len(ramdisk))
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b'', b''):
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    header[576:608] = sha.digest().ljust(32, b'\0')
    out = bytes(header) + kernel.ljust(align(len(kernel)), b'\0') + ramdisk.ljust(align(len(ramdisk)), b'\0')
    # Independent re-read of what ABL will parse.
    assert out[:8] == b'ANDROID!'
    k, = struct.unpack_from('<I', out, 8)
    r, = struct.unpack_from('<I', out, 16)
    assert out[PAGE:PAGE + k] == kernel and gzip.decompress(kernel[:-len(dtb)]) == image
    assert out[PAGE + align(k):PAGE + align(k) + r] == ramdisk
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--boot', required=True, help="the phone's boot_b.img backup (header template)")
    ap.add_argument('--dtb', required=True, help='base DTB extracted from that image')
    ap.add_argument('--payload', help='SWEETXNU payload from tools/mkpayload.py')
    ap.add_argument('--no-gold-clk', action='store_true',
                    help='leave the Gold cluster clock at its reset source (pre-r15 behaviour)')
    ap.add_argument('-o', '--output', default=os.path.join(HERE, 'out', 'river-shim-boot.img'))
    args = ap.parse_args()
    out = os.path.join(HERE, 'out')
    os.makedirs(out, exist_ok=True)
    image = build_image(out, ['-DRIVER_NO_GOLD_CLK'] if args.no_gold_clk else [])
    if args.payload:
        ramdisk = open(args.payload, 'rb').read()
        assert ramdisk[:8] == b'SWEETXNU', 'payload magic'
    else:
        ramdisk = empty_cpio()
    img = pack(open(args.boot, 'rb').read(), open(args.dtb, 'rb').read(), image, ramdisk)
    open(args.output, 'wb').write(img)
    print(f'Image: {len(image)} bytes, sha256 {hashlib.sha256(image).hexdigest()}')
    print(f'ramdisk: {len(ramdisk)} bytes ({"payload" if args.payload else "empty cpio"})')
    print(f'{args.output}: {len(img)} bytes, sha256 {hashlib.sha256(img).hexdigest()}')


if __name__ == '__main__':
    main()
