#!/usr/bin/env python3
"""Receive river's local USB Ethernet kernel log on macOS.

Requires a riverusb=2 kernel and a successfully enumerated CDC-ECM device.
Use an output path outside git. --configure assigns the dedicated USB
interface 192.168.7.1/24 (uses sudo if necessary); no default route is added.
--follow keeps running across phone reboots, one numbered file per USB session.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import time

HOST_MAC = '02:00:5e:77:ee:01'
HOST_IP = '192.168.7.1'
PHONE_IP = '192.168.7.2'


def interfaces(text):
    result, name = {}, None
    for line in text.splitlines():
        match = re.match(r'^([A-Za-z0-9]+):', line)
        if match:
            name = match[1]
            result[name] = {'mac': None, 'ips': []}
        elif name:
            match = re.search(r'\bether\s+([0-9a-fA-F:]{17})\b', line)
            if match:
                result[name]['mac'] = match[1].lower()
            match = re.search(r'\binet\s+(\d+\.\d+\.\d+\.\d+)\b', line)
            if match:
                result[name]['ips'].append(match[1])
    return result


def find_interface(ap):
    state = interfaces(subprocess.check_output(['/sbin/ifconfig', '-a'], text=True))
    matches = [name for name, data in state.items() if data['mac'] == HOST_MAC]
    if len(matches) > 1:
        ap.error('multiple matching USB interfaces; disconnect the extra device')
    return state, (matches[0] if matches else None)


def wait_and_configure(ap, configure):
    print('Waiting for river USB Ethernet (%s)...' % HOST_MAC, flush=True)
    while True:
        state, name = find_interface(ap)
        if name:
            break
        time.sleep(1)
    if any(HOST_IP in data['ips'] for key, data in state.items() if key != name):
        ap.error('%s already belongs to another interface' % HOST_IP)
    if HOST_IP not in state[name]['ips']:
        command = ['/sbin/ifconfig', name, 'inet', HOST_IP, 'netmask', '255.255.255.0', 'alias', 'up']
        if not configure:
            ap.error('rerun with --configure to assign the dedicated USB address')
        if os.geteuid() != 0:
            command = ['/usr/bin/sudo', '-n'] + command
        for attempt in range(10):	# a fresh interface may not accept the alias at once
            try:
                subprocess.run(command, check=True)
                break
            except subprocess.CalledProcessError:
                if command[0] == '/usr/bin/sudo' and attempt == 0 and subprocess.run(
                        ['/usr/bin/sudo', '-n', 'true'], capture_output=True).returncode:
                    manual = ['sudo'] + command[2:]
                    ap.error('USB address setup requires administrator authorization; run this in a local terminal: ' + shlex.join(manual))
                time.sleep(1)
        else:
            ap.error('could not assign %s to %s' % (HOST_IP, name))
    return name


def interface_alive(ap, name):
    state, found = find_interface(ap)
    return found == name and HOST_IP in state[name]['ips']


def session_path(output, n, follow):
    return output if not follow else output.with_name('%s-%d%s' % (output.stem, n, output.suffix))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('--configure', action='store_true')
    ap.add_argument('--follow', action='store_true',
                    help='survive phone reboots: each USB session goes to OUTPUT-1, OUTPUT-2, ...')
    args = ap.parse_args()
    if sys.platform != 'darwin':
        ap.error('this receiver is for macOS')
    output = args.output.expanduser().resolve()
    for parent in output.parents:
        if (parent / '.git').exists():
            ap.error('save phone logs outside any git checkout')
    n = 1
    while session_path(output, n, args.follow).exists():
        if not args.follow:
            ap.error('output already exists; choose a new private log filename')
        n += 1
    output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    while True:
        name = wait_and_configure(ap, args.configure)
        path = session_path(output, n, args.follow)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((HOST_IP, 5140))  # Bind the USB address, not other networks.
            sock.settimeout(5)
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'wb', buffering=0) as log:
                print('%s: USB-only UDP/5140 -> %s' % (name, path), flush=True)
                while True:
                    try:
                        data, source = sock.recvfrom(65535)
                    except socket.timeout:
                        if args.follow and not interface_alive(ap, name):
                            print('\n[receiver] %s gone; waiting for the next boot' % name, flush=True)
                            break
                        continue
                    if source[0] != PHONE_IP:
                        continue
                    log.write(data)
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
        if not args.follow:
            return
        n += 1


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
