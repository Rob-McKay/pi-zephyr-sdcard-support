/*
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

/* 64 spare bytes let a test start the data at any offset. */
uint8_t sdq_wbuf[(SDQ_MAX_BLOCKS * SDQ_BLK) + 64] __aligned(64);
uint8_t sdq_rbuf[(SDQ_MAX_BLOCKS * SDQ_BLK) + 64] __aligned(64);

struct sdq_card sdq = {.why = "not checked"};

static const struct device *const fw = DEVICE_DT_GET(DT_NODELABEL(firmware));

/* Block 0 as read at setup: the health check compares against it. */
static uint8_t block0[SDQ_BLK] __aligned(4);

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
 * @brief Print what a bad block holds instead of the expected pattern.
 *
 * @param p The block.
 * @param lba Block number expected.
 * @param gen Generation expected.
 */
static void describe_bad(const uint8_t *p, uint32_t lba, uint32_t gen)
{
	static uint8_t want[SDQ_BLK];
	size_t off = 0;

	sdq_fill(want, lba, 1, gen);
	while (off < SDQ_BLK && p[off] == want[off]) {
		off++;
	}
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

int sdq_check(const uint8_t *p, uint32_t lba, uint32_t blocks, uint32_t gen)
{
	static uint8_t want[SDQ_BLK];

	for (uint32_t b = 0; b < blocks; b++) {
		const uint8_t *blk = p + ((size_t)b * SDQ_BLK);

		sdq_fill(want, lba + b, 1, gen);
		if (memcmp(blk, want, SDQ_BLK) != 0) {
			describe_bad(blk, lba + b, gen);
			return (int)b;
		}
	}
	return -1;
}

void sdq_write_verify(uint32_t lba, uint32_t blocks, uint32_t gen)
{
	sdq_fill(sdq_wbuf, lba, blocks, gen);
	zassert_ok(disk_access_write(SDQ_DISK, sdq_wbuf, lba, blocks), "write %u+%u", lba,
		   blocks);
	memset(sdq_rbuf, 0xa5, (size_t)blocks * SDQ_BLK);
	zassert_ok(disk_access_read(SDQ_DISK, sdq_rbuf, lba, blocks), "read %u+%u", lba, blocks);
	zassert_equal(sdq_check(sdq_rbuf, lba, blocks, gen), -1, "data of %u+%u", lba, blocks);
}

uint32_t sdq_rand(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

uint64_t sdq_now_us(void)
{
	return k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

uint32_t sdq_kbps(uint64_t bytes, uint64_t us)
{
	return us == 0U ? 0U : (uint32_t)((bytes * 1000U) / us);
}

uintptr_t sdq_sdhost_regs(void)
{
	static uintptr_t base;

	if (base == 0U) {
		device_map(&base, DT_REG_ADDR(DT_NODELABEL(sdhost)),
			   DT_REG_SIZE(DT_NODELABEL(sdhost)), K_MEM_CACHE_NONE);
	}
	return base;
}

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
 * @param e The partition's MBR entry.
 * @return NULL if every cluster the scratch area touches is free, else why not.
 */
static const char *fat_scratch_free(const uint8_t *e)
{
	static uint8_t blk[SDQ_BLK] __aligned(4);
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
	if (sdq.scratch < data) {
		return "the scratch area would overlap the FAT32 partition's tables";
	}
	/* Blocks past the last whole cluster, up to the partition's end, are
	 * slack no file uses.
	 */
	first = 2U + ((sdq.scratch - data) / spc);
	last = MIN(2U + ((MIN(end, sdq.sectors - 1U) - data) / spc), clusters + 1U);
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
 * @param mbr Block 0, with a valid MBR signature.
 * @return NULL if the scratch area may be written, else why not.
 */
static const char *check_partitions(const uint8_t *mbr)
{
	for (size_t i = 0; i < 4U; i++) {
		const uint8_t *e = &mbr[446U + (i * 16U)];
		uint32_t start = sys_get_le32(e + 8);
		uint32_t count = sys_get_le32(e + 12);
		const char *why;

		if (e[4] == 0U || count == 0U || (uint64_t)start + count <= sdq.scratch) {
			continue;
		}
		why = fat_scratch_free(e);
		if (why != NULL) {
			return why;
		}
	}
	return NULL;
}

/**
 * @brief Decide whether the scratch area may be written: if no MBR
 *        partition uses it, or, without an MBR, if block 0 is blank.
 */
static void check_scratch(void)
{
	const char *why;

	sdq.scratch_ok = false;
	if (sdq.sectors < 2U * SDQ_SCRATCH_BLOCKS) {
		sdq.why = "card smaller than twice CONFIG_SDCARD_HW_SCRATCH_MB";
		return;
	}
	sdq.scratch = sdq.sectors - SDQ_SCRATCH_BLOCKS;
	if (block0[510] != 0x55U || block0[511] != 0xaaU) {
		if (!sdq_block_is(block0, 0x00)) {
			sdq.why = "no MBR, and block 0 is not blank";
			return;
		}
	} else {
		why = check_partitions(block0);
		if (why != NULL) {
			sdq.why = why;
			return;
		}
	}
	sdq.scratch_ok = true;
}

/** @brief Print the card, bus and scratch area, once. */
static void print_setup(void)
{
	uintptr_t regs = sdq_sdhost_regs();
	uint32_t core = sdq_core_clock_hz();
	uint32_t div = hal_sdhost_clock_divider(regs);

	TC_PRINT("card: %u blocks of %u bytes (%u MB)\n", sdq.sectors, sdq.sector_size,
		 sdq.sectors / 2048U);
	TC_PRINT("bus: %s, core clock %u Hz / (SDCDIV %u + 2) = %u Hz\n",
		 hal_sdhost_bus_is_4bit(regs) ? "4-bit" : "1-bit", core, div, core / (div + 2U));
	if (sdq.scratch_ok) {
		TC_PRINT("scratch area: blocks %u-%u (%u MB, written by these tests)\n",
			 sdq.scratch, sdq.sectors - 1U, (uint32_t)CONFIG_SDCARD_HW_SCRATCH_MB);
	} else {
		TC_PRINT("no scratch area (%s): the write tests are skipped\n", sdq.why);
		for (size_t i = 0; i < 4U; i++) {
			const uint8_t *e = &block0[446U + (i * 16U)];

			if (e[4] != 0U) {
				TC_PRINT("  partition %zu: type 0x%02x, blocks %u-%u\n", i + 1U, e[4],
					 sys_get_le32(e + 8),
					 sys_get_le32(e + 8) + sys_get_le32(e + 12) - 1U);
			}
		}
	}
	TC_PRINT("random seed: %u (CONFIG_SDCARD_HW_SEED repeats it)\n", sdq.seed);
}

void *sdq_setup(void)
{
	static bool done;

	if (done) {
		return NULL;
	}
	zassert_ok(disk_access_init(SDQ_DISK), "no card, or card init failed");
	zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_GET_SECTOR_COUNT, &sdq.sectors));
	zassert_ok(disk_access_ioctl(SDQ_DISK, DISK_IOCTL_GET_SECTOR_SIZE, &sdq.sector_size));
	zassert_ok(disk_access_read(SDQ_DISK, block0, 0, 1), "block 0 unreadable");
	check_scratch();
	sdq.seed = CONFIG_SDCARD_HW_SEED != 0 ? (uint32_t)CONFIG_SDCARD_HW_SEED
					      : (k_cycle_get_32() | 1U);
	print_setup();
	done = true;
	return NULL;
}

void sdq_need_scratch(void)
{
	if (!sdq.scratch_ok) {
		ztest_test_skip();
	}
}

/**
 * @brief Tell whether the card answers correctly: status OK and block 0
 *        reads back as it did at setup.
 *
 * @return True if healthy.
 */
static bool card_healthy(void)
{
	static uint8_t blk[SDQ_BLK] __aligned(4);

	return disk_access_status(SDQ_DISK) == DISK_STATUS_OK &&
	       disk_access_read(SDQ_DISK, blk, 0, 1) == 0 && memcmp(blk, block0, SDQ_BLK) == 0;
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
 * @return Words removed.
 */
static int drain_fifo(void)
{
	uintptr_t regs = sdq_sdhost_regs();
	int words = 0;

	while (hal_sdhost_fifo_level(regs) != 0U && words < 4096) {
		(void)hal_sdhost_fifo_read(regs);
		words++;
	}
	return words;
}

void sdq_before(void *f)
{
	int ret;

	ARG_UNUSED(f);
	ubsan_driver_overflows = 0;
	if (card_healthy()) {
		return;
	}
	sdq.recoveries++;
	TC_PRINT("RECOVERY: the card did not answer correctly after the previous test; "
		 "power cycling it through the disk API\n");
	ret = power_cycle();
	if (ret == 0 && card_healthy()) {
		return;
	}
	TC_PRINT("RECOVERY: re-init gave %d; emptying %d stale FIFO words and retrying\n", ret,
		 drain_fifo());
	ret = power_cycle();
	if (ret != 0 || !card_healthy()) {
		/* Not an assertion: ztest reports a failure in before() as a
		 * skip, which would hide it. The test runs, and fails on the
		 * card itself.
		 */
		sdq.recovery_failures++;
		TC_PRINT("RECOVERY FAILED (%d): the card is still unusable; this test will fail "
			 "too\n",
			 ret);
	}
}

void sdq_after(void *f)
{
	ARG_UNUSED(f);
	zassert_equal(ubsan_driver_overflows, 0,
		      "signed integer overflow in the driver (%d, last at line %u)",
		      ubsan_driver_overflows, ubsan_last_line);
}
