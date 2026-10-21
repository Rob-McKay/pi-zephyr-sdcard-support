/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 7: several threads using the card at once, as an application with
 * a file system and a logger would. Each writer owns a region of the
 * scratch area and checks its own data; a reader re-reads the start of the
 * card and checks it never changes; a lower-priority thread counts how
 * often it gets to run while the driver busy-waits (review finding 6).
 */

#include <string.h>

#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#include "common.h"

#define R_THREADS    114688U
#define WRITERS      3
#define REGION       2048U /* 1 MB each */
#define CHUNK        64U   /* 32 KB a request */
#define ROUNDS       40
#define STACK        4096
#define READER_READS 200

BUILD_ASSERT(R_THREADS + (WRITERS * REGION) <= SDQ_SCRATCH_BLOCKS, "scratch area too small");

/** A writer's region, buffers and result. */
struct writer {
	uint32_t lba;                         /**< first block of its region */
	int error;                            /**< first failure, 0 if none */
	uint32_t bad_lba;                     /**< block of a data mismatch */
	uint8_t wbuf[CHUNK * SDQ_BLK];       /**< its write buffer */
	uint8_t rbuf[CHUNK * SDQ_BLK];       /**< its read buffer */
};

static struct writer writers[WRITERS];
static struct k_thread threads[WRITERS + 2];
/* Zephyr's definition macro can end in an empty statement: ISO C -Wpedantic
 * flags the ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static K_THREAD_STACK_ARRAY_DEFINE(stacks, WRITERS + 2, STACK);
#pragma GCC diagnostic pop

static int reader_error;
static atomic_t low_wakeups;
static atomic_t stop_low;

/**
 * @brief Writer thread: write and verify its region, chunk by chunk, ROUNDS times.
 *
 * @param p1 Its struct writer.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void writer_entry(void *p1, void *p2, void *p3)
{
	struct writer *w = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (int r = 0; r < ROUNDS && w->error == 0; r++) {
		for (uint32_t b = 0; b < REGION && w->error == 0; b += CHUNK) {
			uint32_t gen = 0x70000U + (uint32_t)r;
			int bad;

			sdq_fill(w->wbuf, w->lba + b, CHUNK, gen);
			w->error = disk_access_write(SDQ_DISK, w->wbuf, w->lba + b, CHUNK);
			if (w->error == 0) {
				w->error = disk_access_read(SDQ_DISK, w->rbuf, w->lba + b, CHUNK);
			}
			bad = w->error == 0 ? sdq_check(w->rbuf, w->lba + b, CHUNK, gen) : -1;
			if (bad >= 0) {
				w->bad_lba = w->lba + b + (uint32_t)bad;
				w->error = -EBADMSG;
			}
		}
	}
}

/**
 * @brief Reader thread: re-read blocks 0-63 and compare them with the first read.
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void reader_entry(void *p1, void *p2, void *p3)
{
	static uint8_t first[64 * SDQ_BLK];
	static uint8_t again[64 * SDQ_BLK];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	reader_error = disk_access_read(SDQ_DISK, first, 0, 64);
	for (int i = 0; i < READER_READS && reader_error == 0; i++) {
		reader_error = disk_access_read(SDQ_DISK, again, 0, 64);
		if (reader_error == 0 && memcmp(first, again, sizeof(first)) != 0) {
			reader_error = -EBADMSG;
		}
	}
}

/**
 * @brief Lower-priority thread: count 1 ms sleeps until told to stop.
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void low_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (atomic_get(&stop_low) == 0) {
		k_msleep(1);
		atomic_inc(&low_wakeups);
	}
}

/** @brief Three writers and a reader at once all see their own data, and nothing fails. */
ZTEST(sdcard_7_concurrency, test_1_writers_and_reader)
{
	int prio = k_thread_priority_get(k_current_get());
	uint64_t t0 = sdq_now_us();
	uint64_t ms;

	sdq_need_scratch();
	reader_error = 0;
	atomic_set(&low_wakeups, 0);
	atomic_set(&stop_low, 0);
	for (int i = 0; i < WRITERS; i++) {
		writers[i].lba = sdq.scratch + R_THREADS + ((uint32_t)i * REGION);
		writers[i].error = 0;
		k_thread_create(&threads[i], stacks[i], STACK, writer_entry, &writers[i], NULL, NULL,
				prio, 0, K_NO_WAIT);
	}
	k_thread_create(&threads[WRITERS], stacks[WRITERS], STACK, reader_entry, NULL, NULL, NULL,
			prio, 0, K_NO_WAIT);
	k_thread_create(&threads[WRITERS + 1], stacks[WRITERS + 1], STACK, low_entry, NULL, NULL,
			NULL, prio + 1, 0, K_NO_WAIT);
	for (int i = 0; i <= WRITERS; i++) {
		k_thread_join(&threads[i], K_FOREVER);
	}
	ms = (sdq_now_us() - t0) / 1000U;
	atomic_set(&stop_low, 1);
	k_thread_join(&threads[WRITERS + 1], K_FOREVER);

	TC_PRINT("%d writers x %u MB x %d rounds and a reader: %llu ms; a lower-priority thread "
		 "ran %ld times in that time (%llu if never blocked)\n",
		 WRITERS, REGION / 2048U, ROUNDS, ms, (long)atomic_get(&low_wakeups), ms);
	for (int i = 0; i < WRITERS; i++) {
		zassert_ok(writers[i].error, "writer %d: %d (bad block %u)", i, writers[i].error,
			   writers[i].bad_lba);
	}
	zassert_ok(reader_error, "reader: %d", reader_error);
}

ZTEST_SUITE(sdcard_7_concurrency, NULL, sdq_setup, sdq_before, sdq_after, NULL);
