# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
#
# Reproduces the findings of docs/reviews/pr110196-sdhost-review.md: the
# PR's SDHost driver (applied to Zephyr by zephyr/patches.yml) and the
# BCM2711 pinctrl driver are compiled against fake register models.

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sdhost_review)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/strict.cmake)
sd_strict_warnings(app)

set(sdhost_c ${ZEPHYR_BASE}/drivers/sdhc/sdhc_bcm2835_sdhost.c)
if(NOT EXISTS ${sdhost_c})
  message(FATAL_ERROR "${sdhost_c} not found: run 'west patch apply' (zephyr/patches.yml)")
endif()

target_sources(app PRIVATE src/sdhost.c src/pinctrl.c src/ubsan.c)
# Upstream sources #included by the tests: SYSTEM keeps them out of our
# warning policy.
target_include_directories(app SYSTEM PRIVATE
  ${ZEPHYR_BASE}/drivers/sdhc
  ${ZEPHYR_BASE}/drivers/pinctrl
  ${ZEPHYR_BASE}/soc/brcm/common)

# Finding 5: catch signed overflow in the driver. Handlers in ubsan.c record
# where it happened, so no sanitizer runtime is linked.
set_source_files_properties(src/sdhost.c PROPERTIES
  COMPILE_OPTIONS "-fsanitize=signed-integer-overflow")
