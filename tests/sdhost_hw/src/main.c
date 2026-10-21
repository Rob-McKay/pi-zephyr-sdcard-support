/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The findings of docs/reviews/pr110196-sdhost-review.md, reproduced on a
 * real Raspberry Pi 3A+ with a real SD card. The PR's SDHost driver runs
 * unmodified (only built with -fsanitize=signed-integer-overflow), through
 * the SD stack for card init and through raw sdhc_request() calls, so each
 * test sees exactly what the driver returns.
 *
 * Each review test asserts the correct behaviour: a FAIL means the finding
 * reproduced on this Pi and card, a PASS that it did not. Some findings
 * depend on the card or the configuration; the test prints what it saw.
 *
 * WRITES TO THE CARD: the last SCRATCH_BLOCKS blocks (16 MB), only if no
 * partition reaches them. Use a spare card.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/dt-bindings/pinctrl/bcm2711-pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sd/sdmmc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/ztest.h>

#include <hal/bcm2835_gpio.h>
#include <hal/bcm2835_sdhost.h>
#include <rpi_fw.h>

#include "ubsan.h"

#define BLK 512

/* Scratch area at the end of the card: pattern blocks, then finding 1's
 * write area.
 */
#define SCRATCH_BLOCKS 32768
#define PAT_BLOCKS     256
#define F1_CHUNK       256 /* 128 KB per write */
#define F1_CHUNKS      ((SCRATCH_BLOCKS - PAT_BLOCKS) / F1_CHUNK)

/* GPIO 26, header pin 37: no on-board function on the 3A+. */
#define FREE_PIN      26

#define FW_CLOCK_CORE 4

static const struct device *const sdhc = DEVICE_DT_GET(DT_ALIAS(sdhc0));
static const struct device *const fw = DEVICE_DT_GET(DT_NODELABEL(firmware));
static struct sd_card card;

static uint32_t scratch_start;
static bool scratch_ok;
static const char *scratch_why = "not checked";
static bool pattern_written;

static uint8_t buf[F1_CHUNK * BLK] __aligned(4);
static uint8_t big[2048 * BLK] __aligned(4);

/* ===== Helpers ====================================================== */

/**
 * @brief Convert a block number to the card's command argument.
 *
 * @param lba Block number.
 * @return @p lba for SDHC/SDXC, the byte address for SDSC.
 */
static uint32_t card_addr(uint32_t lba)
{
	return (card.flags & (uint32_t)SD_HIGH_CAPACITY_FLAG) ? lba : lba * BLK;
}

/**
 * @brief Send one data command straight to the driver, bypassing the SD stack.
 *
 * @param opcode Command opcode.
 * @param lba First block.
 * @param blocks Block count.
 * @param block_size Block size in bytes.
 * @param data Data buffer.
 * @param data_timeout_ms Data timeout.
 * @return The driver's sdhc_request() result.
 */
static int raw_xfer(uint32_t opcode, uint32_t lba, uint32_t blocks, uint32_t block_size,
		    void *data, int data_timeout_ms)
{
	struct sdhc_command cmd = {
		.opcode = opcode,
		.arg = card_addr(lba),
		.response_type = (uint32_t)SD_RSP_TYPE_R1 | (uint32_t)SD_SPI_RSP_TYPE_R1,
		.timeout_ms = CONFIG_SD_CMD_TIMEOUT,
	};
	struct sdhc_data d = {
		.block_size = block_size,
		.blocks = blocks,
		.data = data,
		.timeout_ms = data_timeout_ms,
	};

	return sdhc_request(sdhc, &cmd, &d);
}

/**
 * @brief Read blocks straight through the driver (CMD17, or CMD18 + CMD12).
 *
 * @param lba First block.
 * @param blocks Block count.
 * @param[out] data Buffer of @p blocks blocks.
 * @param data_timeout_ms Data timeout.
 * @return The driver's sdhc_request() result.
 */
static int raw_read(uint32_t lba, uint32_t blocks, void *data, int data_timeout_ms)
{
	return raw_xfer(blocks == 1 ? SD_READ_SINGLE_BLOCK : SD_READ_MULTIPLE_BLOCK, lba, blocks,
			BLK, data, data_timeout_ms);
}

/**
 * @brief Read the card's current state with CMD13.
 *
 * @param cmd_timeout_ms Command timeout.
 * @param[out] state Card state (enum sd_r1_current_state), set on success.
 * @return The driver's sdhc_request() result.
 */
static int card_state(int cmd_timeout_ms, int *state)
{
	struct sdhc_command cmd = {
		.opcode = SD_SEND_STATUS,
		.arg = (uint32_t)card.relative_addr << 16,
		.response_type = (uint32_t)SD_RSP_TYPE_R1 | (uint32_t)SD_SPI_RSP_TYPE_R2,
		.timeout_ms = cmd_timeout_ms,
	};
	int ret = sdhc_request(sdhc, &cmd, NULL);

	if (ret == 0) {
		*state = SD_R1_CURRENT_STATE(cmd.response[0]);
	}
	return ret;
}

/**
 * @brief Fill a buffer with pattern blocks.
 *
 * Each block carries a tag and its own block number, then data unique to
 * it, so a block returned in place of another is caught.
 *
 * @param[out] p Buffer of @p blocks blocks.
 * @param lba Block number of the first block.
 * @param blocks Block count.
 */
static void fill_pattern(uint8_t *p, uint32_t lba, uint32_t blocks)
{
	for (uint32_t b = 0; b < blocks; b++, lba++) {
		uint32_t *w = (uint32_t *)(p + ((size_t)b * BLK));

		w[0] = 0x5d405d40;
		w[1] = lba;
		for (int i = 2; i < BLK / 4; i++) {
			w[i] = (lba * 0x9e3779b1U) + i;
		}
	}
}

/**
 * @brief Compare a buffer with the pattern blocks it should hold.
 *
 * @param p Buffer of @p blocks blocks.
 * @param lba Block number of the first block.
 * @param blocks Block count.
 * @retval -1 All blocks match.
 * @return Otherwise the index of the first bad block.
 */
static int check_pattern(const uint8_t *p, uint32_t lba, uint32_t blocks)
{
	static uint8_t want[BLK] __aligned(4);

	for (uint32_t b = 0; b < blocks; b++) {
		fill_pattern(want, lba + b, 1);
		if (memcmp(p + ((size_t)b * BLK), want, BLK) != 0) {
			return (int)b;
		}
	}
	return -1;
}

/**
 * @brief Give the block number a pattern block claims to be.
 *
 * @param p One block.
 * @return Its block number, or UINT32_MAX if it isn't a pattern block.
 */
static uint32_t pattern_lba(const uint8_t *p)
{
	const uint32_t *w = (const uint32_t *)p;

	return w[0] == 0x5d405d40 ? w[1] : UINT32_MAX;
}

/**
 * @brief Tell whether a block is all zeroes.
 *
 * @param blk One block.
 * @return True if every byte is 0.
 */
static bool block_is_blank(const uint8_t *blk)
{
	for (int i = 0; i < BLK; i++) {
		if (blk[i] != 0) {
			return false;
		}
	}
	return true;
}

/**
 * @brief Tell whether any primary partition of an MBR reaches a block.
 *
 * @param mbr Block 0, with a valid MBR signature.
 * @param lba Block number.
 * @return True if a used partition entry ends after @p lba.
 */
static bool mbr_partition_reaches(const uint8_t *mbr, uint32_t lba)
{
	for (int i = 0; i < 4; i++) {
		const uint8_t *e = &mbr[446 + (i * 16)];
		uint32_t start = sys_get_le32(e + 8);
		uint32_t count = sys_get_le32(e + 12);

		if (e[4] != 0 && count != 0 && (uint64_t)start + count > lba) {
			return true;
		}
	}
	return false;
}

/**
 * @brief Decide whether the scratch area may be written.
 *
 * It may if no MBR partition reaches it; a card without an MBR is used
 * only if block 0 is blank. Sets scratch_start, scratch_ok and, if not
 * usable, scratch_why.
 */
static void check_scratch(void)
{
	static uint8_t mbr[BLK] __aligned(4);

	scratch_ok = false;
	if (card.block_count < 2 * SCRATCH_BLOCKS) {
		scratch_why = "card too small";
		return;
	}
	scratch_start = card.block_count - SCRATCH_BLOCKS;
	if (sdmmc_read_blocks(&card, mbr, 0, 1) != 0) {
		scratch_why = "MBR read failed";
		return;
	}
	if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
		if (!block_is_blank(mbr)) {
			scratch_why = "no MBR, and block 0 is not blank";
			return;
		}
		scratch_ok = true;
		return;
	}
	if (mbr_partition_reaches(mbr, scratch_start)) {
		scratch_why = "a partition reaches the last 16 MB of the card";
		return;
	}
	scratch_ok = true;
}

/** @brief Skip the test unless the scratch area may be written. */
static void need_scratch(void)
{
	if (!scratch_ok) {
		ztest_test_skip();
	}
}

/** @brief Write the pattern blocks through the SD stack and verify them. */
static void write_pattern_blocks(void)
{
	for (uint32_t b = 0; b < PAT_BLOCKS; b += 64) {
		fill_pattern(buf, scratch_start + b, 64);
		zassert_ok(sdmmc_write_blocks(&card, buf, scratch_start + b, 64),
			   "writing the pattern blocks failed");
	}
	zassert_ok(sdmmc_read_blocks(&card, buf, scratch_start, PAT_BLOCKS));
	zassert_equal(check_pattern(buf, scratch_start, PAT_BLOCKS), -1,
		      "pattern blocks did not read back");
}

/** @brief Skip the test without a scratch area, else write the pattern once. */
static void need_pattern(void)
{
	need_scratch();
	if (pattern_written) {
		return;
	}
	write_pattern_blocks();
	pattern_written = true;
}

/**
 * @brief Map a register block uncached.
 *
 * @param phys Physical address.
 * @param size Size in bytes.
 * @return Virtual address.
 */
static uintptr_t map(uintptr_t phys, size_t size)
{
	uintptr_t virt;

	device_map(&virt, phys, size, K_MEM_CACHE_NONE);
	return virt;
}

/**
 * @brief Give the SDHost registers' address, mapping them on first use.
 *
 * @return Virtual base address.
 */
static uintptr_t sdhost_regs(void)
{
	static uintptr_t base;

	if (base == 0) {
		base = map(DT_REG_ADDR(DT_NODELABEL(sdhost)), DT_REG_SIZE(DT_NODELABEL(sdhost)));
	}
	return base;
}

/**
 * @brief Give the GPIO registers' address, mapping them on first use.
 *
 * @return Virtual base address.
 */
static uintptr_t gpio_regs(void)
{
	static uintptr_t base;

	if (base == 0) {
		base = map(DT_REG_ADDR(DT_NODELABEL(pinctrl)), DT_REG_SIZE(DT_NODELABEL(pinctrl)));
	}
	return base;
}

/**
 * @brief Ask the firmware for the real VPU core clock.
 *
 * @return Core clock in Hz.
 */
static uint32_t core_clock_hz(void)
{
	uint32_t req[2] = {FW_CLOCK_CORE, 0};

	zassert_ok(rpi_fw_transfer(fw, RPI_FW_TAG_GET_CLOCK_RATE, req, sizeof(req)),
		   "firmware clock query failed");
	return req[1];
}

/**
 * @brief Compute the SD bus clock as programmed.
 *
 * @return The real core clock over SDCDIV + 2, in Hz.
 */
static uint32_t bus_clock_hz(void)
{
	return core_clock_hz() / (hal_sdhost_clock_divider(sdhost_regs()) + 2);
}

/**
 * @brief Make a multi-block read fail part-way, for finding 2.
 *
 * A data timeout of SDHC_TIMEOUT_FOREVER fails at the first FIFO stall
 * (finding 5); if that is fixed, a 1 ms timeout on a long read is tried.
 *
 * @param lba First block to read.
 * @return The last read's result: nonzero if a read failed.
 */
static int provoke_failed_read(uint32_t lba)
{
	int ret = raw_read(lba, 4, buf, SDHC_TIMEOUT_FOREVER);

	if (ret == 0) {
		ret = raw_read(lba, PAT_BLOCKS, buf, 1);
	}
	return ret;
}

/**
 * @brief Empty the controller's FIFO.
 *
 * @return Number of words removed.
 */
static int drain_fifo(void)
{
	uintptr_t base = sdhost_regs();
	int words = 0;

	while (hal_sdhost_fifo_level(base) != 0 && words < 4096) {
		(void)hal_sdhost_fifo_read(base);
		words++;
	}
	return words;
}

/** @brief Send CMD0, which stops a card still sending data; ignore the result. */
static void go_idle(void)
{
	struct sdhc_command cmd = {
		.opcode = SD_GO_IDLE_STATE,
		.response_type = (uint32_t)SD_RSP_TYPE_NONE | (uint32_t)SD_SPI_RSP_TYPE_R1,
		.timeout_ms = CONFIG_SD_CMD_TIMEOUT,
	};

	(void)sdhc_request(sdhc, &cmd, NULL);
}

/**
 * @brief Put the controller and card back to a known state.
 *
 * Neither the driver's reset nor sd_init() empties the FIFO (finding 2),
 * and stale words there corrupt the SCR and switch-status reads of the
 * next card init, so the harness does it, around a CMD0.
 */
static void harness_reset(void)
{
	int words = drain_fifo();

	go_idle();
	k_busy_wait(1000);
	words += drain_fifo();
	if (words != 0) {
		TC_PRINT("(harness: emptied %d stale words from the SDHost FIFO)\n", words);
	}
}

/** @brief Print the card, bus and scratch area the tests use. */
static void print_setup(void)
{
	TC_PRINT("card: %u blocks (%u MB), %s, bus %u-bit at %u Hz, core clock %u Hz\n",
		 card.block_count, card.block_count / 2048,
		 (card.flags & (uint32_t)SD_HIGH_CAPACITY_FLAG) ? "SDHC/SDXC" : "SDSC",
		 card.bus_io.bus_width == SDHC_BUS_WIDTH4BIT ? 4 : 1, card.bus_io.clock,
		 core_clock_hz());
	if (scratch_ok) {
		TC_PRINT("scratch area: blocks %u-%u (written by these tests)\n", scratch_start,
			 card.block_count - 1);
	} else {
		TC_PRINT("no scratch area (%s): write tests are skipped\n", scratch_why);
	}
}

/**
 * @brief Suite setup: init the card and, once, check the scratch area.
 *
 * @return NULL (no fixture).
 */
static void *suite_setup(void)
{
	static bool checked;

	zassert_true(device_is_ready(sdhc), "SDHost not ready");
	zassert_true(device_is_ready(fw), "firmware mailbox not ready");
	harness_reset();
	zassert_ok(sd_init(sdhc, &card), "no card, or card init failed");
	if (!checked) {
		check_scratch();
		checked = true;
	}
	print_setup();
	return NULL;
}

/**
 * @brief Reset the controller and re-init the card before each test.
 *
 * A reproduced finding can leave either wedged.
 *
 * @param f Unused fixture.
 */
static void before(void *f)
{
	ARG_UNUSED(f);
	ubsan_driver_overflows = 0;
	harness_reset();
	zassert_ok(sd_init(sdhc, &card), "card re-init failed: a previous test left it wedged");
}

/* ===== Sanity: the driver works on this Pi and card ================= */

/** @brief The card is in the transfer state after init. */
ZTEST(sdhost_hw_basic, test_card_in_transfer_state)
{
	int state = -1;

	zassert_ok(card_state(CONFIG_SD_CMD_TIMEOUT, &state));
	zassert_equal(state, SDMMC_R1_TRANSFER, "card state %d after init", state);
}

/** @brief The pattern blocks read back through the SD stack. */
ZTEST(sdhost_hw_basic, test_pattern_reads_back)
{
	need_pattern();
	memset(buf, 0, (size_t)PAT_BLOCKS * BLK);
	zassert_ok(sdmmc_read_blocks(&card, buf, scratch_start, PAT_BLOCKS));
	zassert_equal(check_pattern(buf, scratch_start, PAT_BLOCKS), -1);
}

/** @brief A raw CMD18 + CMD12 read returns the data and leaves the card in transfer state. */
ZTEST(sdhost_hw_basic, test_raw_multi_block_read_and_stop)
{
	int state = -1;

	need_pattern();
	zassert_ok(raw_read(scratch_start + 8, 8, buf, CONFIG_SD_DATA_TIMEOUT));
	zassert_equal(check_pattern(buf, scratch_start + 8, 8), -1);
	zassert_ok(card_state(CONFIG_SD_CMD_TIMEOUT, &state));
	zassert_equal(state, SDMMC_R1_TRANSFER, "card state %d after CMD18 + CMD12", state);
}

ZTEST_SUITE(sdhost_hw_basic, NULL, suite_setup, before, NULL, NULL);

/* ===== Review findings ============================================== */

/**
 * @brief Give the first block of finding 1's i-th write.
 *
 * Visits the chunks out of order (37 is coprime to F1_CHUNKS).
 *
 * @param i Write number, 0 to F1_CHUNKS - 1.
 * @return Block number.
 */
static uint32_t f1_chunk_lba(int i)
{
	return scratch_start + PAT_BLOCKS + (((i * 37) % F1_CHUNKS) * F1_CHUNK);
}

/**
 * @brief Poll CMD13 for up to 1 s from @p t0 until the card is in transfer state.
 *
 * @param t0 Start time (k_uptime_get()).
 * @return The last state read, or -1 if no CMD13 succeeded.
 */
static int wait_transfer_state(int64_t t0)
{
	int state = -1;

	while (k_uptime_get() - t0 < 1000) {
		if (card_state(CONFIG_SD_CMD_TIMEOUT, &state) == 0 &&
		    state == SDMMC_R1_TRANSFER) {
			break;
		}
	}
	return state;
}

/**
 * @brief Measure and report how long the card stayed busy after a failed write.
 *
 * @param i Write number.
 * @param lba First block of the write.
 * @param ret The write's result.
 * @return Milliseconds until the card was back in transfer state (or 1 s).
 */
static int64_t f1_measure_busy(int i, uint32_t lba, int ret)
{
	int64_t t0 = k_uptime_get();
	int state = wait_transfer_state(t0);
	int64_t busy = k_uptime_get() - t0;

	TC_PRINT("write %d (block %u): %d; card back in transfer state after a further "
		 "%lld ms (state %d)\n", i, lba, ret, k_uptime_get() - t0, state);
	return busy;
}

/**
 * @brief Finding 1: 128 KB multi-block writes survive the card's busy time.
 *
 * CMD12 after a multi-block write waits only 100 ms for busy; cards may
 * stay busy for 250 ms (SDHC) or 500 ms (SDXC) (SD Physical Layer
 * Simplified Spec v9.10, 4.6.2.2). Whether this one does depends on the
 * card, so many writes are spread over the scratch area.
 */
ZTEST(sdhost_hw_review, test_f1_multi_block_write_busy)
{
	int failures = 0;
	int64_t worst_busy = 0;

	need_scratch();
	for (int i = 0; i < F1_CHUNKS; i++) {
		uint32_t lba = f1_chunk_lba(i);
		int ret;

		fill_pattern(buf, lba, F1_CHUNK);
		ret = raw_xfer(SD_WRITE_MULTIPLE_BLOCK, lba, F1_CHUNK, BLK, buf,
			       CONFIG_SD_DATA_TIMEOUT);
		if (ret == 0) {
			continue;
		}
		worst_busy = MAX(worst_busy, f1_measure_busy(i, lba, ret));
		failures++;
		harness_reset();
		zassert_ok(sd_init(sdhc, &card), "card re-init failed");
	}
	zassert_equal(failures, 0,
		      "%d of %d multi-block writes failed; the card stayed busy up to %lld ms more",
		      failures, F1_CHUNKS, worst_busy);
}

/**
 * @brief Finding 2: the card is back in transfer state after a failed read.
 *
 * Today no CMD12 follows the failure, so the card is left sending data.
 */
ZTEST(sdhost_hw_review, test_f2_failed_read_leaves_card_in_data_state)
{
	int state;
	int ret;

	need_pattern();
	ret = provoke_failed_read(scratch_start);
	if (ret == 0) {
		ztest_test_skip(); /* could not make a read fail */
	}
	TC_PRINT("multi-block read failed with %d\n", ret);

	ret = card_state(CONFIG_SD_CMD_TIMEOUT, &state);
	zassert_ok(ret, "CMD13 after the failed read: %d (controller not recovered)", ret);
	zassert_equal(state, SDMMC_R1_TRANSFER,
		      "card left in state %d (5 = sending data): no CMD12 after the failure",
		      state);
}

/**
 * @brief Finding 2: the read after a failed read returns its own block.
 *
 * Nothing drains the FIFO after a failure, so it can return the failed
 * read's data.
 */
ZTEST(sdhost_hw_review, test_f2_next_read_after_failure)
{
	const uint32_t b = scratch_start + 100;
	int ret;

	need_pattern();
	ret = provoke_failed_read(scratch_start);
	if (ret == 0) {
		ztest_test_skip();
	}
	TC_PRINT("multi-block read of block %u failed with %d\n", scratch_start, ret);

	memset(buf, 0, BLK);
	ret = raw_read(b, 1, buf, CONFIG_SD_DATA_TIMEOUT);
	zassert_ok(ret, "next read failed with %d: the driver did not recover", ret);
	zassert_equal(check_pattern(buf, b, 1), -1,
		      "read of block %u returned success with the data of block %d (0x%08x)", b,
		      (int)pattern_lba(buf), ((uint32_t *)buf)[0]);
}

/**
 * @brief Finding 3: the SD bus runs no faster than requested.
 *
 * The driver divides the devicetree clock-frequency (250 MHz), not the
 * real core clock; with another core_freq in config.txt the bus can exceed
 * 50 MHz (SD Physical Layer Simplified Spec v9.10, 4.3.11).
 */
ZTEST(sdhost_hw_review, test_f3_bus_clock_within_request)
{
	uint32_t core = core_clock_hz();
	uint32_t actual = bus_clock_hz();

	TC_PRINT("core clock %u Hz (devicetree says %u), SD bus asked for %u Hz, runs at %u Hz\n",
		 core, DT_PROP(DT_NODELABEL(sdhost), clock_frequency), card.bus_io.clock, actual);
	zassert_true(actual <= card.bus_io.clock, "SD bus overclocked: %u Hz for %u Hz", actual,
		     card.bus_io.clock);
}

/**
 * @brief Finding 4: the hardware data timeout covers the card's write busy.
 *
 * SDTOUT keeps its reset value, 0xf00000 bus clocks: 314 ms at 50 MHz,
 * less than an SDXC card's 500 ms (SD Physical Layer Simplified Spec
 * v9.10, 4.6.2.2; SDXC is a card over 32 GB, 3.3.2).
 */
ZTEST(sdhost_hw_review, test_f4_data_timeout_covers_write_busy)
{
	bool sdxc = card.block_count > (32ULL << 30) / BLK;
	uint32_t need_ms = sdxc ? 500 : 250;
	uint32_t tout = hal_sdhost_data_timeout(sdhost_regs());
	uint32_t bus = bus_clock_hz();
	uint32_t tout_ms = (uint32_t)((uint64_t)tout * 1000 / bus);

	TC_PRINT("SDTOUT 0x%08x at %u Hz = %u ms; %s write busy may last %u ms\n", tout, bus,
		 tout_ms, sdxc ? "SDXC" : "SDHC/SDSC", need_ms);
	zassert_true(tout_ms >= need_ms, "hardware data timeout %u ms < %u ms", tout_ms, need_ms);
}

/**
 * @brief Finding 5: a read with SDHC_TIMEOUT_FOREVER succeeds.
 *
 * Today -1 is a deadline in the past: the read gives up at the first
 * moment the FIFO is empty.
 */
ZTEST(sdhost_hw_review, test_f5_read_with_timeout_forever)
{
	int ret;

	need_pattern();
	memset(buf, 0, BLK);
	ret = raw_read(scratch_start + 1, 1, buf, SDHC_TIMEOUT_FOREVER);
	zassert_ok(ret, "read with SDHC_TIMEOUT_FOREVER failed with %d", ret);
	zassert_equal(check_pattern(buf, scratch_start + 1, 1), -1);
}

/** @brief Finding 5: a 3,000,000 ms timeout causes no int overflow in timeout_ms * 1000. */
ZTEST(sdhost_hw_review, test_f5_timeout_multiplication_overflow)
{
	int state;
	int ret = card_state(3000000, &state);

	TC_PRINT("CMD13 with a 3,000,000 ms timeout: %d\n", ret);
	zassert_equal(ubsan_driver_overflows, 0,
		      "signed overflow (undefined behaviour) at sdhc_bcm2835_sdhost.c:%u",
		      ubsan_last_line);
	zassert_ok(ret);
}

/* Finding 6: the driver busy-waits for whole transfers. */
/* Zephyr's definition macro can end in an empty statement: ISO C -Wpedantic
 * flags the ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static K_THREAD_STACK_DEFINE(low_stack, 2048);
#pragma GCC diagnostic pop
static struct k_thread low_thread;
static atomic_t low_runs;

/**
 * @brief Lower-priority thread: count each 1 ms wake-up.
 *
 * @param a Unused.
 * @param b Unused.
 * @param c Unused.
 */
static void low_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_msleep(1);
		atomic_inc(&low_runs);
	}
}

/** @brief Read the first 1 MB of the card into big, 256 blocks at a time. */
static void read_big(void)
{
	const uint32_t blocks = sizeof(big) / BLK;

	for (uint32_t b = 0; b < blocks; b += 256) {
		zassert_ok(raw_read(b, 256, big + ((size_t)b * BLK), CONFIG_SD_DATA_TIMEOUT));
	}
}

/** @brief Finding 6: a lower-priority thread runs during a 1 MB read. */
ZTEST(sdhost_hw_review, test_f6_read_starves_lower_priority)
{
	int64_t t0;
	int64_t ms;
	int runs;

	k_thread_create(&low_thread, low_stack, K_THREAD_STACK_SIZEOF(low_stack), low_entry, NULL,
			NULL, NULL, CONFIG_ZTEST_THREAD_PRIORITY + 1, 0, K_NO_WAIT);
	k_msleep(20);

	atomic_set(&low_runs, 0);
	t0 = k_uptime_get();
	read_big();
	ms = k_uptime_get() - t0;
	runs = (int)atomic_get(&low_runs);
	k_thread_abort(&low_thread);

	TC_PRINT("1 MB read in %lld ms (%lld KB/s); lower-priority thread ran %d times\n", ms,
		 ms ? INT64_C(1024) * 1000 / ms : 0, runs);
	zassert_true(runs >= ms / 4,
		     "lower-priority thread ran %d times in %lld ms (wants the CPU every 1 ms)",
		     runs, ms);
}

/* Finding 7: the pinctrl driver writes the BCM2711's pull registers. */

/**
 * @brief Set FREE_PIN's pull with the BCM2837 GPPUD/GPPUDCLK0 sequence.
 *
 * @param gpio GPIO register base.
 * @param pull BCM2835_GPIO_PULL_OFF, _DOWN or _UP.
 */
static void legacy_pull(uintptr_t gpio, uint32_t pull)
{
	hal_gpio_pull_select(gpio, pull);
	k_busy_wait(10);
	hal_gpio_pull_clock(gpio, FREE_PIN);
	k_busy_wait(10);
	hal_gpio_pull_select(gpio, BCM2835_GPIO_PULL_OFF);
	hal_gpio_pull_clock_end(gpio, FREE_PIN);
	k_busy_wait(100);
}

/**
 * @brief Read FREE_PIN's level.
 *
 * @param gpio GPIO register base.
 * @return True if high.
 */
static bool pin_level(uintptr_t gpio)
{
	return hal_gpio_level(gpio, FREE_PIN);
}

/**
 * @brief Make FREE_PIN an input and check it follows the legacy pulls.
 *
 * If it doesn't, something is connected to it. Leaves it pulled down.
 *
 * @param gpio GPIO register base.
 * @return True if the pin follows its pull.
 */
static bool free_pin_follows_pull(uintptr_t gpio)
{
	bool up, down;

	hal_gpio_set_input(gpio, FREE_PIN);
	legacy_pull(gpio, BCM2835_GPIO_PULL_UP);
	up = pin_level(gpio);
	legacy_pull(gpio, BCM2835_GPIO_PULL_DOWN);
	down = pin_level(gpio);
	if (!up || down) {
		TC_PRINT("GPIO %d (header pin 37) does not follow its pull (up %d, down %d): "
			 "disconnect it\n", FREE_PIN, up, down);
		return false;
	}
	return true;
}

/**
 * @brief Finding 7: pinctrl's pull-up takes effect on the BCM2837B0.
 *
 * Shown on a free GPIO, whose pull is visible in GPLEV0: the SD lines
 * have pull-ups on the card itself.
 */
ZTEST(sdhost_hw_review, test_f7_pinctrl_pull_up_on_bcm2837)
{
	const pinctrl_soc_pin_t pin = {
		.pin = FREE_PIN, .func = BCM2711_FSEL_GPIO_IN, .pull = BCM2711_PULL_UP,
	};
	uintptr_t gpio = gpio_regs();
	bool after;

	if (!free_pin_follows_pull(gpio)) {
		ztest_test_skip();
	}

	zassert_ok(pinctrl_configure_pins(&pin, 1, 0));
	k_busy_wait(100);
	after = pin_level(gpio);
	legacy_pull(gpio, BCM2835_GPIO_PULL_DOWN); /* back to the reset default */

	zassert_true(after, "GPIO %d still low after pinctrl asked for a pull-up", FREE_PIN);
}

/* Finding 9: the driver has no lock. */
#define F9_READS  20
#define F9_BLOCKS 32

struct f9_ctx {
	uint32_t lba;
	uint8_t *data;
	int errors;
	int wrong;
	int first_err;
};

/* Zephyr's definition macro can end in an empty statement: ISO C -Wpedantic
 * flags the ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static K_THREAD_STACK_ARRAY_DEFINE(f9_stacks, 2, 4096);
#pragma GCC diagnostic pop
static struct k_thread f9_threads[2];

/**
 * @brief Finding 9 reader thread: read and check its blocks F9_READS times.
 *
 * @param[in,out] p1 Its struct f9_ctx, where it counts failures.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void f9_entry(void *p1, void *p2, void *p3)
{
	struct f9_ctx *c = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (int i = 0; i < F9_READS; i++) {
		int ret = raw_read(c->lba, F9_BLOCKS, c->data, CONFIG_SD_DATA_TIMEOUT);

		if (ret != 0) {
			if (c->errors++ == 0) {
				c->first_err = ret;
			}
		} else if (check_pattern(c->data, c->lba, F9_BLOCKS) != -1) {
			c->wrong++;
		}
	}
}

/**
 * @brief Run both finding 9 readers to completion.
 *
 * @param[in,out] ctx The two threads' contexts.
 */
static void f9_run(struct f9_ctx ctx[2])
{
	for (int i = 0; i < 2; i++) {
		k_thread_create(&f9_threads[i], f9_stacks[i], K_THREAD_STACK_SIZEOF(f9_stacks[i]),
				f9_entry, &ctx[i], NULL, NULL, CONFIG_ZTEST_THREAD_PRIORITY, 0,
				K_NO_WAIT);
	}
	for (int i = 0; i < 2; i++) {
		k_thread_join(&f9_threads[i], K_FOREVER);
	}
}

/**
 * @brief Finding 9: two threads reading at once all get their own data.
 *
 * Both run at the same priority, time-sliced every 1 ms, so the slices
 * land in the middle of each other's transfers.
 */
ZTEST(sdhost_hw_review, test_f9_concurrent_requests)
{
	struct f9_ctx ctx[2] = {
		{.lba = scratch_start, .data = big},
		{.lba = scratch_start + 128, .data = big + ((size_t)F9_BLOCKS * BLK)},
	};

	need_pattern();
	f9_run(ctx);

	for (int i = 0; i < 2; i++) {
		TC_PRINT("thread %d: %d of %d reads failed (first %d), %d returned wrong data\n", i,
			 ctx[i].errors, F9_READS, ctx[i].first_err, ctx[i].wrong);
	}
	zassert_true(ctx[0].errors + ctx[1].errors + ctx[0].wrong + ctx[1].wrong == 0,
		     "concurrent requests failed or returned wrong data");
}

/**
 * @brief Finding 10: a block size over the host's 1024-byte limit is refused.
 *
 * Today it is sent to the card.
 */
ZTEST(sdhost_hw_review, test_f10_block_size_over_limit)
{
	int ret = raw_xfer(SD_READ_SINGLE_BLOCK, 0, 1, 2048, buf, 1000);

	zassert_equal(ret, -EINVAL, "2048-byte block: got %d, not -EINVAL", ret);
}

ZTEST_SUITE(sdhost_hw_review, NULL, suite_setup, before, NULL, NULL);
