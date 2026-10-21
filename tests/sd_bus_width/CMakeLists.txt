# SPDX-License-Identifier: Apache-2.0
#
# Zephyr's SD stack bus width choice (subsys/sd/sdmmc.c), against a fake
# card on a fake SD host controller: a card is switched to a 4-bit bus
# whether or not it supports High Speed
# (zephyr/patches/zephyr/sdmmc-4bit-without-hs.patch).

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sd_bus_width)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/strict.cmake)
sd_strict_warnings(app)

target_sources(app PRIVATE src/main.c)
