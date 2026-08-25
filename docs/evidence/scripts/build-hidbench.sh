#!/usr/bin/env bash
# Builds hidbench.c twice, once against each hidapi backend already vendored
# by this repo's two CMake build trees (build/ = hidraw, build-libusb/ = libusb).
# Run from the repo root. Produces docs/evidence/scripts/hidbench-hidraw and
# docs/evidence/scripts/hidbench-libusb; neither build step needs root.
set -euo pipefail
cd "$(dirname "$0")/../../.."

INC=build/_deps/hidapi-src/hidapi
SCRIPTS=docs/evidence/scripts

g++ -O2 -I "$INC" "$SCRIPTS/hidbench.c" \
  build/_deps/hidapi-build/src/linux/libhidapi-hidraw.a \
  -ludev \
  -o "$SCRIPTS/hidbench-hidraw"

g++ -O2 -I "$INC" "$SCRIPTS/hidbench.c" \
  build-libusb/_deps/hidapi-build/src/libusb/libhidapi-libusb.a \
  build-libusb/_deps/libusb-build/libusb-1.0.a \
  -ludev -lpthread \
  -o "$SCRIPTS/hidbench-libusb"

echo "built $SCRIPTS/hidbench-hidraw and $SCRIPTS/hidbench-libusb"
