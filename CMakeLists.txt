# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr module build glue: this repository provides the rpi_3a_plus board
# and the build policy its tests use.

include(${CMAKE_CURRENT_LIST_DIR}/cmake/strict.cmake)

# Run-time buffer overflow checks in the C library's memory and string
# functions (CONFIG_FORTIFY_SOURCE_RUN_TIME, which cmake/atfe.conf selects).
# Zephyr gives GCC _FORTIFY_SOURCE=2 but nothing for clang, although clang
# and picolibc support it; a failed check calls Zephyr's __chk_fail(), a
# fatal error.
if(CONFIG_FORTIFY_SOURCE_RUN_TIME AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set_compiler_property(PROPERTY security_fortify_run_time _FORTIFY_SOURCE=2)
endif()
