/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware abstraction for the BCM2835/BCM2837 GPIO block (BCM2835 ARM
 * Peripherals, section 6.1): one inline function per job a register does.
 * Pulls use the legacy GPPUD + GPPUDCLKn sequence of these SoCs; the
 * BCM2711 replaced it.
 */

#ifndef HAL_BCM2835_GPIO_H_
#define HAL_BCM2835_GPIO_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define BCM2835_GPIO_GPFSEL0   0x00
#define BCM2835_GPIO_GPLEV0    0x34
#define BCM2835_GPIO_GPPUD     0x94
#define BCM2835_GPIO_GPPUDCLK0 0x98

/* GPPUD control values. */
#define BCM2835_GPIO_PULL_OFF  0
#define BCM2835_GPIO_PULL_DOWN 1
#define BCM2835_GPIO_PULL_UP   2

/**
 * @brief Make a pin a plain input (GPFSELn).
 *
 * @param base Mapped GPIO block.
 * @param pin GPIO number (0-53).
 */
static inline void hal_gpio_set_input(mm_reg_t base, unsigned int pin)
{
	mm_reg_t reg = base + BCM2835_GPIO_GPFSEL0 + ((mm_reg_t)(pin / 10) * 4);

	sys_write32(sys_read32(reg) & ~(0x7U << ((pin % 10) * 3)), reg);
}

/**
 * @brief Choose the pull the next clock pulse applies (GPPUD).
 *
 * @param base Mapped GPIO block.
 * @param pull BCM2835_GPIO_PULL_OFF, _DOWN or _UP.
 */
static inline void hal_gpio_pull_select(mm_reg_t base, uint32_t pull)
{
	sys_write32(pull, base + BCM2835_GPIO_GPPUD);
}

/**
 * @brief Clock the selected pull into one pin (GPPUDCLKn).
 *
 * @param base Mapped GPIO block.
 * @param pin GPIO number (0-53).
 */
static inline void hal_gpio_pull_clock(mm_reg_t base, unsigned int pin)
{
	sys_write32(BIT((pin % 32)), base + BCM2835_GPIO_GPPUDCLK0 + ((mm_reg_t)(pin / 32) * 4));
}

/**
 * @brief End the pull clock pulse for a pin's bank (GPPUDCLKn).
 *
 * @param base Mapped GPIO block.
 * @param pin Any GPIO in the bank (0-31 or 32-53).
 */
static inline void hal_gpio_pull_clock_end(mm_reg_t base, unsigned int pin)
{
	sys_write32(0, base + BCM2835_GPIO_GPPUDCLK0 + ((mm_reg_t)(pin / 32) * 4));
}

/**
 * @brief Read a pin's level (GPLEVn).
 *
 * @param base Mapped GPIO block.
 * @param pin GPIO number (0-53).
 * @return True if high.
 */
static inline bool hal_gpio_level(mm_reg_t base, unsigned int pin)
{
	return (sys_read32(base + BCM2835_GPIO_GPLEV0 + ((mm_reg_t)(pin / 32) * 4)) &
		BIT((pin % 32))) != 0;
}

#endif /* HAL_BCM2835_GPIO_H_ */
