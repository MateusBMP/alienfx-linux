#!/usr/bin/env bash
# Runs hidbench-libusb (built by build-hidbench.sh) against the LED controller
# (187c:0550 on this machine -- check `lsusb` on yours and edit VIDPID below
# if different) using hidapi's libusb backend, via hid_open(vid,pid) since the
# libusb backend's hid_open_path() takes its own synthetic path string, not a
# /dev node.
#
# Needs root: this repo's Bash-tool session has no controlling terminal, so
# sudo can't be authenticated from there. Run this yourself:
#   sudo bash docs/evidence/scripts/run-libusb-bench.sh
#
# This WILL detach the kernel's usbhid driver from 187c:0550's HID interface
# for the duration of each benchmark run (that's the whole point -- it's the
# libusb backend's DETACH_KERNEL_DRIVER behavior in action) and reattach it
# when hidbench-libusb exits. This is the safe device to do this on: nothing
# else depends on this interface's usbhid binding. Do NOT point VIDPID at a
# keyboard/mouse interface with this script -- see keyboard-repro-watchdog.sh
# for that repro, which has its own recovery machinery.
set -euo pipefail
cd "$(dirname "$0")/../../.."

VIDPID="187c:0550"   # Alienware AW-ELC LED controller -- check 'lsusb' on
                     # your machine and edit if different
OUT=docs/evidence/data

if [ "$(id -u)" -ne 0 ]; then
  echo "must run as root (libusb needs write access to the USB device node)" >&2
  exit 1
fi

if ! lsusb -d "$VIDPID" >/dev/null 2>&1; then
  echo "no such device: $VIDPID -- check 'lsusb' and edit VIDPID in this script" >&2
  exit 1
fi

mkdir -p "$OUT"

for mode in feature output write; do
  echo "=== libusb backend: $mode ==="
  docs/evidence/scripts/hidbench-libusb "$VIDPID" "$mode" 34 0 200 \
    > "$OUT/libusb-$mode.csv" 2> "$OUT/libusb-$mode.summary.txt"
  cat "$OUT/libusb-$mode.summary.txt"
done

echo
echo "Done. Results in $OUT/libusb-{feature,output,write}.{csv,summary.txt}"
