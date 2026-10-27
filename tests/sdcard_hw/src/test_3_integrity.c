/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 3: data written through the driver reads back intact, whatever
 * the data, the transfer size, the buffer alignment or the address, and
 * writes touch only the blocks they name. Uses the scratch area.
 */

#include <string.h>

#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#include "common.h"

/* Regions of the scratch area, in blocks from its start. */
#define R_PATTERNS 0U
#define R_SIZES    64U
#define R_CHUNKS   12288U
#define R_MISAL    13312U
#define R_NEIGH    14336U
#define R_OVER     15360U
#define R_RANDOM   32768U
#define RANDOM_BLOCKS 16384U /* 8 MB */
#define RANDOM_OPS    1000

BUILD_ASSERT((R_RANDOM + RANDOM_BLOCKS <= SDQ_SCRATCH_BLOCKS), "scratch area too small");

/* The suite's state: the shared card, and a shadow of the random region. */
struct sdcard_3_integrity_fixture {
	struct sdq *q;
	/* Generation of each block of the random region, 0 for unwritten. */
	uint16_t shadow[RANDOM_BLOCKS];
};

/**
 * @brief Suite setup: the shared card, and the shadow.
 *
 * @return The fixture.
 *
 * @par Global state
 * Changes @c fixture (its card pointer).
 */
static void *integrity_setup(void)
{
	static struct sdcard_3_integrity_fixture fixture;

	fixture.q = sdq_setup();
	return &fixture;
}

/**
 * @brief Before each test: check the card (sdq_before()).
 *
 * @param f The fixture.
 */
static void integrity_before(void *f)
{
	sdq_before(((struct sdcard_3_integrity_fixture *)f)->q);
}

/**
 * @brief After each test: check the driver (sdq_after()).
 *
 * @param f The fixture.
 */
static void integrity_after(void *f)
{
	sdq_after(((struct sdcard_3_integrity_fixture *)f)->q);
}

/* Fills one block, given the run's random seed. */
typedef void (*fill_fn)(uint8_t *p, uint32_t seed);

/**
 * @brief Write one block of raw bytes and read it back.
 *
 * @param[in,out] q The card and its buffers.
 * @param lba Block.
 * @param fill Fills the write buffer's first block, given the run's seed.
 * @param what Name for messages.
 */
static void raw_block_roundtrip(struct sdq *q, uint32_t lba, fill_fn fill, const char *what)
{
	fill(q->wbuf, q->seed);
	zassert_ok(disk_access_write(SDQ_DISK, q->wbuf, lba, 1), "%s: write", what);
	memset(q->rbuf, 0x5a, SDQ_BLK);
	zassert_ok(disk_access_read(SDQ_DISK, q->rbuf, lba, 1), "%s: read", what);
	zassert_mem_equal(q->rbuf, q->wbuf, SDQ_BLK, "%s: data differs", what);
}

/**
 * @brief All zero bits.
 *
 * @param[out] p Block.
 * @param seed Unused.
 */
static void fill_zeros(uint8_t *p, uint32_t seed)
{
	ARG_UNUSED(seed);
	memset(p, 0x00, SDQ_BLK);
}

/**
 * @brief All one bits.
 *
 * @param[out] p Block.
 * @param seed Unused.
 */
static void fill_ones(uint8_t *p, uint32_t seed)
{
	ARG_UNUSED(seed);
	memset(p, 0xff, SDQ_BLK);
}

/**
 * @brief Alternating bits, 0xaa / 0x55 by byte.
 *
 * @param[out] p Block.
 * @param seed Unused.
 */
static void fill_checker(uint8_t *p, uint32_t seed)
{
	ARG_UNUSED(seed);
	for (size_t i = 0; i < SDQ_BLK; i++) {
		p[i] = (i & 1U) != 0U ? 0x55U : 0xaaU;
	}
}

/**
 * @brief A single one bit walking through each byte.
 *
 * @param[out] p Block.
 * @param seed Unused.
 */
static void fill_walking_one(uint8_t *p, uint32_t seed)
{
	ARG_UNUSED(seed);
	for (size_t i = 0; i < SDQ_BLK; i++) {
		p[i] = (uint8_t)(1U << (i % 8U));
	}
}

/**
 * @brief A single zero bit walking through each byte.
 *
 * @param[out] p Block.
 * @param seed Unused.
 */
static void fill_walking_zero(uint8_t *p, uint32_t seed)
{
	ARG_UNUSED(seed);
	for (size_t i = 0; i < SDQ_BLK; i++) {
		p[i] = (uint8_t)~(1U << (i % 8U));
	}
}

/**
 * @brief Each byte its offset: shows dropped, repeated or swapped bytes.
 *
 * @param[out] p Block.
 * @param seed Unused.
 */
static void fill_counting(uint8_t *p, uint32_t seed)
{
	ARG_UNUSED(seed);
	for (size_t i = 0; i < SDQ_BLK; i++) {
		p[i] = (uint8_t)(i ^ (i >> 8));
	}
}

/**
 * @brief Random bytes.
 *
 * @param[out] p Block.
 * @param seed The run's random seed.
 */
static void fill_random(uint8_t *p, uint32_t seed)
{
	uint32_t s = seed;

	for (size_t i = 0; i < SDQ_BLK; i++) {
		p[i] = (uint8_t)sdq_rand(&s);
	}
}

/** @brief Single blocks of stress patterns (zeros, ones, checker, walking bits, random) round-trip. */
ZTEST_F(sdcard_3_integrity, test_1_patterns)
{
	sdq_need_scratch(fixture->q);
	raw_block_roundtrip(fixture->q, fixture->q->scratch + R_PATTERNS, fill_zeros, "zeros");
	raw_block_roundtrip(fixture->q, fixture->q->scratch + R_PATTERNS + 1U, fill_ones, "ones");
	raw_block_roundtrip(fixture->q, fixture->q->scratch + R_PATTERNS + 2U, fill_checker,
			    "checker");
	raw_block_roundtrip(fixture->q, fixture->q->scratch + R_PATTERNS + 3U, fill_walking_one,
			    "walking one");
	raw_block_roundtrip(fixture->q, fixture->q->scratch + R_PATTERNS + 4U, fill_walking_zero,
			    "walking zero");
	raw_block_roundtrip(fixture->q, fixture->q->scratch + R_PATTERNS + 5U, fill_counting,
			    "counting");
	raw_block_roundtrip(fixture->q, fixture->q->scratch + R_PATTERNS + 6U, fill_random,
			    "random");
}

/** @brief Every transfer size from 1 block to 1 MB, around each power of two, round-trips. */
ZTEST_F(sdcard_3_integrity, test_2_transfer_sizes)
{
	static const uint32_t sizes[] = {1,  2,   3,   4,   7,   8,   9,   15,   16,   17,  31,
					 32, 33,  63,  64,  65,  127, 128, 129,  255,  256, 257,
					 511, 512, 513, 1023, 1024, 1025, 2047, 2048,
					};
	uint32_t lba = fixture->q->scratch + R_SIZES;

	sdq_need_scratch(fixture->q);
	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		sdq_write_verify(fixture->q, lba, sizes[i], 0x30000U + sizes[i]);
		lba += sizes[i];
	}
	zassert_true((lba <= fixture->q->scratch + R_CHUNKS), "regions overlap");
}

/**
 * @brief Read a range back in pieces of @p piece blocks and check it.
 *
 * @param[in,out] q The card and its buffers.
 * @param lba First block.
 * @param blocks Block count.
 * @param piece Blocks a request.
 * @param gen Generation expected.
 */
static void read_in_pieces(struct sdq *q, uint32_t lba, uint32_t blocks, uint32_t piece,
			   uint32_t gen)
{
	memset(q->rbuf, 0xa5, (size_t)blocks * SDQ_BLK);
	for (uint32_t b = 0; b < blocks; b += piece) {
		uint32_t n = MIN(piece, (blocks - b));

		zassert_ok(disk_access_read(SDQ_DISK, &q->rbuf[(size_t)b * SDQ_BLK], lba + b, n),
			   "read %u+%u", (lba + b), n);
	}
	zassert_equal(sdq_check(q->rbuf, lba, blocks, gen), -1, "pieces of %u", piece);
}

/** @brief Data written in one request reads back in pieces, and the reverse. */
ZTEST_F(sdcard_3_integrity, test_3_chunking)
{
	uint32_t lba = fixture->q->scratch + R_CHUNKS;

	sdq_need_scratch(fixture->q);
	/* One 256-block write, read back in 1-, 7- and 64-block pieces. */
	sdq_fill(fixture->q->wbuf, lba, 256, 0x31000U);
	zassert_ok(disk_access_write(SDQ_DISK, fixture->q->wbuf, lba, 256));
	read_in_pieces(fixture->q, lba, 256, 1, 0x31000U);
	read_in_pieces(fixture->q, lba, 256, 7, 0x31000U);
	read_in_pieces(fixture->q, lba, 256, 64, 0x31000U);

	/* 256 one-block writes, read back in one request. */
	sdq_fill(fixture->q->wbuf, lba, 256, 0x31001U);
	for (uint32_t b = 0; b < 256U; b++) {
		zassert_ok(disk_access_write(SDQ_DISK, &fixture->q->wbuf[(size_t)b * SDQ_BLK],
					     lba + b, 1));
	}
	read_in_pieces(fixture->q, lba, 256, 256, 0x31001U);
}

/** @brief Buffers at every byte offset 0-7 work for writes and reads (the driver packs by hand). */
ZTEST_F(sdcard_3_integrity, test_4_misaligned_buffers)
{
	uint32_t lba = fixture->q->scratch + R_MISAL;

	sdq_need_scratch(fixture->q);
	for (size_t off = 0; off < 8U; off++) {
		uint32_t gen = 0x32000U + (uint32_t)off;

		sdq_fill(&fixture->q->wbuf[off], lba, 17, gen);
		zassert_ok(disk_access_write(SDQ_DISK, &fixture->q->wbuf[off], lba, 17),
			   "write, offset %zu", off);
		memset(fixture->q->rbuf, 0xa5, (size_t)18U * SDQ_BLK);
		zassert_ok(disk_access_read(SDQ_DISK, &fixture->q->rbuf[7U - off], lba, 17),
			   "read, offset %zu", (7U - off));
		zassert_equal(sdq_check(&fixture->q->rbuf[7U - off], lba, 17, gen), -1,
			      "write offset %zu, read offset %zu", off, (7U - off));
		/* Nothing written before or after the read buffer. */
		for (size_t i = 0; i < 7U - off; i++) {
			zassert_equal(fixture->q->rbuf[i], 0xa5,
				      "byte before the buffer overwritten");
		}
		zassert_equal(fixture->q->rbuf[(7U - off) + ((size_t)17U * SDQ_BLK)], 0xa5,
			      "byte after the buffer overwritten");
	}
}

/** @brief A write changes only its own blocks: the ones either side keep their data. */
ZTEST_F(sdcard_3_integrity, test_5_neighbours_untouched)
{
	uint32_t lba = fixture->q->scratch + R_NEIGH;

	sdq_need_scratch(fixture->q);
	sdq_write_verify(fixture->q, lba, 64, 0x33000U);
	for (uint32_t at = 1; at < 60U; at += 13U) {
		sdq_fill(fixture->q->wbuf, lba + at, 3, 0x33001U + at);
		zassert_ok(disk_access_write(SDQ_DISK, fixture->q->wbuf, lba + at, 3));
		zassert_ok(disk_access_read(SDQ_DISK, fixture->q->rbuf, lba, 64));
		zassert_equal(sdq_check(fixture->q->rbuf, lba, at, 0x33000U), -1, "before %u", at);
		zassert_equal(sdq_check(&fixture->q->rbuf[(size_t)at * SDQ_BLK], lba + at, 3,
					0x33001U + at),
			      -1, "written at %u", at);
		zassert_equal(sdq_check(&fixture->q->rbuf[(size_t)(at + 3U) * SDQ_BLK],
					lba + at + 3U, 64U - at - 3U, 0x33000U),
			      -1, "after %u", at);
		/* Restore the first generation for the next round. */
		sdq_fill(fixture->q->wbuf, lba + at, 3, 0x33000U);
		zassert_ok(disk_access_write(SDQ_DISK, fixture->q->wbuf, lba + at, 3));
	}
}

/** @brief Rewriting the same blocks returns the newest data, never an older write. */
ZTEST_F(sdcard_3_integrity, test_6_overwrite)
{
	uint32_t lba = fixture->q->scratch + R_OVER;

	sdq_need_scratch(fixture->q);
	for (uint32_t gen = 0; gen < 16U; gen++) {
		uint32_t blocks = 1U << (gen % 9U); /* 1 to 256 */

		sdq_write_verify(fixture->q, lba, blocks, 0x34000U + gen);
	}
}

/** @brief Blocks at every power-of-two distance apart keep distinct data (no address aliasing). */
ZTEST_F(sdcard_3_integrity, test_7_address_bits)
{
	uint32_t lbas[24];
	size_t n = 0;

	sdq_need_scratch(fixture->q);
	lbas[n++] = fixture->q->scratch;
	for (uint32_t d = 1; d < SDQ_SCRATCH_BLOCKS && n < ARRAY_SIZE(lbas) - 1U; d <<= 1) {
		lbas[n++] = fixture->q->scratch + d;
	}
	lbas[n++] = fixture->q->sectors - 1U;
	for (size_t i = 0; i < n; i++) {
		sdq_fill(fixture->q->wbuf, lbas[i], 1, 0x35000U);
		zassert_ok(disk_access_write(SDQ_DISK, fixture->q->wbuf, lbas[i], 1), "write %u",
			   lbas[i]);
	}
	for (size_t i = 0; i < n; i++) {
		zassert_ok(disk_access_read(SDQ_DISK, fixture->q->rbuf, lbas[i], 1), "read %u",
			   lbas[i]);
		zassert_equal(sdq_check(fixture->q->rbuf, lbas[i], 1, 0x35000U), -1, "block %u",
			      lbas[i]);
	}
}

/**
 * @brief Check a range of the random region against the shadow.
 *
 * @param[in,out] q The card and its buffers.
 * @param shadow Generation of each block of the random region.
 * @param first First block, from the region's start.
 * @param blocks Block count.
 */
static void check_random_range(struct sdq *q, const uint16_t *shadow, uint32_t first,
			       uint32_t blocks)
{
	uint32_t base = q->scratch + R_RANDOM;

	zassert_ok(disk_access_read(SDQ_DISK, q->rbuf, base + first, blocks));
	for (uint32_t b = 0; b < blocks; b++) {
		zassert_equal(sdq_check(&q->rbuf[(size_t)b * SDQ_BLK], base + first + b, 1,
					shadow[first + b]),
			      -1, "block %u", (base + first + b));
	}
}

/** @brief 1000 writes and reads of random sizes at random places all check against a shadow copy. */
ZTEST_F(sdcard_3_integrity, test_8_random_mix)
{
	uint32_t base = fixture->q->scratch + R_RANDOM;
	uint32_t s = fixture->q->seed;

	sdq_need_scratch(fixture->q);
	for (uint32_t b = 0; b < RANDOM_BLOCKS; b += SDQ_MAX_BLOCKS) {
		sdq_fill(fixture->q->wbuf, base + b, SDQ_MAX_BLOCKS, 1);
		zassert_ok(disk_access_write(SDQ_DISK, fixture->q->wbuf, base + b, SDQ_MAX_BLOCKS));
	}
	for (uint32_t b = 0; b < RANDOM_BLOCKS; b++) {
		fixture->shadow[b] = 1;
	}
	for (int op = 0; op < RANDOM_OPS; op++) {
		uint32_t blocks = 1U + (sdq_rand(&s) % 256U);
		uint32_t first = sdq_rand(&s) % (RANDOM_BLOCKS - blocks);

		if ((sdq_rand(&s) & 1U) != 0U) {
			uint16_t gen = (uint16_t)(2 + op);

			sdq_fill(fixture->q->wbuf, base + first, blocks, gen);
			zassert_ok(
				disk_access_write(SDQ_DISK, fixture->q->wbuf, base + first, blocks),
				"op %d: write %u+%u", op, (base + first), blocks);
			for (uint32_t b = 0; b < blocks; b++) {
				fixture->shadow[first + b] = gen;
			}
		} else {
			check_random_range(fixture->q, fixture->shadow, first, blocks);
		}
	}
	for (uint32_t b = 0; b < RANDOM_BLOCKS; b += SDQ_MAX_BLOCKS) {
		check_random_range(fixture->q, fixture->shadow, b, SDQ_MAX_BLOCKS);
	}
}

ZTEST_SUITE(sdcard_3_integrity, NULL, integrity_setup, integrity_before, integrity_after, NULL);
