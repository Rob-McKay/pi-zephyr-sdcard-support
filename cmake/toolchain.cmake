# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
#
# Choose the compiler: clang everywhere. Include before find_package(Zephyr).
#
#   rpi_3a_plus  Arm Toolchain for Embedded (ATfE), Arm's LLVM-based
#                bare-metal toolchain, pinned to SD_ATFE_VERSION, with
#                cmake/atfe.conf (lld, compiler-rt, _FORTIFY_SOURCE).
#   native_sim   the host's clang (the unit tests; ATfE has no Linux
#                runtimes for AddressSanitizer or coverage).
#
# Zephyr's variant for both is host/llvm, so a toolchain chosen explicitly
# (ZEPHYR_TOOLCHAIN_VARIANT as a CMake or environment variable) other than
# that wins, and other boards keep Zephyr's default.
#
# ATfE is found at LLVM_TOOLCHAIN_PATH, else $ATFE_PATH, else
# /opt/atfe-<version> (where the Docker image installs it).

set(SD_ATFE_VERSION 23.1.0)

if(BOARD MATCHES "^rpi_3a_plus")
  set(sd_toolchain atfe)
elseif(BOARD MATCHES "^native_sim")
  set(sd_toolchain host_clang)
else()
  return()
endif()

if(DEFINED ENV{ZEPHYR_TOOLCHAIN_VARIANT} AND NOT ZEPHYR_TOOLCHAIN_VARIANT)
  set(ZEPHYR_TOOLCHAIN_VARIANT $ENV{ZEPHYR_TOOLCHAIN_VARIANT})
endif()
# Zephyr caches host/llvm as "host" plus TOOLCHAIN_VARIANT_COMPILER "llvm":
# when CMake re-runs in an existing build directory, that is still ours.
if(ZEPHYR_TOOLCHAIN_VARIANT STREQUAL "host" AND TOOLCHAIN_VARIANT_COMPILER STREQUAL "llvm")
  set(ZEPHYR_TOOLCHAIN_VARIANT host/llvm)
endif()
if(ZEPHYR_TOOLCHAIN_VARIANT AND NOT ZEPHYR_TOOLCHAIN_VARIANT STREQUAL "host/llvm")
  message(STATUS "Toolchain ${ZEPHYR_TOOLCHAIN_VARIANT} chosen explicitly")
  return()
endif()
set(ZEPHYR_TOOLCHAIN_VARIANT host/llvm)

if(sd_toolchain STREQUAL "host_clang")
  find_program(SD_HOST_CLANG clang)
  if(NOT SD_HOST_CLANG)
    message(FATAL_ERROR "clang not found: install it (apt install clang libclang-rt-18-dev llvm)")
  endif()
  message(STATUS "Toolchain: host clang (${SD_HOST_CLANG})")
  return()
endif()

if(NOT LLVM_TOOLCHAIN_PATH)
  if(DEFINED ENV{ATFE_PATH})
    set(LLVM_TOOLCHAIN_PATH $ENV{ATFE_PATH})
  else()
    set(LLVM_TOOLCHAIN_PATH /opt/atfe-${SD_ATFE_VERSION})
  endif()
endif()
# Cached, because Zephyr's host/llvm toolchain otherwise caches an empty
# LLVM_TOOLCHAIN_PATH, which a re-run of CMake would then use.
set(LLVM_TOOLCHAIN_PATH ${LLVM_TOOLCHAIN_PATH} CACHE PATH "Arm Toolchain for Embedded" FORCE)

execute_process(COMMAND ${LLVM_TOOLCHAIN_PATH}/bin/clang --version
  OUTPUT_VARIABLE atfe_version_text ERROR_QUIET RESULT_VARIABLE atfe_result)
if(NOT atfe_result EQUAL 0 OR NOT atfe_version_text MATCHES "clang version ${SD_ATFE_VERSION}")
  message(FATAL_ERROR "Arm Toolchain for Embedded ${SD_ATFE_VERSION} not found at "
    "${LLVM_TOOLCHAIN_PATH}: install it there, set ATFE_PATH, or choose another "
    "toolchain with -DZEPHYR_TOOLCHAIN_VARIANT=...")
endif()

list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/atfe.conf)
message(STATUS "Toolchain: Arm Toolchain for Embedded ${SD_ATFE_VERSION} (${LLVM_TOOLCHAIN_PATH})")
