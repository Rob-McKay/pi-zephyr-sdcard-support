/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware abstraction for inspecting the BCM283x SDHost controller from
 * tests: read-only state, and emptying the FIFO. The SD driver itself is
 * upstream Zephyr (drivers/sdhc/sdhc_bcm2835_sdhost.c) and has its own
 * register access.
 */

#ifndef HAL_BCM2835_SDHOST_H_
#define HAL_BCM2835_SDHOST_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define BCM2835_SDHOST_SDTOUT 0x08
#define BCM2835_SDHOST_SDCDIV 0x0c
#define BCM2835_SDHOST_SDEDM  0x34
#define BCM2835_SDHOST_SDHCFG 0x38
#define BCM2835_SDHOST_SDDATA 0x40

/**
 * @brief Read the data timeout, in SD clock cycles (SDTOUT).
 *
 * @param base Mapped SDHost block.
 * @return Timeout in cycles.
 */
static inline uint32_t hal_sdhost_data_timeout(mm_reg_t base)
{
	return sys_read32(base + BCM2835_SDHOST_SDTOUT);
}

/**
 * @brief Read the clock divider: SD clock = core clock / (divider + 2)
 *        (SDCDIV).
 *
 * @param base Mapped SDHost block.
 * @return Divider.
 */
static inline uint32_t hal_sdhost_clock_divider(mm_reg_t base)
{
	return sys_read32(base + BCM2835_SDHOST_SDCDIV);
}

/**
 * @brief Tell whether the card bus is 4 bits wide (SDHCFG WIDE_EXT_BUS, bit 2).
 *
 * @param base Mapped SDHost block.
 * @return True for a 4-bit bus, false for 1-bit.
 */
static inline bool hal_sdhost_bus_is_4bit(mm_reg_t base)
{
	return (sys_read32(base + BCM2835_SDHOST_SDHCFG) & BIT(2)) != 0U;
}

/**
 * @brief Read how many words the FIFO holds (SDEDM bits 8:4).
 *
 * @param base Mapped SDHost block.
 * @return Fill level, 0-31.
 */
static inline uint32_t hal_sdhost_fifo_level(mm_reg_t base)
{
	return (sys_read32(base + BCM2835_SDHOST_SDEDM) >> 4) & 0x1f;
}

/**
 * @brief Take one word from the FIFO (SDDATA).
 *
 * @param base Mapped SDHost block.
 * @return The word.
 */
static inline uint32_t hal_sdhost_fifo_read(mm_reg_t base)
{
	return sys_read32(base + BCM2835_SDHOST_SDDATA);
}

#endif /* HAL_BCM2835_SDHOST_H_ */
