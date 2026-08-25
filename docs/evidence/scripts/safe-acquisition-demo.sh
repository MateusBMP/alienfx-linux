#!/usr/bin/env bash
# Demonstrates hidapi's libusb-backend kernel-driver acquisition (detach ->
# claim -> ... -> reattach on close) against 187c:0550, the Alienware LED
# controller -- NOT a keyboard/mouse, so this is safe to run: nothing on this
# machine depends on that interface's usbhid binding. Uses hidbench-libusb
# (built by build-hidbench.sh) directly, which opens *only* this one VID:PID
# via hid_open() -- unlike `alienfx_cli status`, it never touches any other
# HID device, so it can't accidentally detach a keyboard.
#
# Needs root (libusb needs write access to the USB device node) and a real
# controlling terminal for gdb's default prompts, disabled below anyway. This
# repo's Bash-tool session has no controlling terminal and can't authenticate
# sudo, so run this yourself:
#   sudo bash docs/evidence/scripts/safe-acquisition-demo.sh
#
# Produces, under docs/evidence/data/:
#   safe-demo-driver-timeline.log   -- sysfs driver-bind state before/during/after
#   safe-demo-gdb-backtrace.log     -- gdb backtrace at the libusb_detach_kernel_driver call
#   safe-demo-bench.csv/.summary.txt -- the hid_send_output_report timings from that run
set -euo pipefail
cd "$(dirname "$0")/../../.."

VIDPID="187c:0550"
OUT=docs/evidence/data
mkdir -p "$OUT"

if [ "$(id -u)" -ne 0 ]; then
  echo "must run as root" >&2
  exit 1
fi

# Find this device's sysfs directory (has idVendor/idProduct; interface
# sub-directories don't) and its HID interface (the one with a driver link).
DEVDIR=""
for d in /sys/bus/usb/devices/*/; do
  [ -f "${d}idVendor" ] || continue
  [ -f "${d}idProduct" ] || continue
  if [ "$(cat "${d}idVendor")" = "187c" ] && [ "$(cat "${d}idProduct")" = "0550" ]; then
    DEVDIR="$d"
    break
  fi
done
if [ -z "$DEVDIR" ]; then
  echo "device $VIDPID not found -- check 'lsusb'" >&2
  exit 1
fi
IFACE=""
for i in "${DEVDIR}"*:*/; do
  [ -e "${i}driver" ] && IFACE="$i" && break
done
if [ -z "$IFACE" ]; then
  echo "no driver-bound interface found under $DEVDIR" >&2
  exit 1
fi
echo "Using interface: $IFACE (driver: $(readlink -f "${IFACE}driver"))"

{
  echo "=== $(date -Ins) before ==="
  readlink -f "${IFACE}driver" || echo "(no driver bound)"
} > "$OUT/safe-demo-driver-timeline.log"

# Long-enough run (100 output reports, ~6.4s at this endpoint's observed
# ~64ms/report under hidraw -- libusb uses the control endpoint for this
# call, so timing will differ; either way it's long enough to observe
# mid-run driver state) backgrounded so we can sample sysfs while it's open.
docs/evidence/scripts/hidbench-libusb "$VIDPID" output 34 0 100 \
  > "$OUT/safe-demo-bench.csv" 2> "$OUT/safe-demo-bench.summary.txt" &
BENCHPID=$!

sleep 0.3
{
  echo "=== $(date -Ins) during (hidbench-libusb pid $BENCHPID running) ==="
  readlink -f "${IFACE}driver" || echo "(no driver bound -- DETACHED)"
} >> "$OUT/safe-demo-driver-timeline.log"

wait "$BENCHPID" || true

{
  echo "=== $(date -Ins) after ==="
  readlink -f "${IFACE}driver" || echo "(no driver bound)"
} >> "$OUT/safe-demo-driver-timeline.log"

cat "$OUT/safe-demo-driver-timeline.log"
echo
cat "$OUT/safe-demo-bench.summary.txt"

# Now capture the actual acquisition backtrace: break on the real detach
# call, run one iteration, print the stack, let it finish normally.
echo
echo "=== capturing gdb backtrace at libusb_detach_kernel_driver ==="
gdb -batch -q \
  -ex 'set debuginfod enabled off' \
  -ex 'break libusb_detach_kernel_driver' \
  -ex run \
  -ex bt \
  -ex continue \
  --args docs/evidence/scripts/hidbench-libusb "$VIDPID" feature 34 0 1 \
  > "$OUT/safe-demo-gdb-backtrace.log" 2>&1 || true

echo "gdb backtrace saved to $OUT/safe-demo-gdb-backtrace.log:"
sed -n '/^#0/,/^#/p; /^Breakpoint 1,/,/^$/p' "$OUT/safe-demo-gdb-backtrace.log" | head -40

echo
echo "Confirming driver is back after gdb run too:"
readlink -f "${IFACE}driver" || echo "(no driver bound -- investigate before continuing)"
