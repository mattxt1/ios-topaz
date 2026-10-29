#!/usr/bin/env python3
"""Make River's /System/Library/xpc/launchd.plist from the stock one.

  river-launchd-plist.py STOCK OUT

Removes jobs that restart every ~10 s on River (fairplayd.H2: no FairPlay;
bluetoothd: no radio; lskdd), plus unsupported voicemail and runaway crash
reporting (vmd: no baseband; ReportCrash: observed sustained CPU load).
Adds dropbear on port 22 plus a one-shot job that creates its host key (the same multi-call binary run as
dropbearkey; dropbear -R would write to /etc/dropbear, which iOS lacks).
Installed by the boot image's file drop with riverfilessys=1 (0203); needs
kernel 0204 or later when OUT is shorter than the file on the phone.
"""
import plistlib, sys

REMOVE = ('com.apple.fairplayd.H2', 'com.apple.bluetoothd', 'com.apple.lskdd',
          'com.apple.voicemail.vmd', 'com.apple.ReportCrash')
KEY = '/usr/local/etc/dropbear/dropbear_ed25519_host_key'

def main(stock, out):
    p = plistlib.load(open(stock, 'rb'))
    d = p['LaunchDaemons']
    for label in REMOVE:
        d.pop(f'/System/Library/LaunchDaemons/{label}.plist', None)
    d['/System/Library/LaunchDaemons/org.river.dropbear.plist'] = {
        'Label': 'org.river.dropbear',
        'ProgramArguments': ['/usr/local/bin/dropbear', '-F', '-p', '22', '-r', KEY],
        'RunAtLoad': True, 'KeepAlive': True, 'UserName': 'root',
        'EnvironmentVariables': {'PATH': '/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
    }
    d['/System/Library/LaunchDaemons/org.river.dropbearkey.plist'] = {
        'Label': 'org.river.dropbearkey',
        'Program': '/usr/local/bin/dropbear',
        'ProgramArguments': ['dropbearkey', '-t', 'ed25519', '-f', KEY],
        'RunAtLoad': True, 'LaunchOnlyOnce': True, 'UserName': 'root',
    }
    plistlib.dump(p, open(out, 'wb'), fmt=plistlib.FMT_BINARY)
    print(f'{out}: {len(d)} jobs')

if __name__ == '__main__':
    main(*sys.argv[1:])
