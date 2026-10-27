/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared state and helpers of the SD card qualification suites.
 */

#include "common.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/ztest.h>

#include <hal/bcm2835_sdhost.h>
#include <rpi_fw.h>

#include "ubsan.h"

#define TAG           0x5d0c5d0cU
#define FW_CLOCK_CORE 4

static const struct device *const fw = DEVICE_DT_GET(DT_NODELABEL(firmware));

/* The card and buffers every suite shares: their fixture (common.h). */
static struct sdq sdq = {.why = "not checked"};

/**
 * @brief Pattern word @p i of block @p lba, generation @p gen.
 *
 * @param lba Block number.
 * @param gen Generation.
 * @param i Word index, 3 to 127.
 * @return The word.
 */
static uint32_t pattern_word(uint32_t lba, uint32_t gen, uint32_t i)
{
	uint32_t x = (lba * 0x9e3779b1U) ^ (gen * 0x85ebca6bU) ^ (i * 0xc2b2ae35U);

	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	return x;
}

/* Interface: common.h. */
void sdq_fill(uint8_t *p, uint32_t lba, uint32_t blocks, uint32_t gen)
{
	for (uint32_t b = 0; b < blocks; b++) {
		uint8_t *blk = p + ((size_t)b * SDQ_BLK);

		sys_put_le32(TAG, blk);
		sys_put_le32(lba + b, blk + 4);
		sys_put_le32(gen, blk + 8);
		for (uint32_t i = 3; i < SDQ_BLK / 4U; i++) {
			sys_put_le32(pattern_word(lba + b, gen, i), blk + ((size_t)i * 4U));
		}
	}
}

/* Interface: common.h. */
bool sdq_block_is(const uint8_t *p, uint8_t value)
{
	for (size_t i = 0; i < SDQ_BLK; i++) {
		if (p[i] != value) {
			return false;
		}
	}
	return true;
}

/**
 * @brief Find the first word of a block that differs from its pattern.
 *
 * Computes each expected word rather than filling a buffer, so several
 * threads can check at once.
 *
 * @param p The block.
 * @param lba Block number expected.
 * @param gen Generation expected.
 * @return Index of the first bad word, or SDQ_BLK / 4 if all match.
 */
static uint32_t first_bad_word(const uint8_t *p, uint32_t lba, uint32_t gen)
{
	const uint32_t head[3] = {TAG, lba, gen};

	for (uint32_t i = 0; i < SDQ_BLK / 4U; i++) {
		uint32_t want = i < 3U ? head[i] : pattern_word(lba, gen, i);

		if (sys_get_le32(p + ((size_t)i * 4U)) != want) {
			return i;
		}
	}
	return SDQ_BLK / 4U;
}

/**
 * @brief Print what a bad block holds instead of the expected pattern.
 *
 * @param p The block.
 * @param lba Block number expected.
 * @param gen Generation expected.
 */
static void describe_bad(const uint8_t *p, uint32_t lba, uint32_t gen)
{
	size_t off = (size_t)first_bad_word(p, lba, gen) * 4U;

	if (sys_get_le32(p) == TAG) {
		TC_PRINT("block %u: holds block %u generation %u (expected generation %u), "
			 "first difference at byte %zu\n",
			 lba, sys_get_le32(p + 4), sys_get_le32(p + 8), gen, off);
	} else if (sdq_block_is(p, 0x00) || sdq_block_is(p, 0xff)) {
		TC_PRINT("block %u: erased (all 0x%02x)\n", lba, p[0]);
	} else {
		TC_PRINT("block %u: unknown data, first word 0x%08x, first difference at byte %zu\n",
			 lba, sys_get_le32(p), off);
	}
}

/* Interface: common.h. */
int sdq_check(const uint8_t *p, uint32_t lba, uint32_t blocks, uint32_t gen)
{
	for (uint32_t b = 0; b < blocks; b++) {
		const uint8_t *blk = p + ((size_t)b * SDQ_BLK);

		if (first_bad_word(blk, lba + b, gen) < SDQ_BLK / 4U) {
			describe_bad(blk, lba + b, gen);
			return (int)b;
		}
	}
	return -1;
}

/* Interface: common.h. */
void sdq_write_verify(struct sdq *q, uint32_t lba, uint32_t blocks, uint32_t gen)
{
	sdq_fill(q->wbuf, lba, blocks, gen);
	zassert_ok(disk_access_write(SDQ_DISK, q->wbuf, lba, blocks), "write %u+%u", lba, blocks);
	memset(q->rbuf, 0xa5, (size_t)blocks * SDQ_BLK);
	zassert_ok(disk_access_read(SDQ_DISK, q->rbuf, lba, blocks), "read %u+%u", lba, blocks);
	zassert_equal(sdq_check(q->rbuf, lba, blocks, gen), -1, "data of %u+%u", lba, blocks);
}

/* Interface: common.h. */
uint32_t sdq_rand(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* Interface: common.h. */
uint64_t sdq_now_us(void)
{
	return k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

/* Interface: common.h. */
uint32_t sdq_kbps(uint64_t bytes, uint64_t us)
{
	return us == 0U ? 0U : (uint32_t)((bytes * 1000U) / us);
}

/* Interface: common.h. */
uint32_t sdq_core_clock_hz(void)
{
	uint32_t req[2] = {FW_CLOCK_CORE, 0};

	if (!device_is_ready(fw) ||
	    rpi_fw_transfer(fw, RPI_FW_TAG_GET_CLOCK_RATE, req, sizeof(req)) != 0) {
		return 0;
	}
	return req[1];
}

/**
 * @brief Check that the scratch area lies in free clusters of a FAT32
 *        partition, by reading its boot sector and FAT a block at a time.
 *
 * Writing there changes no file: FAT32 never reads a free cluster.
 *
 * @param[in,out] q The card; its spare block is used.
 * @param e The partition's MBR entry.
 * @return NULL if every cluster the scratch area touches is free, else why not.
 */
static const char *fat_scratch_free(struct sdq *q, const uint8_t *e)
{
	uint8_t *blk = q->blk;
	uint32_t start = sys_get_le32(e + 8);
	uint32_t end = start + sys_get_le32(e + 12) - 1U; /* last block */
	uint32_t fat;
	uint32_t data;
	uint32_t spc;
	uint32_t clusters;
	uint32_t first;
	uint32_t last;
	uint32_t loaded = UINT32_MAX;

	if (e[4] != 0x0bU && e[4] != 0x0cU) {
		return "a partition that isn't FAT32 reaches the scratch area at the end of the card";
	}
	if (disk_access_read(SDQ_DISK, blk, start, 1) != 0 || blk[510] != 0x55U ||
	    blk[511] != 0xaaU || sys_get_le16(&blk[11]) != SDQ_BLK || blk[13] == 0U ||
	    blk[16] == 0U || sys_get_le32(&blk[36]) == 0U) {
		return "the FAT32 partition reaching the scratch area has no usable boot sector";
	}
	spc = blk[13];
	fat = start + sys_get_le16(&blk[14]);
	data = fat + (blk[16] * sys_get_le32(&blk[36]));
	clusters = (sys_get_le32(&blk[32]) - (data - start)) / spc;
	if (q->scratch < data) {
		return "the scratch area would overlap the FAT32 partition's tables";
	}
	/* Blocks past the last whole cluster, up to the partition's end, are
	 * slack no file uses.
	 */
	first = 2U + ((q->scratch - data) / spc);
	last = MIN((2U + ((MIN(end, (q->sectors - 1U)) - data) / spc)), (clusters + 1U));
	for (uint32_t c = first; c <= last; c++) {
		uint32_t lba = fat + ((c * 4U) / SDQ_BLK);

		if (lba != loaded) {
			if (disk_access_read(SDQ_DISK, blk, lba, 1) != 0) {
				return "the FAT could not be read";
			}
			loaded = lba;
		}
		if ((sys_get_le32(&blk[(c * 4U) % SDQ_BLK]) & 0x0fffffffU) != 0U) {
			return "the FAT32 partition has files in the scratch area at the end of the card";
		}
	}
	TC_PRINT("scratch area lies in free clusters %u-%u of FAT32 partition at block %u\n",
		 first, last, start);
	return NULL;
}

/**
 * @brief Check the MBR's partitions against the scratch area: none may
 *        reach it, except a FAT32 partition whose clusters there are free.
 *
 * @param[in,out] q The card.
 * @param mbr Block 0, with a valid MBR signature.
 * @return NULL if the scratch area may be written, else why not.
 */
static const char *check_partitions(struct sdq *q, const uint8_t *mbr)
{
	for (size_t i = 0; i < 4U; i++) {
		const uint8_t *e = &mbr[446U + (i * 16U)];
		uint32_t start = sys_get_le32(e + 8);
		uint32_t count = sys_get_le32(e + 12);
		const char *why;

		if (e[4] == 0U || count == 0U || (uint64_t)start + count <= q->scratch) {
			continue;
		}
		why = fat_scratch_free(q, e);
		if (why != NULL) {
			return why;
		}
	}
	return NULL;
}

/**
 * @brief Decide whether the scratch area may be written: if no MBR
 *        partition uses it, or, without an MBR, if block 0 is blank.
 *
 * @param[in,out] q The card.
 */
static void check_scratch(struct sdq *q)
{
	const char *why;

	q->scratch_ok = false;
	if (q->sectors < 2U * SDQ_SCRATCH_BLOCKS) {
		q->why = "card smaller than twice CONFIG_SDCARD_HW_SCRATCH_MB";
		return;
	}
	q->scratch = q->sectors - SDQ_SCRATCH_BLOCKS;
	if (q->block0[510] != 0x55U || q->block0[511] != 0xaaU) {
		if (!sdq_block_is(q->block0, 0x00)) {
			q->why = "no MBR, and block 0 is not blank";
			return;
		}
	} else {
		why = check_partitions(q, q->block0);
		if (why != NULL) {
			q->why = why;
			return;
		}
	}
	q->scratch_ok = true;
}

/**
 * @brief Print the card, bus and scratch area, once.
 *
 * @param q The card.
 */
static void print_setup(const struct sdq *q)
{
	uintptr_t regs = q->sdhost;
	uint32_t core = sdq_core_clock_hz();
	uint32_t div = hal_sdhost_clock_divider(regs);

	TC_PRINT("card: %u blocks of %u bytes (%u MB)\n", q->sectors, q->sector_size,
		 (q->sectors / 2048U));
	TC_PRINT("bus: %s, core clock %u Hz / (SDCDIV %u + 2) = %u Hz\n",
		 (hal_sdhost_bus_is_4bit(regs) ? "4-bit" : "1-bit"), core, div,
		 (core / (div + 2U)));
	if (q->scratch_ok) {
		TC_PRINT("scratch area: blocks %u-%u (%u MB, written by these tests)\n", q->scratch,
			 (q->sectors - 1U), (uint32_t)CONFIG_SDCARD_HW_SCRATCH_MB);
	} else {
		TC_PRINT("no scratch area (%s): the write tests are skipped\n", q->why);
		for (size_t i = 0; i < 4U; i++) {
			const uint8_t *e = &q->block0[446U + (i * 16U)];

			if (e[4] != 0U) {
				TC_PRINT("  partition %zu: type 0x%02x, blocks %u-%u\n", (i + 1U),
					 e[4], sys_get_le32(e + 8),
					 (sys_get_le32(e + 8) + sys_get_le32(e + 12) - 1U));
			}
		}
	}
	TC_PRINT("random seed: %u (CONFIG_SDCARD_HW_SEED repeats it)\n", q->seed);
}

/* Interface: common.h.
 * Global state: changes sdq.
 */
void *sdq_setup(void)
{
	struct sdq *q = &sdq;

	if (q->ready) {
		return q;
	}
	device_map(&q->sdhost, DT_REG_ADDR(DT_NODELABEL(sdhost)), DT_REG_SIZE(DT_NODELABEL(sdhost)),
		   K_MEM_CACHE_NONE);
	zassert_ok(disk_access_init(SDQ_DISK), "no card, or card init failed");
	zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_GET_SECTOR_COUNT, &q->sectors));
	zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_GET_SECTOR_SIZE, &q->sector_size));
	zassert_ok(disk_access_read(SDQ_DISK, q->block0, 0, 1), "block 0 unreadable");
	check_scratch(q);
	q->seed = CONFIG_SDCARD_HW_SEED != 0 ? (uint32_t)CONFIG_SDCARD_HW_SEED
					     : (k_cycle_get_32() | 1U);
	print_setup(q);
	q->ready = true;
	return q;
}

/* Interface: common.h. */
void sdq_need_scratch(const struct sdq *q)
{
	if (!q->scratch_ok) {
		ztest_test_skip();
	}
}

/**
 * @brief Tell whether the card answers correctly: status OK and block 0
 *        reads back as it did at setup.
 *
 * @param[in,out] q The card; its spare block is used.
 * @return True if healthy.
 */
static bool card_healthy(struct sdq *q)
{
	return disk_access_status(SDQ_DISK) == DISK_STATUS_OK &&
	       disk_access_read(SDQ_DISK, q->blk, 0, 1) == 0 &&
	       memcmp(q->blk, q->block0, SDQ_BLK) == 0;
}

/**
 * @brief Power the card off and initialise it again through the disk API.
 *
 * @return 0, or the error of the re-init.
 */
static int power_cycle(void)
{
	(void)disk_access_ioctl(SDQ_DISK, DISK_IOCTL_CTRL_DEINIT, NULL);
	k_msleep(10);
	return disk_access_ioctl(SDQ_DISK, DISK_IOCTL_CTRL_INIT, NULL);
}

/**
 * @brief Empty the controller's FIFO, which no driver path does (review
 *        finding 2): the last resort when a power cycle didn't help.
 *
 * @param q The card.
 * @return Words removed.
 */
static int drain_fifo(const struct sdq *q)
{
	uintptr_t regs = q->sdhost;
	int words = 0;

	while (hal_sdhost_fifo_level(regs) != 0U && words < 4096) {
		(void)hal_sdhost_fifo_read(regs);
		words++;
	}
	return words;
}

/* Interface: common.h. */
void sdq_before(void *f)
{
	struct sdq *q = f;
	int ret;

	ubsan_reset();
	if (card_healthy(q)) {
		return;
	}
	q->recoveries++;
	TC_PRINT("RECOVERY: the card did not answer correctly after the previous test; "
		 "power cycling it through the disk API\n");
	ret = power_cycle();
	if (ret == 0 && card_healthy(q)) {
		return;
	}
	TC_PRINT("RECOVERY: re-init gave %d; emptying %d stale FIFO words and retrying\n", ret,
		 drain_fifo(q));
	ret = power_cycle();
	if (ret != 0 || !card_healthy(q)) {
		/* Not an assertion: ztest reports a failure in before() as a
		 * skip, which would hide it. The test runs, and fails on the
		 * card itself.
		 */
		q->recovery_failures++;
		TC_PRINT("RECOVERY FAILED (%d): the card is still unusable; this test will fail "
			 "too\n",
			 ret);
	}
}

/* Interface: common.h. */
void sdq_after(void *f)
{
	ARG_UNUSED(f);
	zassert_equal(ubsan_driver_overflows, 0,
		      "signed integer overflow in the driver (%d, last at line %u)",
		      ubsan_driver_overflows, ubsan_last_line);
}
