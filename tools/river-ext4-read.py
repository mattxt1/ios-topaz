#!/usr/bin/env python3
"""List or extract files from a read-only ext4 image with extent inodes.

Usage: river-ext4-read.py IMAGE|ssh:/dev/remmc0sN PATH [OUTPUT]
Directories are listed; regular files are written to OUTPUT or stdout.
This deliberately has no image-writing code.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys


class Ext4:
    def __init__(self, filename):
        self.f = open(filename, 'rb')
        self.init_superblock()

    def init_superblock(self):
        sb = self.read_at(1024, 1024)
        if self.u16(sb, 56) != 0xef53:
            raise ValueError('not ext4')
        self.block_size = 1024 << self.u32(sb, 24)
        self.inodes_per_group = self.u32(sb, 40)
        self.inode_size = self.u16(sb, 88) or 128
        self.desc_size = max(32, self.u16(sb, 254))
        self.gdt_offset = 2 * self.block_size if self.block_size == 1024 else self.block_size

    @staticmethod
    def u16(data, offset):
        return struct.unpack_from('<H', data, offset)[0]

    @staticmethod
    def u32(data, offset):
        return struct.unpack_from('<I', data, offset)[0]

    def read_at(self, offset, size):
        self.f.seek(offset)
        result = self.f.read(size)
        if len(result) != size:
            raise EOFError(f'short read at {offset:#x}')
        return result

    def inode(self, number):
        group, index = divmod(number - 1, self.inodes_per_group)
        gd = self.read_at(self.gdt_offset + group * self.desc_size, self.desc_size)
        table = self.u32(gd, 8)
        if self.desc_size >= 64:
            table |= self.u32(gd, 40) << 32
        return self.read_at(table * self.block_size + index * self.inode_size, self.inode_size)

    def extents(self, inode):
        def walk(data):
            if self.u16(data, 0) != 0xf30a:
                raise ValueError('inode does not use extents')
            count, depth = self.u16(data, 2), self.u16(data, 6)
            for index in range(count):
                off = 12 + 12 * index
                logical = self.u32(data, off)
                if depth:
                    block = self.u32(data, off + 4) | self.u16(data, off + 8) << 32
                    yield from walk(self.read_at(block * self.block_size, self.block_size))
                else:
                    length = self.u16(data, off + 4) & 0x7fff
                    block = self.u32(data, off + 8) | self.u16(data, off + 6) << 32
                    yield logical, block, length
        yield from walk(inode[40:100])

    def content(self, number):
        inode = self.inode(number)
        size = self.u32(inode, 4) | self.u32(inode, 108) << 32
        result = bytearray(size)
        for logical, physical, blocks in self.extents(inode):
            off = logical * self.block_size
            n = min(blocks * self.block_size, max(0, size - off))
            if n:
                result[off:off+n] = self.read_at(physical * self.block_size, n)
        return bytes(result)

    def directory(self, number):
        data = self.content(number)
        off = 0
        while off + 8 <= len(data):
            ino = self.u32(data, off)
            length = self.u16(data, off + 4)
            if length < 8 or off + length > len(data):
                raise ValueError(f'bad directory entry at {off}')
            if ino:
                name = data[off + 8:off + 8 + data[off + 6]].decode('utf-8', 'replace')
                yield name, ino, data[off + 7]
            off += length

    def lookup(self, path):
        ino = 2
        for part in path.strip('/').split('/'):
            if not part:
                continue
            ino = next((child for name, child, _ in self.directory(ino) if name == part), None)
            if ino is None:
                raise FileNotFoundError(path)
        return ino


class RemoteExt4(Ext4):
    """Read a River eMMC partition through rget; raw reads need 512-byte alignment."""
    def __init__(self, device):
        self.device = device
        self.ssh = str(Path(__file__).resolve().parent / 'river-ssh')
        self.init_superblock()

    def read_at(self, offset, size):
        start = offset // 512 * 512
        end = (offset + size + 511) // 512 * 512
        result = subprocess.run(
            [self.ssh, f'rget {self.device} {start} {end - start}'],
            stdout=subprocess.PIPE, check=True,
        ).stdout
        if len(result) != end - start:
            raise EOFError(f'short remote read at {offset:#x}')
        return result[offset - start:offset - start + size]


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    fs = RemoteExt4(sys.argv[1][4:]) if sys.argv[1].startswith('ssh:') else Ext4(sys.argv[1])
    ino = fs.lookup(sys.argv[2])
    mode = fs.u16(fs.inode(ino), 0) & 0xf000
    if mode == 0x4000:
        for name, child, kind in fs.directory(ino):
            if name not in ('.', '..'):
                print(f'{kind} {child:6} {name}')
    elif mode == 0x8000:
        data = fs.content(ino)
        if len(sys.argv) == 4:
            with open(sys.argv[3], 'wb') as out:
                out.write(data)
        else:
            os.write(1, data)
    else:
        raise ValueError(f'unsupported inode mode {mode:#x}')


if __name__ == '__main__':
    main()
