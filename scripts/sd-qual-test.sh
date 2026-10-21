#!/usr/bin/env bash
# SD card qualification (tests/sdcard_hw) for a real Raspberry Pi 3A+: the
# SDHost driver from Zephyr PR #110196 through Zephyr's disk access API.
# Runs inside the Docker image. See docs/TESTING.md.
#
#   scripts/sd-qual-test.sh          build, and write out/sd-qual-test/sdcard.img
#   scripts/sd-qual-test.sh --qemu   also boot it on QEMU's emulated Pi 3A+
#                                    with a 256 MB scratch card, as a smoke
#                                    test (a 20 s soak; QEMU has none of the
#                                    real controller's timing)
#
# Environment:
#   SD_QUAL_SOAK_SECONDS   soak duration (default 600)
#   SD_QUAL_SCRATCH_MB     scratch area at the end of the card (default 64)
#   SD_QUAL_SEED           random seed, to repeat a run (default: from the clock)
#   RPI_CORE_FREQ          VPU core clock in MHz (default 250); the console
#                          baud rate scales with it
#
# Flash the image to a SPARE card (the tests write its last
# SD_QUAL_SCRATCH_MB), boot the Pi and capture the serial console (GPIO
# 14/15, 115200 baud).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
app=$here/tests/sdcard_hw
out=$here/build/sd-qual-test

conf=(-DCONFIG_SDCARD_HW_SOAK_SECONDS="${SD_QUAL_SOAK_SECONDS:-600}"
	-DCONFIG_SDCARD_HW_SCRATCH_MB="${SD_QUAL_SCRATCH_MB:-64}"
	-DCONFIG_SDCARD_HW_SEED="${SD_QUAL_SEED:-0}")

# Put zephyr.crc, zephyr.bin's CRC-32 as the host computes it, on the boot
# partition (at 1 MB, scripts/mksdcard.sh), for suite 2 to compare with.
add_crc() {
	local bin=$1 img=$2
	python3 -I -c 'import sys, zlib; print("%08x" % zlib.crc32(open(sys.argv[1], "rb").read()))' \
		"$bin" >"$out/zephyr.crc"
	mcopy -o -i "$img@@1M" "$out/zephyr.crc" ::/zephyr.crc
	echo "zephyr.bin crc32: $(cat "$out/zephyr.crc")"
}

west build -p auto -b rpi_3a_plus -d "$out/rpi" "$app" -- "${conf[@]}"
"$here/scripts/mksdcard.sh" "$out/rpi" "$here/out/sd-qual-test"
add_crc "$out/rpi/zephyr/zephyr.bin" "$here/out/sd-qual-test/sdcard.img"
# The same file in boot/, for a card made by copying the files.
cp "$out/zephyr.crc" "$here/out/sd-qual-test/boot/zephyr.crc"

[[ ${1:-} == --qemu ]] || exit 0

west build -p auto -b rpi_3a_plus -d "$out/qemu" "$app" -- \
	-DEXTRA_DTC_OVERLAY_FILE="$here/qemu/raspi3ap.overlay" \
	-DCONFIG_SDCARD_HW_SOAK_SECONDS=20 -DCONFIG_SDCARD_HW_SCRATCH_MB=64 -DCONFIG_SDCARD_HW_ON_QEMU=y \
	-DCONFIG_SDCARD_HW_SEED="${SD_QUAL_SEED:-0}"
# A 256 MB card (QEMU wants a power of two) holding a boot partition with
# the QEMU build's image, so suite 2 has files to read.
"$here/scripts/mksdcard.sh" "$out/qemu" "$out/qemu-card" >/dev/null
cp "$out/qemu-card/sdcard.img" "$out/card.img"
add_crc "$out/qemu/zephyr/zephyr.bin" "$out/card.img"
truncate -s 256M "$out/card.img"
log=$out/qemu.log
"$here/scripts/run-qemu.sh" "$out/qemu" \
	-drive if=sd,format=raw,file="$out/card.img" </dev/null >"$log" 2>&1 &
qemu=$!
for _ in $(seq 1 900); do
	grep -qE "PROJECT EXECUTION (SUCCESSFUL|FAILED)|FATAL ERROR" "$log" && break
	sleep 1
done
kill "$qemu" 2>/dev/null || true
wait "$qemu" 2>/dev/null || true
grep -E "^ *(PASS|FAIL|SKIP) - |SUITE (PASS|FAIL)|PROJECT EXECUTION" "$log" || true
echo "QEMU log: $log"
grep -q "PROJECT EXECUTION SUCCESSFUL" "$log"
