#!/usr/bin/env bash
# Zephyr's own SD tests on the emulated Pi 3A+ (QEMU raspi3ap), with a
# scratch card image on the SD bus. Exercises the BCM283x SDHost driver
# (Zephyr PR #110196, applied by zephyr/patches.yml) through the rpi_3a_plus
# board node. Runs inside the Docker image.
#
#   tests/drivers/sdhc             host controller API (reset, set_io, ...)
#   tests/subsys/sd/sdmmc          SD stack: card init, block read/write
#   tests/drivers/disk/disk_access disk layer: read, write, erase
#
# QEMU models the SDHost registers, FIFO and card, but not the real
# controller's timing quirks: this is a functional check before hardware.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build/sd-test
zephyr=${ZEPHYR_BASE:?set ZEPHYR_BASE}
mkdir -p "$out"

# The Pi firmware's toolchain, Arm Toolchain for Embedded (cmake/toolchain.cmake,
# which Zephyr's own test apps don't include).
atfe=(-DZEPHYR_TOOLCHAIN_VARIANT=host/llvm "-DLLVM_TOOLCHAIN_PATH=${ATFE_PATH:-/opt/atfe-23.1.0}"
	"-DEXTRA_CONF_FILE=$here/cmake/atfe.conf")

tests=(
	"drivers/sdhc|-DCONFIG_SDHC=y"
	"subsys/sd/sdmmc|-DCONFIG_SDHC=y"
	"drivers/disk/disk_access|-DCONFIG_SDHC=y -DCONFIG_DISK_DRIVER_SDMMC=y"
)

failed=0
for entry in "${tests[@]}"; do
	t=${entry%%|*}
	read -r -a extra <<<"${entry#*|}"
	name=${t//\//_}
	build=$out/$name

	echo "=== $t"
	west build -p auto -b rpi_3a_plus -d "$build" "$zephyr/tests/$t" -- \
		-DEXTRA_DTC_OVERLAY_FILE="$here/qemu/raspi3ap.overlay" \
		"${atfe[@]}" "${extra[@]}" >"$out/$name.build.log" 2>&1 ||
		{ echo "FAIL $t: build (see $out/$name.build.log)"; failed=1; continue; }

	# A fresh 64 MB card per test (QEMU wants a power-of-two size).
	truncate -s 0 "$out/card.img"
	truncate -s 64M "$out/card.img"

	log=$out/$name.log
	"$here/scripts/run-qemu.sh" "$build" \
		-drive if=sd,format=raw,file="$out/card.img" </dev/null >"$log" 2>&1 &
	qemu=$!
	verdict=""
	for _ in $(seq 1 300); do
		if grep -q "PROJECT EXECUTION SUCCESSFUL" "$log"; then
			verdict=pass
			break
		fi
		if grep -qE "PROJECT EXECUTION FAILED|FATAL ERROR|ZEPHYR FATAL" "$log"; then
			verdict=fail
			break
		fi
		sleep 1
	done
	kill "$qemu" 2>/dev/null || true
	wait "$qemu" 2>/dev/null || true

	grep -E "^ *(PASS|FAIL|SKIP) - |SUITE (PASS|FAIL)" "$log" | sed 's/^/    /' || true
	case $verdict in
	pass) echo "PASS $t" ;;
	fail) echo "FAIL $t (see $log)"; failed=1 ;;
	*) echo "FAIL $t: timed out (see $log)"; failed=1 ;;
	esac
done

exit $failed
