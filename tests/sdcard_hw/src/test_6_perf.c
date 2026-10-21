/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 6: throughput and latency, printed as a table for the report.
 * Sequential reads use the start of the card (read-only); writes and the
 * random writes use the scratch area and are verified afterwards. Only
 * gross failures are asserted: the numbers depend on the card.
 */

#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#include "common.h"

#define R_SEQ_WRITE 98304U
#define SEQ_BLOCKS  16384U /* 8 MB */
#define RANDOM_OPS  500

BUILD_ASSERT(R_SEQ_WRITE + SEQ_BLOCKS <= SDQ_SCRATCH_BLOCKS, "scratch area too small");

/* Blocks a request, for the sequential runs. */
static const uint32_t chunks[] = {1, 8, 64, 256, 2048};

/**
 * @brief Read or write @p total blocks from @p lba, @p chunk at a time.
 *
 * @param write True to write sdq_wbuf, false to read into sdq_rbuf.
 * @param lba First block.
 * @param total Blocks; a multiple of @p chunk.
 * @param chunk Blocks a request.
 * @return Microseconds taken.
 */
static uint64_t sequential(bool write, uint32_t lba, uint32_t total, uint32_t chunk)
{
	uint64_t t0 = sdq_now_us();

	for (uint32_t b = 0; b < total; b += chunk) {
		int ret = write ? disk_access_write(SDQ_DISK, &sdq_wbuf[0], lba + b, chunk)
				: disk_access_read(SDQ_DISK, sdq_rbuf, lba + b, chunk);

		zassert_ok(ret, "%s %u+%u", write ? "write" : "read", lba + b, chunk);
	}
	return sdq_now_us() - t0;
}

/** @brief Sequential reads, 1 block to 1 MB a request: multi-block reads run at least 1 MB/s. */
ZTEST(sdcard_6_perf, test_1_sequential_read)
{
	uint32_t best = 0;

	TC_PRINT("sequential read, from block 0:\n");
	for (size_t i = 0; i < ARRAY_SIZE(chunks); i++) {
		/* 1 MB a block at a time, 8 MB otherwise. */
		uint32_t total = chunks[i] == 1U ? 2048U : SEQ_BLOCKS;
		uint64_t us = sequential(false, 0, total, chunks[i]);
		uint32_t kbps = sdq_kbps((uint64_t)total * SDQ_BLK, us);

		TC_PRINT("  %4u blocks/request: %6u kB/s\n", chunks[i], kbps);
		best = MAX(best, kbps);
	}
	zassert_true(best >= 1000U, "best sequential read %u kB/s", best);
}

/** @brief Sequential writes, 1 block to 1 MB a request, then the data verified. */
ZTEST(sdcard_6_perf, test_2_sequential_write)
{
	uint32_t lba = sdq.scratch + R_SEQ_WRITE;

	sdq_need_scratch();
	TC_PRINT("sequential write, scratch area:\n");
	for (size_t i = 0; i < ARRAY_SIZE(chunks); i++) {
		uint32_t total = chunks[i] == 1U ? 2048U : SEQ_BLOCKS;
		uint64_t us;

		/* Every request writes the same buffer; the last is read back. */
		sdq_fill(sdq_wbuf, lba, chunks[i], 0x60000U + chunks[i]);
		us = sequential(true, lba, total, chunks[i]);
		TC_PRINT("  %4u blocks/request: %6u kB/s\n", chunks[i],
			 sdq_kbps((uint64_t)total * SDQ_BLK, us));
		/* The last chunk, as written, must read back. */
		zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba + total - chunks[i], chunks[i]));
		zassert_mem_equal(sdq_rbuf, sdq_wbuf, (size_t)chunks[i] * SDQ_BLK,
				  "last chunk of %u differs", chunks[i]);
	}
}

/**
 * @brief Print latency statistics.
 *
 * @param what Name.
 * @param min_us Shortest.
 * @param sum_us Sum.
 * @param max_us Longest.
 * @param n Operations.
 */
static void print_latency(const char *what, uint64_t min_us, uint64_t sum_us, uint64_t max_us,
			  int n)
{
	TC_PRINT("%s: %d ops, %llu ops/s, latency min %llu us, avg %llu us, max %llu us\n", what,
		 n, sum_us == 0U ? 0U : ((uint64_t)n * 1000000U) / sum_us, min_us,
		 sum_us / (uint64_t)n, max_us);
}

/** @brief Random 4 KB reads over the whole card. */
ZTEST(sdcard_6_perf, test_3_random_read)
{
	uint32_t s = sdq.seed ^ 0x6a09e667U;
	uint64_t min_us = UINT64_MAX;
	uint64_t max_us = 0;
	uint64_t sum = 0;

	for (int i = 0; i < RANDOM_OPS; i++) {
		uint32_t lba = (sdq_rand(&s) % (sdq.sectors / 8U)) * 8U;
		uint64_t t0 = sdq_now_us();
		uint64_t us;

		zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba, 8), "read %u", lba);
		us = sdq_now_us() - t0;
		min_us = MIN(min_us, us);
		max_us = MAX(max_us, us);
		sum += us;
	}
	print_latency("random 4 KB read", min_us, sum, max_us, RANDOM_OPS);
}

/** @brief Random 4 KB writes over the scratch area, each verified at the end. */
ZTEST(sdcard_6_perf, test_4_random_write)
{
	static uint32_t lbas[RANDOM_OPS];
	uint32_t s = sdq.seed ^ 0xbb67ae85U;
	uint64_t min_us = UINT64_MAX;
	uint64_t max_us = 0;
	uint64_t sum = 0;
	uint32_t slots = SDQ_SCRATCH_BLOCKS / 8U;
	uint32_t first = sdq_rand(&s) % slots;

	sdq_need_scratch();
	for (int i = 0; i < RANDOM_OPS; i++) {
		/* Distinct 4 KB slots scattered over the scratch area (40503 is
		 * a prime, so a step coprime to the slot count), so every write
		 * survives to the check.
		 */
		uint32_t lba = sdq.scratch + (((first + ((uint32_t)i * 40503U)) % slots) * 8U);
		uint64_t t0;
		uint64_t us;

		lbas[i] = lba;
		sdq_fill(sdq_wbuf, lba, 8, 0x61000U + (uint32_t)i);
		t0 = sdq_now_us();
		zassert_ok(disk_access_write(SDQ_DISK, sdq_wbuf, lba, 8), "write %u", lba);
		us = sdq_now_us() - t0;
		min_us = MIN(min_us, us);
		max_us = MAX(max_us, us);
		sum += us;
	}
	print_latency("random 4 KB write", min_us, sum, max_us, RANDOM_OPS);
	for (int i = 0; i < RANDOM_OPS; i++) {
		zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lbas[i], 8));
		zassert_equal(sdq_check(sdq_rbuf, lbas[i], 8, 0x61000U + (uint32_t)i), -1,
			      "write %d at %u", i, lbas[i]);
	}
}

ZTEST_SUITE(sdcard_6_perf, NULL, sdq_setup, sdq_before, sdq_after, NULL);
