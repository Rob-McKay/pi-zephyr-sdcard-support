#!/usr/bin/env bash
# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
# Zephyr's own SD tests that PR #110196 reports passing on a Pi Zero 2 W,
# built for a real Raspberry Pi 3A+: one card image each. Runs inside the
# Docker image. See docs/TESTING.md.
#
#   tests/drivers/sdhc             host controller API: card presence, host
#                                  properties, reset, set_io, CMD8 (5 tests)
#   tests/drivers/disk/disk_access disk layer: reads, writes and erases of
#                                  1-31 blocks at the start, middle and end
#                                  of the card (3 tests)
#
#   scripts/sd-zephyr-hw-test.sh   writes out/sd-zephyr-hw-test/<test>/sdcard.img
#
# DESTRUCTIVE: disk_access writes and erases the card's first block (the
# partition table), its middle and its last block. Use a spare card, and
# flash it again before each run: after disk_access the card no longer
# boots. RPI_CORE_FREQ sets the core clock as for the other SD tests.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build/sd-zephyr-hw-test
zephyr=${ZEPHYR_BASE:?set ZEPHYR_BASE}

# The Pi firmware's toolchain, Arm Toolchain for Embedded (cmake/toolchain.cmake,
# which Zephyr's own test apps don't include).
atfe=(-DZEPHYR_TOOLCHAIN_VARIANT=host/llvm "-DLLVM_TOOLCHAIN_PATH=${ATFE_PATH:-/opt/atfe-23.1.0}"
	"-DEXTRA_CONF_FILE=$here/cmake/atfe.conf")

tests=(
	"drivers/sdhc|-DCONFIG_SDHC=y"
	"drivers/disk/disk_access|-DCONFIG_SDHC=y -DCONFIG_DISK_DRIVER_SDMMC=y"
)

for entry in "${tests[@]}"; do
	t=${entry%%|*}
	read -r -a extra <<<"${entry#*|}"
	name=${t//\//_}

	echo "=== $t"
	west build -p auto -b rpi_3a_plus -d "$out/$name" "$zephyr/tests/$t" -- \
		"${atfe[@]}" "${extra[@]}"
	"$here/scripts/mksdcard.sh" "$out/$name" "$here/out/sd-zephyr-hw-test/$name"
done

echo
echo "Card images (flash one, boot, log the console; flash again before each run):"
for entry in "${tests[@]}"; do
	t=${entry%%|*}
	echo "  out/sd-zephyr-hw-test/${t//\//_}/sdcard.img"
done
