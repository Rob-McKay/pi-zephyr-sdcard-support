/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 1: the card is found and initialised, and the controller runs the
 * bus as the SD specification allows.
 */

#include <zephyr/drivers/disk.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#include <hal/bcm2835_sdhost.h>

#include "common.h"

/* Default Speed allows 25 MHz, High Speed 50 MHz (SD Physical Layer
 * Specification, 4.3.10). The driver reports which it chose only through
 * the clock, so the test allows High Speed.
 */
#define HS_MAX_HZ 50000000U

/** @brief Skip a test that reads controller registers QEMU doesn't model. */
static void need_real_registers(void)
{
	if (IS_ENABLED(CONFIG_SDCARD_HW_ON_QEMU)) {
		TC_PRINT("QEMU doesn't model this register\n");
		ztest_test_skip();
	}
}

/** @brief The disk reports OK, 512-byte blocks and a plausible capacity. */
ZTEST(sdcard_1_identify, test_disk_ready)
{
	zassert_equal(disk_access_status(SDQ_DISK), DISK_STATUS_OK);
	zassert_equal(sdq.sector_size, SDQ_BLK, "block size %u", sdq.sector_size);
	/* Not the 64 MB card image's size: the whole card's. */
	zassert_true(sdq.sectors >= 131072U, "only %u blocks: is this the whole card?",
		     sdq.sectors);
}

/** @brief The SD bus is 4 bits wide, as every SD memory card supports. */
ZTEST(sdcard_1_identify, test_bus_4bit)
{
	need_real_registers();
	zassert_true(hal_sdhost_bus_is_4bit(sdq_sdhost_regs()),
		     "the driver left the bus 1 bit wide: a quarter of the speed");
}

/** @brief The bus clock, from the real core clock, is within the High Speed limit. */
ZTEST(sdcard_1_identify, test_bus_clock_in_spec)
{
	uint32_t core = sdq_core_clock_hz();
	uint32_t bus;

	need_real_registers();
	zassert_not_equal(core, 0, "firmware did not report the core clock");
	bus = core / (hal_sdhost_clock_divider(sdq_sdhost_regs()) + 2U);
	TC_PRINT("SD bus clock %u Hz\n", bus);
	zassert_true(bus <= HS_MAX_HZ, "bus clock %u Hz over 50 MHz (review finding 3)", bus);
	zassert_true(bus >= 400000U, "bus clock %u Hz: still at the identification rate", bus);
}

/** @brief The hardware data timeout lasts at least the 500 ms an SDXC write may take. */
ZTEST(sdcard_1_identify, test_data_timeout_covers_busy)
{
	uintptr_t regs = sdq_sdhost_regs();
	uint32_t core = sdq_core_clock_hz();
	uint32_t bus = core / (hal_sdhost_clock_divider(regs) + 2U);
	uint32_t cycles = hal_sdhost_data_timeout(regs);
	uint64_t ms = bus == 0U ? 0U : ((uint64_t)cycles * 1000U) / bus;

	need_real_registers();
	TC_PRINT("SDTOUT %u cycles = %llu ms at %u Hz\n", cycles, ms, bus);
	zassert_true(ms >= 500U, "data timeout %llu ms < 500 ms (review finding 4)", ms);
}

/** @brief The erase block size and sync ioctls answer. */
ZTEST(sdcard_1_identify, test_ioctls)
{
	uint32_t erase = 0;

	zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_GET_ERASE_BLOCK_SZ, &erase));
	zassert_true(erase >= 1U, "erase block size %u", erase);
	zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_CTRL_SYNC, NULL));
}

ZTEST_SUITE(sdcard_1_identify, NULL, sdq_setup, sdq_before, sdq_after, NULL);
