/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 8: power cycles and re-initialisation through the disk API, and
 * the card staying usable after a command it doesn't answer.
 */

#include <zephyr/drivers/disk.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "common.h"

#define R_RECOVERY 120832U
#define CYCLES     20

BUILD_ASSERT(R_RECOVERY + 64U <= SDQ_SCRATCH_BLOCKS, "scratch area too small");

/** @brief 20 power-off / init cycles: each init succeeds and the data survives. */
ZTEST(sdcard_8_recovery, test_1_power_cycles)
{
	uint32_t lba = sdq.scratch + R_RECOVERY;
	uint64_t worst = 0;

	sdq_need_scratch();
	sdq_write_verify(lba, 64, 0x80000U);
	for (int i = 0; i < CYCLES; i++) {
		uint64_t t0;
		uint64_t us;

		zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_CTRL_DEINIT, NULL),
			   "cycle %d: power off", i);
		zassert_not_equal(disk_access_status(SDQ_DISK), DISK_STATUS_OK,
				  "cycle %d: status OK while off", i);
		t0 = sdq_now_us();
		zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_CTRL_INIT, NULL),
			   "cycle %d: init", i);
		us = sdq_now_us() - t0;
		worst = MAX(worst, us);
		zassert_equal(disk_access_status(SDQ_DISK), DISK_STATUS_OK, "cycle %d: status", i);
		zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba, 64), "cycle %d: read", i);
		zassert_equal(sdq_check(sdq_rbuf, lba, 64, 0x80000U), -1, "cycle %d: data", i);
	}
	TC_PRINT("%d power cycles; slowest card init %llu ms\n", CYCLES, worst / 1000U);
}

/** @brief Writing right after an init, then power-cycling at once, keeps the data. */
ZTEST(sdcard_8_recovery, test_2_write_then_power_off)
{
	uint32_t lba = sdq.scratch + R_RECOVERY;

	sdq_need_scratch();
	for (uint32_t i = 0; i < 5U; i++) {
		sdq_fill(sdq_wbuf, lba, 64, 0x80001U + i);
		zassert_ok(disk_access_write(SDQ_DISK, sdq_wbuf, lba, 64));
		/* DEINIT waits for the card to finish programming first. */
		zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_CTRL_DEINIT, NULL));
		zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_CTRL_INIT, NULL));
		zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba, 64));
		zassert_equal(sdq_check(sdq_rbuf, lba, 64, 0x80001U + i), -1, "round %u", i);
	}
}

/**
 * @brief The CID ioctl answers or fails cleanly, and the card works after.
 *
 * Zephyr's SD stack reads the CID with CMD2, which a card in transfer state
 * doesn't answer: the command times out. What matters for the driver is
 * that the timeout leaves it usable (review finding 2).
 */
ZTEST(sdcard_8_recovery, test_3_unanswered_command)
{
	uint32_t cid[4] = {0};
	int ret = disk_access_ioctl(SDQ_DISK, DISK_IOCTL_GET_CARD_CID, cid);

	if (ret == 0) {
		TC_PRINT("CID: manufacturer 0x%02x, OEM %c%c, product %c%c%c%c%c, rev %u.%u, "
			 "serial %08x, made %u/%u\n",
			 cid[3] >> 24, (char)(cid[3] >> 16), (char)(cid[3] >> 8), (char)cid[3],
			 (char)(cid[2] >> 24), (char)(cid[2] >> 16), (char)(cid[2] >> 8),
			 (char)cid[2], (cid[1] >> 28) & 0xfU, (cid[1] >> 24) & 0xfU,
			 ((cid[1] & 0xffffffU) << 8) | (cid[0] >> 24), (cid[0] >> 8) & 0xfU,
			 2000U + ((cid[0] >> 12) & 0xffU));
	} else {
		TC_PRINT("CID ioctl: %d (CMD2 is not answered in transfer state)\n", ret);
	}
	zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, 0, 8),
		   "the card no longer reads after the CID ioctl (%d): review finding 2", ret);
}

ZTEST_SUITE(sdcard_8_recovery, NULL, sdq_setup, sdq_before, sdq_after, NULL);
