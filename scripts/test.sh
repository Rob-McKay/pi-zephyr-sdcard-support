#!/usr/bin/env bash
# The tests that need no hardware, on native_sim (64-bit host builds). Runs
# inside the Docker image.
#
#   tests/sdhost_review  the SDHost driver of Zephyr PR #110196, with the
#                        carried fix, against a model of the controller and
#                        card: the review's findings (expected failures where
#                        still open) and sanity tests
#   tests/sd_bounds      Zephyr's SD stack range checks against a fake
#                        controller: ranges past the end, including ones
#                        that wrap past block 0xffffffff, are refused
#   tests/hal_bcm2835    the hardware abstraction layer (hal/) against
#                        RAM-backed registers
#
# The HAL must be fully unit tested: scripts/check-coverage.py fails the run
# unless the tests execute every line of it. Every build is strict (no
# warnings, clean clang-tidy: cmake/strict.cmake).
#
# First, scripts/check-docs.py checks that every function has a Doxygen
# comment, and a search checks that only the HAL touches registers.
#
# The hardware tests are built by the sd-*.sh scripts.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)

"$here/scripts/check-docs.py"

# Only the hardware abstraction layer (hal/) touches registers.
if grep -rnE '\bsys_(read|write)(8|16|32|64)\(' "$here/tests/sdhost_hw/src" \
	"$here/tests/sdcard_hw/src"; then
	echo "Register access outside hal/: use a HAL function (docs/CODING.md)"
	exit 1
fi

for t in sdhost_review sd_bounds hal_bcm2835; do
	build_dir="$here/build/test-$t"
	west build -p auto -b native_sim/native/64 -d "$build_dir" "$here/tests/$t"
	# Coverage counts accumulate across runs: start from zero.
	find "$build_dir" -name '*.gcda' -delete
	"$build_dir/zephyr/zephyr.exe"
done

"$here/scripts/check-coverage.py" "$here/build/test-hal_bcm2835" 'hal/include/hal/bcm2835_*.h'
