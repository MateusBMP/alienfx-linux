#!/usr/bin/env bash
# Builds recipient-probe.c against the libusb already vendored by this repo's
# CMake build tree. Run from anywhere; needs no root.
set -euo pipefail
cd "$(dirname "$0")/../../.."

SCRIPTS=docs/evidence/scripts
LIBUSB_SRC=build/_deps/libusb-src/libusb/libusb
LIBUSB_A=build/_deps/libusb-build/libusb-1.0.a

if [[ ! -f $LIBUSB_A ]]; then
  echo "missing $LIBUSB_A -- configure and build the project first:" >&2
  echo "  cmake -S . -B build -G Ninja && cmake --build build" >&2
  exit 1
fi

cc -O2 -Wall -Wextra -I "$LIBUSB_SRC" "$SCRIPTS/recipient-probe.c" \
  "$LIBUSB_A" -ludev -lpthread \
  -o "$SCRIPTS/recipient-probe"

echo "built $SCRIPTS/recipient-probe"
