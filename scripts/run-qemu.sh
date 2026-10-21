#!/usr/bin/env bash
# Run a Raspberry Pi 3A+ build on QEMU's emulated Pi 3A+ (raspi3ap).
#
#   scripts/run-qemu.sh <build dir> [extra QEMU arguments...]
#
# The mini UART console is this terminal. The SD test scripts add the card
# image with "-drive if=sd,format=raw,file=...". Quit with Ctrl-a x.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
build=${1:?usage: run-qemu.sh <build dir> [qemu args...]}

sdk=${ZEPHYR_SDK_INSTALL_DIR:?set ZEPHYR_SDK_INSTALL_DIR}
qemu=$(ls "$sdk"/hosttools/sysroots/*/usr/bin/qemu-system-aarch64 | head -1)
# The firmware's toolchain, Arm Toolchain for Embedded (cmake/toolchain.cmake).
atfe=${ATFE_PATH:-/opt/atfe-23.1.0}/bin

# Boot stub standing in for the GPU firmware's (see qemu/raspi3-armstub.S).
entry=$("$atfe"/llvm-readelf -h "$build/zephyr/zephyr.elf" | awk '/Entry point/ {print $4}')
stub=$build/armstub
"$atfe"/llvm-mc -triple=aarch64-none-elf -filetype=obj --defsym ZEPHYR_ENTRY="$entry" \
	-o "$stub.o" "$here/qemu/raspi3-armstub.S"
"$atfe"/ld.lld --image-base=0 -Ttext=0 -o "$stub.elf" "$stub.o"
"$atfe"/llvm-objcopy -O binary "$stub.elf" "$stub.bin"

exec "$qemu" -M raspi3ap -display none -monitor none \
	-serial null -serial mon:stdio \
	-device loader,file="$stub.bin",addr=0x0,force-raw=on \
	-device loader,file="$build/zephyr/zephyr.elf" \
	"${@:2}"
