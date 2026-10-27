/*
 * Copyright (c) 2026 Rob McKay
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

BUILD_ASSERT((R_THREADS + (WRITERS * REGION) <= SDQ_SCRATCH_BLOCKS), "scratch area too small");

/** A writer's region, buffers and result. */
struct writer {
	uint32_t lba;                         /**< first block of its region */
	int error;                            /**< first failure, 0 if none */
	uint32_t bad_lba;                     /**< block of a data mismatch */
	uint8_t wbuf[CHUNK * SDQ_BLK];       /**< its write buffer */
	uint8_t rbuf[CHUNK * SDQ_BLK];       /**< its read buffer */
};

/** The reader's two copies of blocks 0-63, and its result. */
struct reader {
	int error;                   /**< first failure, 0 if none */
	uint8_t first[64 * SDQ_BLK]; /**< the first read */
	uint8_t again[64 * SDQ_BLK]; /**< each later read */
};

/** The lower-priority thread's count, and the flag that stops it. */
struct low {
	atomic_t wakeups;
	atomic_t stop;
};

/** The suite's state: the shared card, and the threads'. */
struct sdcard_7_concurrency_fixture {
	struct sdq *q;
	struct writer writers[WRITERS];
	struct reader reader;
	struct low low;
	struct k_thread threads[WRITERS + 2];
};

/* Zephyr's definition macro can end in an empty statement: ISO C -Wpedantic
 * flags the ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static K_THREAD_STACK_ARRAY_DEFINE(stacks, WRITERS + 2, STACK);
#pragma GCC diagnostic pop

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
 * @param p1 Its struct reader.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void reader_entry(void *p1, void *p2, void *p3)
{
	struct reader *rd = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	rd->error = disk_access_read(SDQ_DISK, rd->first, 0, 64);
	for (int i = 0; i < READER_READS && rd->error == 0; i++) {
		rd->error = disk_access_read(SDQ_DISK, rd->again, 0, 64);
		if (rd->error == 0 && memcmp(rd->first, rd->again, sizeof(rd->first)) != 0) {
			rd->error = -EBADMSG;
		}
	}
}

/**
 * @brief Lower-priority thread: count 1 ms sleeps until told to stop.
 *
 * @param p1 Its struct low.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void low_entry(void *p1, void *p2, void *p3)
{
	struct low *lo = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (atomic_get(&lo->stop) == 0) {
		k_msleep(1);
		atomic_inc(&lo->wakeups);
	}
}

/**
 * @brief Start the writers and the reader at @p prio, and the counting
 *        thread just below it.
 *
 * @param[in,out] fx The suite's state.
 * @param prio The test thread's priority.
 *
 * @par Global state
 * Changes @c stacks (the threads run on them).
 */
static void start_threads(struct sdcard_7_concurrency_fixture *fx, int prio)
{
	fx->reader.error = 0;
	atomic_set(&fx->low.wakeups, 0);
	atomic_set(&fx->low.stop, 0);
	for (int i = 0; i < WRITERS; i++) {
		fx->writers[i].lba = fx->q->scratch + R_THREADS + ((uint32_t)i * REGION);
		fx->writers[i].error = 0;
		k_thread_create(&fx->threads[i], stacks[i], STACK, writer_entry, &fx->writers[i],
				NULL, NULL, prio, 0, K_NO_WAIT);
	}
	k_thread_create(&fx->threads[WRITERS], stacks[WRITERS], STACK, reader_entry, &fx->reader,
			NULL, NULL, prio, 0, K_NO_WAIT);
	k_thread_create(&fx->threads[WRITERS + 1], stacks[WRITERS + 1], STACK, low_entry, &fx->low,
			NULL, NULL, prio + 1, 0, K_NO_WAIT);
}

/** @brief Three writers and a reader at once all see their own data, and nothing fails. */
ZTEST_F(sdcard_7_concurrency, test_1_writers_and_reader)
{
	int prio = k_thread_priority_get(k_current_get());
	uint64_t t0 = sdq_now_us();
	uint64_t ms;

	sdq_need_scratch(fixture->q);
	start_threads(fixture, prio);
	for (int i = 0; i <= WRITERS; i++) {
		k_thread_join(&fixture->threads[i], K_FOREVER);
	}
	ms = (sdq_now_us() - t0) / 1000U;
	atomic_set(&fixture->low.stop, 1);
	k_thread_join(&fixture->threads[WRITERS + 1], K_FOREVER);

	TC_PRINT("%d writers x %u MB x %d rounds and a reader: %llu ms; a lower-priority thread "
		 "ran %ld times in that time (%llu if never blocked)\n",
		 WRITERS, (REGION / 2048U), ROUNDS, ms, (long)atomic_get(&fixture->low.wakeups),
		 ms);
	for (int i = 0; i < WRITERS; i++) {
		zassert_ok(fixture->writers[i].error, "writer %d: %d (bad block %u)", i,
			   fixture->writers[i].error, fixture->writers[i].bad_lba);
	}
	zassert_ok(fixture->reader.error, "reader: %d", fixture->reader.error);
}

/**
 * @brief Suite setup: the shared card, and the threads' state.
 *
 * @return The fixture.
 *
 * @par Global state
 * Changes @c fixture (its card pointer).
 */
static void *concurrency_setup(void)
{
	static struct sdcard_7_concurrency_fixture fixture;

	fixture.q = sdq_setup();
	return &fixture;
}

/**
 * @brief Before each test: check the card (sdq_before()).
 *
 * @param f The fixture.
 */
static void concurrency_before(void *f)
{
	sdq_before(((struct sdcard_7_concurrency_fixture *)f)->q);
}

/**
 * @brief After each test: check the driver (sdq_after()).
 *
 * @param f The fixture.
 */
static void concurrency_after(void *f)
{
	sdq_after(((struct sdcard_7_concurrency_fixture *)f)->q);
}

ZTEST_SUITE(sdcard_7_concurrency, NULL, concurrency_setup, concurrency_before, concurrency_after,
	    NULL);
