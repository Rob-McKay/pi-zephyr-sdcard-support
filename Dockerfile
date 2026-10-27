# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
# Build environment for the BCM283x SD card tests (Raspberry Pi 3 Model A+).
#
# Contains Arm Toolchain for Embedded (the Pi firmware's compiler), the
# host's clang (the native_sim unit tests), the Zephyr SDK's host tools
# (QEMU) and a west workspace with Zephyr pinned by west.yml. There is no
# GCC: every build uses clang (cmake/toolchain.cmake). Mount this repository at
# /workdir/sdcard and use the scripts in scripts/ (or `make` on the host).
# The Dockerfile applies this repository's west.yml and patches.
#
# Multi-arch: builds natively for linux/amd64 (PCs, CI) and linux/arm64
# (Apple Silicon Macs, Arm Linux hosts); the matching Zephyr SDK and ATfE
# host builds are picked from TARGETARCH.
#
# Behind a TLS-intercepting proxy, pass its CA certificate as a build secret
# (docker build --secret id=extra_ca,src=proxy-ca.crt ...) along with the
# usual HTTP(S)_PROXY build args; see `make image EXTRA_CA=...`.

ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ARG TARGETARCH
ARG ZEPHYR_SDK_VERSION=1.0.1
ARG RPI_FIRMWARE_TAG=1.20260915

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# - clang, libclang-rt-18-dev, llvm: the native_sim unit tests (compiler,
#   AddressSanitizer and coverage runtimes, llvm-cov for the coverage check)
# - device-tree-compiler: Zephyr's extra devicetree checks
# - dosfstools, mtools, fdisk: the SD card image (scripts/mksdcard.sh)
# - doxygen: the documentation check
# - file: the Zephyr SDK's host tools installer (relocating QEMU)
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget git xz-utils file make cmake ninja-build \
        device-tree-compiler python3 python3-venv \
        clang libclang-rt-18-dev llvm \
        dosfstools mtools fdisk doxygen \
    && rm -rf /var/lib/apt/lists/*

# Optional extra CA certificate (see top of file). git, wget and curl use the
# system store; point pip/requests at it as well.
RUN --mount=type=secret,id=extra_ca,required=false \
    if [ -s /run/secrets/extra_ca ]; then \
        cp /run/secrets/extra_ca /usr/local/share/ca-certificates/extra_ca.crt \
        && update-ca-certificates; \
    fi
ENV PIP_CERT=/etc/ssl/certs/ca-certificates.crt \
    REQUESTS_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt

# Python tooling in a venv (Ubuntu 24.04 forbids system-wide pip installs).
# clang-tidy comes from PyPI: the build needs 20 or later (.clang-tidy's
# banned-function list), and Ubuntu 24.04 ships 18. lizard measures
# function size (scripts/check-size.py); libclang, matching the host
# clang 18 whose built-in headers it uses, reads the code for
# scripts/check-globals.py and scripts/check-macro-args.py.
ENV VIRTUAL_ENV=/opt/venv \
    PATH=/opt/venv/bin:$PATH
RUN python3 -m venv $VIRTUAL_ENV && pip install --no-cache-dir west clang-tidy==23.1.3 lizard==1.24.3 \
    libclang==18.1.1

# Zephyr SDK for this host architecture: its host tools (QEMU) only, no
# toolchains.
RUN case "${TARGETARCH:-$(dpkg --print-architecture)}" in \
        amd64) sdk_host=linux-x86_64 ;; \
        arm64) sdk_host=linux-aarch64 ;; \
        *) echo "unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && sdk=zephyr-sdk-${ZEPHYR_SDK_VERSION}_${sdk_host}_minimal.tar.xz \
    && cd /opt \
    && wget -q https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/${sdk} \
    && tar xf ${sdk} \
    && rm ${sdk} \
    && cd zephyr-sdk-${ZEPHYR_SDK_VERSION} \
    && ./setup.sh -c -h
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}

# Arm Toolchain for Embedded (LLVM, picolibc) builds the Pi firmware
# (cmake/toolchain.cmake). Checksums from the release's .sha256 files.
ARG ATFE_VERSION=23.1.0
RUN case "${TARGETARCH:-$(dpkg --print-architecture)}" in \
        amd64) atfe_host=x86_64 \
               atfe_sha=a7be511613af15151c93961ad39c8cf9ae6889a453c8b547f42f4051a416dc8e ;; \
        arm64) atfe_host=AArch64 \
               atfe_sha=df3b055d05659d8fda710d820e6c1ceff7bca3234c5067abe82a16e781d8de99 ;; \
        *) echo "unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && atfe=ATfE-${ATFE_VERSION}-Linux-${atfe_host} \
    && cd /opt \
    && wget -q https://github.com/arm/arm-toolchain/releases/download/release-${ATFE_VERSION}-ATfE/${atfe}.tar.xz \
    && echo "${atfe_sha}  ${atfe}.tar.xz" | sha256sum -c - \
    && tar xf ${atfe}.tar.xz \
    && rm ${atfe}.tar.xz \
    && mv ${atfe} atfe-${ATFE_VERSION}
ENV ATFE_PATH=/opt/atfe-${ATFE_VERSION}

# West workspace. Only west.yml is copied so that source edits don't
# invalidate this (slow) layer; the full repo is mounted at run time.
WORKDIR /workdir
COPY west.yml /workdir/sdcard/west.yml
RUN git config --global --add safe.directory '*' \
    && west init -l sdcard \
    && west update --narrow --fetch-opt=--depth=1 \
    && pip install --no-cache-dir -r zephyr/scripts/requirements-base.txt \
    && west zephyr-export
# Ubuntu's ccache (4.9) is older than Zephyr's minimum (4.12), so Zephyr
# would only report it unsuitable: tell it not to look.
ENV ZEPHYR_BASE=/workdir/zephyr \
    USE_CCACHE=0

# Patches carried until they are merged upstream (zephyr/patches.yml).
# A layer of its own, so changing a patch doesn't re-fetch the workspace.
COPY zephyr/patches.yml /workdir/sdcard/zephyr/patches.yml
COPY zephyr/patches /workdir/sdcard/zephyr/patches
RUN west patch apply

# Raspberry Pi GPU firmware for the SD card boot partition.
RUN mkdir -p /opt/rpi-firmware && cd /opt/rpi-firmware \
    && for f in bootcode.bin start_cd.elf fixup_cd.dat LICENCE.broadcom; do \
         wget -q https://raw.githubusercontent.com/raspberrypi/firmware/${RPI_FIRMWARE_TAG}/boot/$f || exit 1; \
       done

WORKDIR /workdir/sdcard
CMD ["/bin/bash"]
