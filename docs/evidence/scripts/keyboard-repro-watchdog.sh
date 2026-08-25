#!/usr/bin/env bash
# Full repro of the original bug on the actual affected device (0d62:3740,
# the Darfon keyboard) using the libusb-backend build, with a self-contained
# recovery path -- the target process never needs keyboard input to reach
# the hang (stdin is a FIFO this script itself holds open, and it runs under
# setsid so /dev/tty is unreachable), and this script kills + recovers it
# without needing the affected keyboard either. A detached watchdog is armed
# BEFORE anything is opened as a backstop in case this script's own logic
# fails partway through.
#
# Even so: this detaches your keyboard's usbhid binding for the run. Have a
# second input path ready (a mouse can drive an on-screen keyboard; SSH from
# another machine also works) before starting, as defense in depth -- you
# should not need it if this script behaves as designed, but don't run this
# unattended.
#
# Needs root. This repo's Bash-tool session has no controlling terminal and
# can't authenticate sudo, so run this yourself, watching the output live:
#   sudo bash docs/evidence/scripts/keyboard-repro-watchdog.sh
#
# Produces, under docs/evidence/data/:
#   kbd-repro-driver-timeline.log   -- sysfs driver-bind state at each step
#   kbd-repro-dmesg.log             -- dmesg tail across the whole run
#   kbd-repro-hang-backtrace.log    -- `thread apply all bt` while genuinely blocked
#   kbd-repro-watchdog.log          -- the backstop watchdog's own log (should be empty/no-op)
set -uo pipefail   # not -e: recovery steps must all run even if an earlier one fails
cd "$(dirname "$0")/../../.."

OUT=docs/evidence/data
mkdir -p "$OUT"

if [ "$(id -u)" -ne 0 ]; then
  echo "must run as root" >&2
  exit 1
fi

BIN=build-libusb/alienfx_cli
if [ ! -x "$BIN" ]; then
  echo "missing $BIN -- configure/build build-libusb first (see docs/evidence/README.md)" >&2
  exit 1
fi

# --- locate the Darfon device and its keyboard (usbhid-bound) interface ---
DEVDIR=""
for d in /sys/bus/usb/devices/*/; do
  [ -f "${d}idVendor" ] || continue
  [ -f "${d}idProduct" ] || continue
  if [ "$(cat "${d}idVendor")" = "0d62" ] && [ "$(cat "${d}idProduct")" = "3740" ]; then
    DEVDIR="$d"
    break
  fi
done
if [ -z "$DEVDIR" ]; then
  echo "device 0d62:3740 not found -- check 'lsusb'; this script targets this specific fork's test hardware" >&2
  exit 1
fi
IFACE=""
for i in "${DEVDIR}"*:*/; do
  [ -e "${i}driver" ] || continue
  if [ "$(basename "$(readlink -f "${i}driver")")" = "usbhid" ]; then
    IFACE="$i"
    break
  fi
done
if [ -z "$IFACE" ]; then
  echo "no usbhid-bound interface found under $DEVDIR -- is the keyboard already detached from a previous failed run? Check manually before proceeding." >&2
  exit 1
fi
IFACE_NAME="$(basename "${IFACE%/}")"   # e.g. "3-8:1.1"
BUS_PORT="$(basename "$DEVDIR")"        # e.g. "3-8"
echo "Target interface: $IFACE_NAME (currently bound to usbhid)"

log() { echo "=== $(date -Ins) $* ==="; }

{
  log "start"
  echo "IFACE_NAME=$IFACE_NAME BUS_PORT=$BUS_PORT"
  readlink -f "${IFACE}driver" || echo "(no driver bound)"
} > "$OUT/kbd-repro-driver-timeline.log"

dmesg -c > /dev/null 2>&1 || true   # clear the ring buffer so kbd-repro-dmesg.log is just this run
: > "$OUT/kbd-repro-dmesg.log"

recover() {
  # Idempotent: safe to call more than once (by both the main script and the
  # watchdog), and safe to call even if the interface is already back.
  {
    log "recover() invoked"
  } >> "$OUT/kbd-repro-driver-timeline.log"
  pkill -9 -f "$BIN" 2>/dev/null || true
  sleep 1
  if [ ! -e "${IFACE}driver" ]; then
    echo -n "$IFACE_NAME" > /sys/bus/usb/drivers/usbhid/bind 2>/dev/null || true
    sleep 1
  fi
  if [ ! -e "${IFACE}driver" ] || [ "$(basename "$(readlink -f "${IFACE}driver")")" != "usbhid" ]; then
    echo -n "$BUS_PORT" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null || true
    sleep 1
    echo -n "$BUS_PORT" > /sys/bus/usb/drivers/usb/bind 2>/dev/null || true
    sleep 1
  fi
  {
    echo "post-recover driver: $(readlink -f "${IFACE}driver" 2>&1 || echo '(still no driver bound)')"
  } >> "$OUT/kbd-repro-driver-timeline.log"
}

# --- arm the backstop watchdog before opening anything ---
WATCHDOG_TIMEOUT=45
( sleep "$WATCHDOG_TIMEOUT"
  {
    echo "=== $(date -Ins) watchdog fired after ${WATCHDOG_TIMEOUT}s ==="
    echo "driver at fire time: $(readlink -f "${IFACE}driver" 2>&1 || echo '(no driver bound)')"
  } > "$OUT/kbd-repro-watchdog.log"
  if [ ! -e "${IFACE}driver" ] || [ "$(basename "$(readlink -f "${IFACE}driver")" 2>/dev/null)" != "usbhid" ]; then
    echo "watchdog: interface not on usbhid, recovering" >> "$OUT/kbd-repro-watchdog.log"
    pkill -9 -f "$BIN" 2>/dev/null || true
    sleep 1
    echo -n "$IFACE_NAME" > /sys/bus/usb/drivers/usbhid/bind 2>/dev/null || true
    sleep 1
    if [ ! -e "${IFACE}driver" ] || [ "$(basename "$(readlink -f "${IFACE}driver")" 2>/dev/null)" != "usbhid" ]; then
      echo -n "$BUS_PORT" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null || true
      sleep 1
      echo -n "$BUS_PORT" > /sys/bus/usb/drivers/usb/bind 2>/dev/null || true
    fi
  else
    echo "watchdog: interface already back on usbhid, no-op" >> "$OUT/kbd-repro-watchdog.log"
  fi
) </dev/null >/dev/null 2>&1 &
disown
WATCHDOG_SHELL_PID=$!
echo "Watchdog armed (backstop pid $WATCHDOG_SHELL_PID, fires in ${WATCHDOG_TIMEOUT}s if this script doesn't clean up first)"

# --- FIFO stdin: kept open for writing by this script (fd 3) so the child's
# read/poll never sees EOF -- it just blocks forever, exactly like a human
# who never answers, without needing an actual human. ---
FIFO="$(mktemp -u /tmp/alienfx-repro-stdin.XXXXXX)"
mkfifo "$FIFO"
exec 3<>"$FIFO"

log "launching probe" >> "$OUT/kbd-repro-driver-timeline.log"
setsid "$BIN" probe --yes <"$FIFO" >"$OUT/kbd-repro-probe-stdout.log" 2>&1 &
# Not $!: setsid forks when the backgrounded job is already its own process
# group leader (true in some shells/harnesses), so $! can be setsid's own,
# now-exited PID rather than the exec'd alienfx_cli. Find the real PID by
# cmdline instead -- setsid's fork branch reparents the child immediately,
# so it's a plain, independently-findable process by the time this returns.
PROBE_PID=""
for _ in $(seq 1 50); do   # up to 5s
  PROBE_PID="$(pgrep -f "$BIN probe --yes" | head -1)"
  [ -n "$PROBE_PID" ] && break
  sleep 0.1
done
if [ -z "$PROBE_PID" ]; then
  echo "ERROR: could not find the launched probe process" >&2
  recover
  exit 1
fi
echo "probe launched, pid $PROBE_PID"

# --- wait for the interface to actually detach (bounded, not a fixed sleep) ---
DETACHED=0
for _ in $(seq 1 100); do   # up to 10s
  if [ ! -e "${IFACE}driver" ]; then
    DETACHED=1
    break
  fi
  sleep 0.1
done

{
  echo "detach observed: $DETACHED (after up to 10s poll)"
  readlink -f "${IFACE}driver" 2>&1 || echo "(no driver bound -- DETACHED, confirmed)"
} >> "$OUT/kbd-repro-driver-timeline.log"
dmesg | tail -n 40 >> "$OUT/kbd-repro-dmesg.log"

if [ "$DETACHED" -ne 1 ]; then
  echo "WARNING: interface never showed as detached -- probe may have failed to open it, or this machine's timing differs. Capturing whatever state exists, then recovering." >&2
fi

# --- capture the hang: the process should be blocked in poll()/read() by now ---
sleep 1
echo "Attaching gdb to pid $PROBE_PID to capture the hang backtrace..."
gdb -p "$PROBE_PID" -batch -q \
  -ex 'set debuginfod enabled off' \
  -ex 'thread apply all bt' \
  -ex detach \
  -ex quit \
  > "$OUT/kbd-repro-hang-backtrace.log" 2>&1
echo "Backtrace saved to $OUT/kbd-repro-hang-backtrace.log"

# --- tear down and recover, regardless of what the backtrace capture did ---
recover

# release our FIFO writer fd and remove it
exec 3>&- 2>/dev/null || true
rm -f "$FIFO"

FINAL_DRIVER="$(readlink -f "${IFACE}driver" 2>/dev/null || echo '(none)')"
{
  log "final state"
  echo "final driver: $FINAL_DRIVER"
} >> "$OUT/kbd-repro-driver-timeline.log"
dmesg | tail -n 40 >> "$OUT/kbd-repro-dmesg.log"

if [ "$(basename "$FINAL_DRIVER")" = "usbhid" ]; then
  echo "RECOVERED: $IFACE_NAME is back on usbhid."
  echo "Please confirm with an actual keypress that the keyboard responds before trusting this."
  # Disarm the backstop watchdog now that recovery is confirmed by sysfs.
  kill "$WATCHDOG_SHELL_PID" 2>/dev/null || true
else
  echo "NOT RECOVERED (driver: $FINAL_DRIVER) -- the watchdog will still fire in up to ${WATCHDOG_TIMEOUT}s from script start as a backstop. If it also fails, use the manual recovery commands in docs/probe-keyboard-lockup.md." >&2
fi

echo
echo "=== driver timeline ==="
cat "$OUT/kbd-repro-driver-timeline.log"
