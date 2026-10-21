#!/bin/zsh
# Start river's live USB log receiver in a local terminal.
#   tools/river-receiver.sh ../logs-private/NAME.txt
# Asks for the macOS admin password once (only to assign 192.168.7.1 to the
# phone's USB Ethernet interface), keeps the sudo ticket alive, and keeps
# running across phone reboots: each boot is written to NAME-1.txt, NAME-2.txt...
# The first connection of a new USB device may need "Allow accessory" in macOS.
cd "${0:A:h}/.." || exit 1
out=${1:?usage: tools/river-receiver.sh ../logs-private/NAME.txt (outside git)}
echo "Admin password is used only for: ifconfig <river USB iface> inet 192.168.7.1/24 alias"
sudo -v || exit 1
( while sudo -n -v 2>/dev/null; do sleep 60; done ) &
keepalive=$!
trap 'kill $keepalive 2>/dev/null' EXIT INT TERM
python3 tools/river-usbnet-macos.py "$out" --configure --follow
