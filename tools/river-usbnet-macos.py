#!/usr/bin/env python3
"""Receive river's local USB Ethernet kernel log on macOS.

Requires a riverusb=2 kernel and a successfully enumerated CDC-ECM device.
Use an output path outside git. --configure assigns the dedicated USB
interface 192.168.7.1/24 (uses sudo if necessary); no default route is added.
"""
import argparse
import os
from pathlib import Path
import re
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('--configure', action='store_true')
    args = ap.parse_args()
    if sys.platform != 'darwin':
        ap.error('this receiver is for macOS')
    output = args.output.expanduser().resolve()
    for parent in output.parents:
        if (parent / '.git').exists():
            ap.error('save phone logs outside any git checkout')
    if output.exists():
        ap.error('output already exists; choose a new private log filename')
    print('Waiting for river USB Ethernet (%s)...' % HOST_MAC, flush=True)
    while True:
        state = interfaces(subprocess.check_output(['/sbin/ifconfig', '-a'], text=True))
        matches = [name for name, data in state.items() if data['mac'] == HOST_MAC]
        if len(matches) > 1:
            ap.error('multiple matching USB interfaces; disconnect the extra device')
        if matches:
            break
        time.sleep(1)
    name = matches[0]
    if any(HOST_IP in data['ips'] for key, data in state.items() if key != name):
        ap.error('%s already belongs to another interface' % HOST_IP)
    if HOST_IP not in state[name]['ips']:
        command = ['/sbin/ifconfig', name, 'inet', HOST_IP, 'netmask', '255.255.255.0', 'alias', 'up']
        if not args.configure:
            ap.error('rerun with --configure to assign the dedicated USB address')
        if os.geteuid() != 0:
            command = ['/usr/bin/sudo', '-n'] + command
        try:
            subprocess.run(command, check=True)
        except subprocess.CalledProcessError:
            ap.error('USB address setup requires sudo authorization; run the printed ifconfig command in a local terminal: ' + ' '.join(command[2:] if command[0] == '/usr/bin/sudo' else command))
    output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((HOST_IP, 5140))  # Bind the USB address, not other networks.
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb', buffering=0) as log:
            print('%s: USB-only UDP/5140 -> %s' % (name, output), flush=True)
            while True:
                data, source = sock.recvfrom(65535)
                if source[0] != PHONE_IP:
                    continue
                log.write(data)
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
