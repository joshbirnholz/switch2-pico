#!/bin/sh
# Run a command with Wi-Fi off, then turn Wi-Fi back on, even if the
# command fails. The MacBook's Bluetooth drops LE links to the controller
# while Wi-Fi is on 2.4 GHz (see headset-audio.md).
#
# Usage: research/audio/wifi-off.sh COMMAND [ARGS...]
trap 'nmcli radio wifi on' EXIT INT TERM
nmcli radio wifi off
sleep 2
"$@"
status=$?
nmcli radio wifi on
trap - EXIT INT TERM
exit $status
