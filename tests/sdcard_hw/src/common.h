/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared state and helpers of the SD card qualification suites: the disk,
 * the scratch area the write tests may use, pattern blocks that say which
 * block and which write they came from, buffers, timing and a seeded
 * random generator.
 */

#ifndef SDCARD_HW_COMMON_H_
#define SDCARD_HW_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/** The card's disk name (board devicetree, sdmmc node). */
#define SDQ_DISK "SD"

/** Block size: every SD card since SDHC uses 512 bytes. */
#define SDQ_BLK 512U

/** Largest transfer the tests make: 1 MB. */
#define SDQ_MAX_BLOCKS 2048U

/** Blocks in the scratch area. */
#define SDQ_SCRATCH_BLOCKS ((uint32_t)CONFIG_SDCARD_HW_SCRATCH_MB * 2048U)

/** Write and read buffers, SDQ_MAX_BLOCKS blocks plus room to misalign. */
extern uint8_t sdq_wbuf[];
extern uint8_t sdq_rbuf[];

/** What the suites know about the card. */
struct sdq_card {
	uint32_t sectors;     /**< capacity in blocks */
	uint32_t sector_size; /**< block size the disk reports */
	uint32_t scratch;     /**< first block of the scratch area */
	bool scratch_ok;      /**< the write tests may use it */
	const char *why;      /**< why not, if not */
	uint32_t seed;        /**< random seed of this run */
	uint32_t recoveries;  /**< times a test found the card unusable */
	uint32_t recovery_failures; /**< times recovering it didn't work */
};

extern struct sdq_card sdq;

/**
 * @brief Suite setup shared by every suite: initialise the disk and, once,
 *        decide whether the scratch area may be written.
 *
 * @return NULL (no fixture).
 */
void *sdq_setup(void);

/**
 * @brief Per-test setup: check the card still answers, recovering it
 *        (power cycle and re-init) if a previous test left it unusable.
 *
 * Never fails: ztest would report the test as skipped. If recovery
 * doesn't work, it says so and the test fails on the card.
 *
 * @param f Unused fixture.
 */
void sdq_before(void *f);

/**
 * @brief Per-test teardown: fail if the driver overflowed a signed integer.
 *
 * @param f Unused fixture.
 */
void sdq_after(void *f);

/** @brief Skip the test unless the scratch area may be written. */
void sdq_need_scratch(void);

/**
 * @brief Fill blocks with pattern data.
 *
 * Each block holds a tag, its own block number, a generation number and
 * data derived from both, so a block read from the wrong place, or an
 * older write of the right place, is recognised.
 *
 * @param[out] p Buffer of @p blocks blocks (any alignment).
 * @param lba Block number of the first block.
 * @param blocks Block count.
 * @param gen Generation (write number).
 */
void sdq_fill(uint8_t *p, uint32_t lba, uint32_t blocks, uint32_t gen);

/**
 * @brief Check blocks against the pattern they should hold.
 *
 * Prints what the first bad block holds instead (another block's data,
 * an older generation, erased or unknown data).
 *
 * @param p Buffer of @p blocks blocks.
 * @param lba Block number of the first block.
 * @param blocks Block count.
 * @param gen Generation expected.
 * @retval -1 All blocks match.
 * @return Otherwise the index of the first bad block.
 */
int sdq_check(const uint8_t *p, uint32_t lba, uint32_t blocks, uint32_t gen);

/**
 * @brief Write and read back blocks of the scratch area through the disk
 *        API, asserting both succeed and the data matches.
 *
 * @param lba First block.
 * @param blocks Block count, at most SDQ_MAX_BLOCKS.
 * @param gen Generation to write.
 */
void sdq_write_verify(uint32_t lba, uint32_t blocks, uint32_t gen);

/**
 * @brief Next number from the run's random generator (xorshift32).
 *
 * @param[in,out] state Generator state, never 0.
 * @return Next number.
 */
uint32_t sdq_rand(uint32_t *state);

/**
 * @brief Microseconds since boot, in kernel ticks (100 us).
 *
 * @return Time in microseconds.
 */
uint64_t sdq_now_us(void);

/**
 * @brief Throughput in kB/s (1000 bytes) of a transfer.
 *
 * @param bytes Bytes moved.
 * @param us Time taken, in microseconds.
 * @return kB/s, or 0 for no time.
 */
uint32_t sdq_kbps(uint64_t bytes, uint64_t us);

/**
 * @brief Tell whether a block holds one byte value throughout.
 *
 * @param p One block.
 * @param value Byte value.
 * @return True if every byte is @p value.
 */
bool sdq_block_is(const uint8_t *p, uint8_t value);

/**
 * @brief Give the SDHost registers' address, mapping them on first use.
 *
 * @return Virtual base address.
 */
uintptr_t sdq_sdhost_regs(void);

/**
 * @brief Ask the firmware for the VPU core clock, the SDHost's input.
 *
 * @return Core clock in Hz, or 0 if the firmware didn't answer.
 */
uint32_t sdq_core_clock_hz(void);

#endif /* SDCARD_HW_COMMON_H_ */
