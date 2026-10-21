/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 5: erase (CMD32/33/38). Erased blocks read as all 0x00 or all
 * 0xff (the card chooses), their neighbours keep their data, and a large
 * erase, which keeps the card busy for a long time, completes. Uses the
 * scratch area.
 */

#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#include "common.h"

#define R_SMALL 65536U
#define R_LARGE 81920U
#define LARGE_BLOCKS 16384U /* 8 MB */

BUILD_ASSERT(R_LARGE + LARGE_BLOCKS <= SDQ_SCRATCH_BLOCKS, "scratch area too small");

/**
 * @brief Check that blocks are erased, all to the same value.
 *
 * @param lba First block.
 * @param blocks Block count, at most SDQ_MAX_BLOCKS.
 * @param[in,out] value The erased value seen so far, or -1 for none yet.
 */
static void expect_erased(uint32_t lba, uint32_t blocks, int *value)
{
	zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba, blocks));
	for (uint32_t b = 0; b < blocks; b++) {
		const uint8_t *p = &sdq_rbuf[(size_t)b * SDQ_BLK];

		if (*value < 0) {
			*value = p[0];
		}
		zassert_true((*value == 0x00 || *value == 0xff) && sdq_block_is(p, (uint8_t)*value),
			     "block %u not erased (first byte 0x%02x)", lba + b, p[0]);
	}
}

/** @brief An 8-block erase in the middle of written data erases exactly those blocks. */
ZTEST(sdcard_5_erase, test_1_small_erase_exact)
{
	uint32_t lba = sdq.scratch + R_SMALL;
	int value = -1;

	sdq_need_scratch();
	sdq_write_verify(lba, 64, 0x50000U);
	zassert_ok(disk_access_erase(SDQ_DISK, lba + 8U, 8, DISK_ACCESS_ERASE_PHYSICAL));
	expect_erased(lba + 8U, 8, &value);
	TC_PRINT("erased blocks read as 0x%02x\n", value);
	zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba, 64));
	zassert_equal(sdq_check(sdq_rbuf, lba, 8, 0x50000U), -1, "blocks before the erase");
	zassert_equal(sdq_check(&sdq_rbuf[(size_t)16U * SDQ_BLK], lba + 16U, 48, 0x50000U), -1,
		      "blocks after the erase");
}

/** @brief A single-block erase, and a write to it afterwards, work. */
ZTEST(sdcard_5_erase, test_2_single_block_then_write)
{
	uint32_t lba = sdq.scratch + R_SMALL + 100U;
	int value = -1;

	sdq_need_scratch();
	sdq_write_verify(lba, 1, 0x50001U);
	zassert_ok(disk_access_erase(SDQ_DISK, lba, 1, DISK_ACCESS_ERASE_PHYSICAL));
	expect_erased(lba, 1, &value);
	sdq_write_verify(lba, 1, 0x50002U);
}

/** @brief An 8 MB erase (long busy on DAT0: review finding 1) completes, and the card works after. */
ZTEST(sdcard_5_erase, test_3_large_erase)
{
	uint32_t lba = sdq.scratch + R_LARGE;
	uint64_t t0;
	int value = -1;
	int ret;

	sdq_need_scratch();
	for (uint32_t b = 0; b < LARGE_BLOCKS; b += SDQ_MAX_BLOCKS) {
		sdq_fill(sdq_wbuf, lba + b, SDQ_MAX_BLOCKS, 0x50003U);
		zassert_ok(disk_access_write(SDQ_DISK, sdq_wbuf, lba + b, SDQ_MAX_BLOCKS));
	}
	t0 = sdq_now_us();
	ret = disk_access_erase(SDQ_DISK, lba, LARGE_BLOCKS, DISK_ACCESS_ERASE_PHYSICAL);
	TC_PRINT("8 MB erase: %d after %llu ms\n", ret, (sdq_now_us() - t0) / 1000U);
	zassert_ok(ret, "8 MB erase failed: R1b busy wait too short? (review finding 1)");
	for (uint32_t b = 0; b < LARGE_BLOCKS; b += SDQ_MAX_BLOCKS) {
		expect_erased(lba + b, SDQ_MAX_BLOCKS, &value);
	}
	sdq_write_verify(lba + LARGE_BLOCKS - 1U, 1, 0x50004U);
}

ZTEST_SUITE(sdcard_5_erase, NULL, sdq_setup, sdq_before, sdq_after, NULL);
