# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the hardware abstraction layer (hal/include/hal): each
# accessor runs against a RAM-backed register block.

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_hal_bcm2835)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/strict.cmake)
sd_strict_warnings(app)

target_include_directories(app PRIVATE ../../hal/include)
target_sources(app PRIVATE src/main.c)
