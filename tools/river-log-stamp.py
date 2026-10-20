#!/usr/bin/env python3
"""Follow a growing river live log and write a host-timestamped copy.

Each complete line gets "+SECONDS" relative to BOOT_EPOCH (the host time of
`fastboot boot`). Lines matching --drop (regex) are counted but not copied;
a per-interval count of dropped lines is emitted so floods stay visible.
Usage: river-log-stamp.py LIVE.txt OUT.txt BOOT_EPOCH [--drop REGEX]
"""
import argparse
import os
import re
import time

ap = argparse.ArgumentParser()
ap.add_argument('live')
ap.add_argument('out')
ap.add_argument('boot_epoch', type=float)
ap.add_argument('--drop', default=r'^SWEET USB: event 0x')
args = ap.parse_args()
drop = re.compile(args.drop)

fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
out = os.fdopen(fd, 'w', buffering=1)
with open(args.live, 'rb') as live:
    pending, dropped, last_report = b'', 0, time.time()
    while True:
        chunk = live.read(65536)
        now = time.time()
        if chunk:
            pending += chunk
            *lines, pending = pending.split(b'\n')
            for raw in lines:
                line = raw.decode('utf-8', 'replace').rstrip('\r')
                if drop.search(line):
                    dropped += 1
                    continue
                out.write('+%7.1f %s\n' % (now - args.boot_epoch, line))
        if now - last_report >= 10:
            if dropped:
                out.write('+%7.1f [stamp] dropped %d flood lines in last %.0fs\n'
                          % (now - args.boot_epoch, dropped, now - last_report))
            dropped, last_report = 0, now
        if not chunk:
            time.sleep(0.2)
