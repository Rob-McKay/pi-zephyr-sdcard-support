/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 9: soak. For CONFIG_SDCARD_HW_SOAK_SECONDS, random writes, reads
 * and erases of random sizes anywhere in the scratch area, every read
 * checked against a shadow of what each block should hold; then the whole
 * scratch area is read back and checked. Progress is printed every 30 s.
 */

#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#include "common.h"

/* Shadow values: unknown (not written in this suite), erased, or a generation. */
#define UNKNOWN 0U
#define ERASED  0xffffU

#define REPORT_US ((uint64_t)30U * 1000000U)

/** Counters for the progress report. */
struct soak_counts {
	uint32_t writes, reads, erases;
	uint64_t written, read; /* blocks */
};

/** The soak suite's state: the shared card, a shadow of the scratch area, counters. */
struct sdcard_9_soak_fixture {
	struct sdq *q;
	uint16_t shadow[SDQ_SCRATCH_BLOCKS]; /**< what each block should hold */
	int erased_value;                    /**< what erased blocks read as, -1 if not seen */
	struct soak_counts st;
};

/* The summary suite's fixture is the shared card (common.h). */
#define sdcard_zz_summary_fixture sdq

/**
 * @brief Check blocks just read against the shadow.
 *
 * @param[in,out] fx The suite's state.
 * @param p Data read.
 * @param first First block, from the scratch area's start.
 * @param blocks Block count.
 * @return -1 if all match, else the index of the first bad block.
 */
static int check_shadow(struct sdcard_9_soak_fixture *fx, const uint8_t *p, uint32_t first,
			uint32_t blocks)
{
	for (uint32_t b = 0; b < blocks; b++) {
		const uint8_t *blk = &p[(size_t)b * SDQ_BLK];
		uint16_t want = fx->shadow[first + b];

		if (want == ERASED) {
			if (fx->erased_value < 0) {
				fx->erased_value = blk[0];
			}
			if (!sdq_block_is(blk, (uint8_t)fx->erased_value)) {
				TC_PRINT("block %u: should be erased\n",
					 (fx->q->scratch + first + b));
				return (int)b;
			}
		} else if (want != UNKNOWN &&
			   sdq_check(blk, fx->q->scratch + first + b, 1, want) >= 0) {
			return (int)b;
		}
	}
	return -1;
}

/**
 * @brief Do one random operation.
 *
 * @param[in,out] fx The suite's state.
 * @param[in,out] s Random state.
 * @param gen Generation for a write.
 * @return 0, or the failing operation's error (-EBADMSG for bad data).
 */
static int one_op(struct sdcard_9_soak_fixture *fx, uint32_t *s, uint16_t gen)
{
	uint32_t pick = sdq_rand(s) % 100U;
	/* Mostly small requests, sometimes up to 1 MB. */
	uint32_t blocks = (sdq_rand(s) % 8U) == 0U ? 1U + (sdq_rand(s) % SDQ_MAX_BLOCKS)
						   : 1U + (sdq_rand(s) % 64U);
	uint32_t first = sdq_rand(s) % (SDQ_SCRATCH_BLOCKS - blocks);
	uint32_t lba = fx->q->scratch + first;
	int ret;

	if (pick < 45U) {
		sdq_fill(fx->q->wbuf, lba, blocks, gen);
		ret = disk_access_write(SDQ_DISK, fx->q->wbuf, lba, blocks);
		for (uint32_t b = 0; b < blocks && ret == 0; b++) {
			fx->shadow[first + b] = gen;
		}
		fx->st.writes++;
		fx->st.written += blocks;
	} else if (pick < 97U) {
		ret = disk_access_read(SDQ_DISK, fx->q->rbuf, lba, blocks);
		if (ret == 0 && check_shadow(fx, fx->q->rbuf, first, blocks) >= 0) {
			ret = -EBADMSG;
		}
		fx->st.reads++;
		fx->st.read += blocks;
	} else {
		ret = disk_access_erase(SDQ_DISK, lba, blocks, DISK_ACCESS_ERASE_PHYSICAL);
		for (uint32_t b = 0; b < blocks && ret == 0; b++) {
			fx->shadow[first + b] = ERASED;
		}
		fx->st.erases++;
	}
	if (ret != 0) {
		TC_PRINT("operation %s %u+%u failed: %d\n",
			 (pick < 45U ? "write" : (pick < 97U ? "read" : "erase")), lba, blocks,
			 ret);
	}
	return ret;
}

/**
 * @brief Print the progress counters.
 *
 * @param[in,out] fx The suite's state.
 * @param t0 Start time, microseconds.
 */
static void report(struct sdcard_9_soak_fixture *fx, uint64_t t0)
{
	uint64_t us = sdq_now_us() - t0;

	TC_PRINT("soak %llu s: %u writes (%llu MB, %u kB/s), %u reads (%llu MB, %u kB/s), "
		 "%u erases\n",
		 (us / 1000000U), fx->st.writes, (fx->st.written / 2048U),
		 sdq_kbps(fx->st.written * SDQ_BLK, us), fx->st.reads, (fx->st.read / 2048U),
		 sdq_kbps(fx->st.read * SDQ_BLK, us), fx->st.erases);
}

/** @brief Random writes, reads and erases for the configured time, all checked. */
ZTEST_F(sdcard_9_soak, test_1_random_workload)
{
	uint64_t t0 = sdq_now_us();
	uint64_t end = t0 + ((uint64_t)CONFIG_SDCARD_HW_SOAK_SECONDS * 1000000U);
	uint64_t next_report = t0 + REPORT_US;
	uint32_t s = fixture->q->seed ^ 0x3c6ef372U;
	uint16_t gen = 1;

	sdq_need_scratch(fixture->q);
	if (CONFIG_SDCARD_HW_SOAK_SECONDS == 0) {
		ztest_test_skip();
	}
	TC_PRINT("soak for %d s over %u MB\n", CONFIG_SDCARD_HW_SOAK_SECONDS,
		 (uint32_t)CONFIG_SDCARD_HW_SCRATCH_MB);
	while (sdq_now_us() < end) {
		zassert_ok(one_op(fixture, &s, gen), "after %u operations",
			   (fixture->st.writes + fixture->st.reads + fixture->st.erases));
		gen = gen == ERASED - 1U ? 1U : (uint16_t)(gen + 1U);
		if (sdq_now_us() >= next_report) {
			report(fixture, t0);
			next_report += REPORT_US;
		}
	}
	report(fixture, t0);
}

/** @brief Afterwards, every block of the scratch area holds what the soak last put there. */
ZTEST_F(sdcard_9_soak, test_2_final_verify)
{
	sdq_need_scratch(fixture->q);
	if (CONFIG_SDCARD_HW_SOAK_SECONDS == 0) {
		ztest_test_skip();
	}
	for (uint32_t b = 0; b < SDQ_SCRATCH_BLOCKS; b += SDQ_MAX_BLOCKS) {
		zassert_ok(disk_access_read(SDQ_DISK, fixture->q->rbuf, fixture->q->scratch + b,
					    SDQ_MAX_BLOCKS));
		zassert_equal(check_shadow(fixture, fixture->q->rbuf, b, SDQ_MAX_BLOCKS), -1,
			      "in MB %u of the area", (b / 2048U));
	}
	TC_PRINT("scratch area verified\n");
}

/**
 * @brief Suite setup: the shared card, and an empty shadow.
 *
 * @return The fixture.
 *
 * @par Global state
 * Changes @c fixture.
 */
static void *soak_setup(void)
{
	static struct sdcard_9_soak_fixture fixture = {.erased_value = -1};

	fixture.q = sdq_setup();
	return &fixture;
}

/**
 * @brief Before each test: check the card (sdq_before()).
 *
 * @param f The fixture.
 */
static void soak_before(void *f)
{
	sdq_before(((struct sdcard_9_soak_fixture *)f)->q);
}

/**
 * @brief After each test: check the driver (sdq_after()).
 *
 * @param f The fixture.
 */
static void soak_after(void *f)
{
	sdq_after(((struct sdcard_9_soak_fixture *)f)->q);
}

ZTEST_SUITE(sdcard_9_soak, NULL, soak_setup, soak_before, soak_after, NULL);

/** @brief No test left the card unusable: the harness never had to recover it. */
ZTEST_F(sdcard_zz_summary, test_no_recovery_needed)
{
	TC_PRINT("recoveries needed during the run: %u, of which %u failed\n", fixture->recoveries,
		 fixture->recovery_failures);
	zassert_equal(fixture->recoveries, 0,
		      "the card needed recovering %u times: see RECOVERY above",
		      fixture->recoveries);
}

/* Named to sort after every other suite (ztest runs them by name). */
ZTEST_SUITE(sdcard_zz_summary, NULL, sdq_setup, NULL, NULL, NULL);
