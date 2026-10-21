# SPDX-License-Identifier: Apache-2.0
#
# SD card qualification on a real Raspberry Pi 3A+: the SDHost driver from
# Zephyr PR #110196 (applied by zephyr/patches.yml) driven through Zephyr's
# disk access API, as an application or file system would use it. Build
# and package with scripts/sd-qual-test.sh ("make sd-qual-test").

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sdcard_hw)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/strict.cmake)
sd_strict_warnings(app)

if(NOT TARGET drivers__sdhc)
  message(FATAL_ERROR "no SDHC driver library: is zephyr/patches.yml applied ('west patch apply')?")
endif()

target_sources(app PRIVATE
  src/common.c
  src/test_1_identify.c
  src/test_2_bootfs.c
  src/test_3_integrity.c
  src/test_4_bounds.c
  src/test_5_erase.c
  src/test_6_perf.c
  src/test_7_concurrency.c
  src/test_8_recovery.c
  src/test_9_soak.c
  ../sdhost_review/src/ubsan.c
)
target_include_directories(app PRIVATE ../sdhost_review/src ../../hal/include)

# Catch signed overflow in the driver as built into the image; ubsan.c
# records where it happened, and every test checks the count.
target_compile_options(drivers__sdhc PRIVATE -fsanitize=signed-integer-overflow)
