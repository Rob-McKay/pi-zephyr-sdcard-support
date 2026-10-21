#!/usr/bin/env bash
# The SD driver review tests (tests/sdhost_hw) for a real Raspberry Pi 3A+.
# Runs inside the Docker image.
#
#   scripts/sd-hw-test.sh          build, and write out/sd-hw-test/sdcard.img
#   scripts/sd-hw-test.sh --qemu   also boot it on QEMU's emulated Pi 3A+ with
#                                  a scratch card, as a smoke test (QEMU has
#                                  none of the timing the tests look for)
#
# Flash the image to a SPARE card (the tests write its last 16 MB), boot the
# Pi and read the results on the serial console (GPIO 14/15, 115200 baud).
# RPI_CORE_FREQ=400 sets core_freq in config.txt, for finding 3; the console
# baud rate then scales with it (400/250 x 115200 = 184320).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
zephyr_tests=$here/tests/sdhost_hw
out=$here/build/sd-hw-test

west build -p auto -b rpi_3a_plus -d "$out/rpi" "$zephyr_tests"
"$here/scripts/mksdcard.sh" "$out/rpi" "$here/out/sd-hw-test"

[[ ${1:-} == --qemu ]] || exit 0

west build -p auto -b rpi_3a_plus -d "$out/qemu" "$zephyr_tests" -- \
	-DEXTRA_DTC_OVERLAY_FILE="$here/qemu/raspi3ap.overlay"
truncate -s 0 "$out/card.img"
truncate -s 64M "$out/card.img"
log=$out/qemu.log
"$here/scripts/run-qemu.sh" "$out/qemu" \
	-drive if=sd,format=raw,file="$out/card.img" </dev/null >"$log" 2>&1 &
qemu=$!
for _ in $(seq 1 300); do
	grep -qE "PROJECT EXECUTION (SUCCESSFUL|FAILED)|FATAL ERROR" "$log" && break
	sleep 1
done
kill "$qemu" 2>/dev/null || true
wait "$qemu" 2>/dev/null || true
grep -E "^ *(PASS|FAIL|SKIP) - |SUITE (PASS|FAIL)|PROJECT EXECUTION" "$log" || true
echo "QEMU log: $log"
