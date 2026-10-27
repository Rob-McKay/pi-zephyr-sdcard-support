#!/usr/bin/env bash
# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
# Assemble the SD card boot files (and a ready-to-flash image) from a build.
#
#   scripts/mksdcard.sh <build dir> <output dir>
#
# Output:
#   <out>/boot/      copy these to the FAT32 first partition of a microSD card
#   <out>/sdcard.img 64 MB image with that partition, for dd / Raspberry Pi Imager
set -euo pipefail

build_dir=$1
out=$2
fw=${RPI_FIRMWARE_DIR:-/opt/rpi-firmware}

rm -rf "$out"
mkdir -p "$out/boot"

# The cut-down GPU firmware: gpu_mem=16 (below) needs it, and the tests
# use none of the full firmware's video or camera features.
cp "$fw"/bootcode.bin "$fw"/start_cd.elf "$fw"/fixup_cd.dat "$fw"/LICENCE.broadcom "$out/boot/"
cp "$build_dir/zephyr/zephyr.bin" "$out/boot/zephyr.bin"

# RPI_CORE_FREQ overrides the VPU core clock (MHz); the SD driver tests use
# it (scripts/sd-hw-test.sh). The console baud rate scales with it.
core_freq=${RPI_CORE_FREQ:-250}
# kernel_address must match the dram0 base in the board devicetree.
cat > "$out/boot/config.txt" <<CFG
# Zephyr SD card tests - Raspberry Pi 3 Model A+
arm_64bit=1
kernel=zephyr.bin
kernel_address=0x200000
# Mini UART console on GPIO 14/15; lock the core clock it
# derives its baud rate from.
enable_uart=1
core_freq=$core_freq
core_freq_min=$core_freq
# The GPU firmware's boot log on the same UART, before Zephyr starts.
uart_2ndstage=1
# Keep the GPU's share of RAM small: 16 MB needs the cut-down firmware.
gpu_mem=16
start_file=start_cd.elf
fixup_file=fixup_cd.dat
CFG

# FAT32 partition image, then a partitioned card image around it.
part="$out/boot.fat"
rm -f "$part"
mkfs.vfat -C -F 32 -n SDTESTBOOT "$part" $((62 * 1024)) >/dev/null
mcopy -i "$part" "$out"/boot/* ::/

img="$out/sdcard.img"
dd if=/dev/zero of="$img" bs=1M count=64 status=none
echo 'start=2048, type=c, bootable' | sfdisk -q "$img"
dd if="$part" of="$img" bs=512 seek=2048 conv=notrunc status=none
rm -f "$part"

echo "SD card files: $out/boot"
echo "SD card image: $img"
true
