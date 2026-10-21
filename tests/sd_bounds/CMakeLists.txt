# SPDX-License-Identifier: Apache-2.0
#
# Zephyr's SD stack range checks (subsys/sd/sd_ops.c), against a fake SD
# host controller: reads and writes past the card's end, including ranges
# whose end wraps past block 0xffffffff, must be refused before they reach
# the controller (zephyr/patches/zephyr/sd-ops-bounds-overflow.patch).

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sd_bounds)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/strict.cmake)
sd_strict_warnings(app)

target_sources(app PRIVATE src/main.c)
