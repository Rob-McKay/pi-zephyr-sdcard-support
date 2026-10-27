# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
#
# Reproduces the findings of docs/reviews/pr110196-sdhost-review.md on a
# real Raspberry Pi 3A+ with a real SD card: the PR's SDHost driver
# (applied to Zephyr by zephyr/patches.yml) runs unmodified against the
# hardware. Build and package with scripts/sd-hw-test.sh.

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sdhost_hw)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/strict.cmake)
sd_strict_warnings(app)

if(NOT TARGET drivers__sdhc)
  message(FATAL_ERROR "no SDHC driver library: is zephyr/patches.yml applied ('west patch apply')?")
endif()

target_sources(app PRIVATE src/main.c ../sdhost_review/src/ubsan.c)
target_include_directories(app PRIVATE ../sdhost_review/src ../../hal/include)

# Finding 5: catch signed overflow in the driver as built into the image.
# The handlers in ubsan.c record where it happened; no runtime is linked.
target_compile_options(drivers__sdhc PRIVATE -fsanitize=signed-integer-overflow)
