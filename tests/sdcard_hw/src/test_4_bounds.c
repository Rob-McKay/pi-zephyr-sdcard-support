/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 4: the ends of the card. The last blocks read and write; requests
 * past the end are refused without disturbing the card.
 */

#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#include "common.h"

/** @brief The first blocks of the card (MBR, partition start) read, twice the same. */
ZTEST(sdcard_4_bounds, test_1_first_blocks)
{
	zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, 0, 64));
	zassert_ok(disk_access_read(SDQ_DISK, sdq_wbuf, 0, 64));
	zassert_mem_equal(sdq_rbuf, sdq_wbuf, (size_t)64U * SDQ_BLK, "two reads of blocks 0-63 differ");
}

/** @brief The very last block, and the last 64 blocks, read and write. */
ZTEST(sdcard_4_bounds, test_2_last_blocks)
{
	sdq_need_scratch();
	sdq_write_verify(sdq.sectors - 1U, 1, 0x40000U);
	sdq_write_verify(sdq.sectors - 64U, 64, 0x40001U);
}

/** @brief Reads and writes that run past the end are refused (-EINVAL) before reaching the card. */
ZTEST(sdcard_4_bounds, test_3_past_the_end_refused)
{
	zassert_equal(disk_access_read(SDQ_DISK, sdq_rbuf, sdq.sectors, 1), -EINVAL,
		      "read of the block after the last");
	zassert_equal(disk_access_read(SDQ_DISK, sdq_rbuf, sdq.sectors - 1U, 2), -EINVAL,
		      "read across the end");
	if (sdq.scratch_ok) {
		zassert_equal(disk_access_write(SDQ_DISK, sdq_wbuf, sdq.sectors - 1U, 2), -EINVAL,
			      "write across the end");
	}
	zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, sdq.sectors - 1U, 1),
		   "the last block no longer reads after the refused requests");
}

/**
 * @brief A range whose end wraps past block 0xffffffff is refused (-EINVAL)
 *        before reaching the card.
 *
 * Zephyr's SD stack checks reads and writes with start + count >
 * block_count (subsys/sd/sd_ops.c), which wraps to a small number for
 * block 0xffffffff, so the request goes to the card and only the card's
 * error stops it. Its erase check is overflow-aware; reads and writes
 * aren't. Not the SDHost driver's bug, but a write could wrap the same way.
 * Marked as an expected failure: when Zephyr fixes the check it passes,
 * which fails the suite, as a reminder to remove the marker.
 */
ZTEST_EXPECT_FAIL(sdcard_4_bounds, test_4_wrapping_range_refused);
ZTEST(sdcard_4_bounds, test_4_wrapping_range_refused)
{
	int ret = disk_access_read(SDQ_DISK, sdq_rbuf, UINT32_MAX, 1);

	zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, sdq.sectors - 1U, 1),
		   "the last block no longer reads after a read of block 0xffffffff (%d)", ret);
	zassert_equal(ret, -EINVAL,
		      "read of block 0xffffffff went to the card (%d): the SD stack's bounds "
		      "check wrapped",
		      ret);
}

/** @brief Reading the whole card's address range in 1 MB steps (a block each) works. */
ZTEST(sdcard_4_bounds, test_5_whole_range_sampled)
{
	for (uint32_t lba = 0; lba < sdq.sectors; lba += 2048U) {
		zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba, 1), "block %u", lba);
	}
}

ZTEST_SUITE(sdcard_4_bounds, NULL, sdq_setup, sdq_before, sdq_after, NULL);
