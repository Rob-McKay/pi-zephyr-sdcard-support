# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
# Host-side convenience wrapper: everything runs in the Docker image.
#
#   make image                 build the Docker image for this machine
#                              (arm64 on Apple Silicon, amd64 on PCs)
#   make image-multiarch       build linux/amd64 + linux/arm64 with buildx
#                              (needs a registry: PUSH_TO=registry/name:tag)
#   make test                  the tests that need no hardware: the driver
#                              against a model of the controller, the HAL,
#                              the documentation check
#   make sd-test               Zephyr's SD card tests on the emulated Pi 3A+
#   make sd-qual-test          SD card qualification for a real Pi 3A+
#                              (docs/TESTING.md): writes
#                              out/sd-qual-test/sdcard.img (spare card!);
#                              SD_QUAL_TEST_FLAGS=--qemu adds a QEMU smoke run,
#                              SD_QUAL_SOAK_SECONDS, SD_QUAL_SCRATCH_MB and
#                              SD_QUAL_SEED change the run
#   make sd-zephyr-hw-test     Zephyr's sdhc and disk_access tests (as PR
#                              #110196 reports them) for a real Pi 3A+: a card
#                              image each in out/sd-zephyr-hw-test/ (spare
#                              card; disk_access overwrites the partition table)
#   make sd-hw-test            the code review's finding tests for a real
#                              Pi 3A+: writes out/sd-hw-test/sdcard.img (spare
#                              card!); SD_HW_TEST_FLAGS=--qemu adds a QEMU
#                              smoke run
#   make shell                 interactive shell in the build container
#
# RPI_CORE_FREQ=400 builds the card images for a different VPU core clock.

IMAGE ?= pi-zephyr-sdcard-support

# Optional: CA certificate of a TLS-intercepting proxy, passed as a build
# secret, and extra docker build flags (e.g. --network host, --build-arg).
EXTRA_CA ?=
DOCKER_BUILD_FLAGS ?=
comma := ,
BUILD_FLAGS = $(DOCKER_BUILD_FLAGS) $(if $(EXTRA_CA),--secret id=extra_ca$(comma)src=$(EXTRA_CA))
# Interactive by default; DOCKER_TTY= for scripted/CI use.
DOCKER_TTY ?= -it
DOCKER_RUN = docker run --rm $(DOCKER_TTY) -v $(CURDIR):/workdir/sdcard $(IMAGE)

.PHONY: image image-multiarch test sd-test sd-qual-test sd-zephyr-hw-test sd-hw-test \
	shell clean

image:
	DOCKER_BUILDKIT=1 docker build $(BUILD_FLAGS) -t $(IMAGE) .

image-multiarch:
	@test -n "$(PUSH_TO)" || { echo "set PUSH_TO=registry/name:tag"; exit 1; }
	docker buildx build $(BUILD_FLAGS) --platform linux/amd64,linux/arm64 \
		-t $(PUSH_TO) --push .

test:
	$(DOCKER_RUN) scripts/test.sh

sd-test:
	$(DOCKER_RUN) scripts/sd-test.sh

sd-qual-test:
	$(DOCKER_RUN) env RPI_CORE_FREQ=$(RPI_CORE_FREQ) \
		SD_QUAL_SOAK_SECONDS=$(SD_QUAL_SOAK_SECONDS) SD_QUAL_SCRATCH_MB=$(SD_QUAL_SCRATCH_MB) \
		SD_QUAL_SEED=$(SD_QUAL_SEED) scripts/sd-qual-test.sh $(SD_QUAL_TEST_FLAGS)

sd-zephyr-hw-test:
	$(DOCKER_RUN) env RPI_CORE_FREQ=$(RPI_CORE_FREQ) scripts/sd-zephyr-hw-test.sh

sd-hw-test:
	$(DOCKER_RUN) env RPI_CORE_FREQ=$(RPI_CORE_FREQ) scripts/sd-hw-test.sh $(SD_HW_TEST_FLAGS)

shell:
	$(DOCKER_RUN) bash

clean:
	rm -rf build out
