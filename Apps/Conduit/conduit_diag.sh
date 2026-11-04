#!/bin/sh
# Read-only USB audio diagnostics. Run it on the device (SSH or a terminal
# app) with a USB interface plugged in; it writes conduit_diag.txt next to
# itself. Nothing is changed on the system.
OUT="$(cd "$(dirname "$0")" && pwd)/conduit_diag.txt"
{
  echo "== date";            date
  echo "== kernel";          uname -a
  echo "== os";              cat /etc/os-release 2>/dev/null | head -5; cat /opt/muos/config/version.txt 2>/dev/null
  echo "== sound cards";     cat /proc/asound/cards 2>/dev/null || echo "no /proc/asound"
  echo "== capture pcms";    grep -i capture /proc/asound/pcm 2>/dev/null
  echo "== /dev/snd";        ls -l /dev/snd 2>/dev/null
  echo "== snd modules";     lsmod 2>/dev/null | grep -i snd
  echo "== usb-audio driver"
  ls /sys/bus/usb/drivers/ 2>/dev/null | tr '\n' ' '; echo
  [ -d /sys/bus/usb/drivers/snd-usb-audio ] && echo "snd-usb-audio: PRESENT" || echo "snd-usb-audio: missing (no USB audio class support in this kernel)"
  echo "== usb devices";     (lsusb 2>/dev/null || cat /sys/bus/usb/devices/*/product 2>/dev/null)
  echo "== usb roles"
  for f in $(find /sys -maxdepth 6 \( -name 'role' -o -name 'otg_role' -o -name 'dr_mode' \) 2>/dev/null); do
    echo "$f: $(cat "$f" 2>/dev/null)"; done
  echo "== audio servers";   ps 2>/dev/null | grep -Ei 'pipewire|pulse|wireplumber' | grep -v grep
  echo "== recent kernel usb messages"; dmesg 2>/dev/null | grep -iE 'usb|snd|audio' | tail -40
} >"$OUT" 2>&1
echo "wrote $OUT"
