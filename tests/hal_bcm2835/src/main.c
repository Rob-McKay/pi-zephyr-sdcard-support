/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the hardware abstraction layer: each accessor runs
 * against a RAM-backed register block, and the test checks it touched the
 * right register with the right value and nothing else.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <hal/bcm2835_gpio.h>
#include <hal/bcm2835_sdhost.h>

/* A register block large enough for every peripheral here; the guard
 * value shows which registers an accessor did not write.
 */
#define NREGS 64
#define GUARD 0xdeadbeefU

/* The fake register block. The accessors reach it by address (base()), so
 * it is a static rather than a fixture.
 */
static uint32_t regs[NREGS];

/**
 * @brief Fill the fake block with the guard value before each test.
 *
 * @param f Unused.
 *
 * @par Global state
 * Changes @c regs (every word set to GUARD).
 */
static void before(void *f)
{
	ARG_UNUSED(f);
	for (int i = 0; i < NREGS; i++) {
		regs[i] = GUARD;
	}
}

/**
 * @brief The fake block's base address.
 *
 * @return Base for the accessors.
 */
static mm_reg_t base(void)
{
	return (mm_reg_t)regs;
}

/**
 * @brief Check one register holds a value and every other one is untouched.
 *
 * @param off Register offset.
 * @param val Expected value.
 */
static void expect_only(uint32_t off, uint32_t val)
{
	for (uint32_t i = 0; i < NREGS; i++) {
		if (i == off / 4) {
			zassert_equal(regs[i], val, "reg 0x%02x = 0x%08x, want 0x%08x", off,
				      regs[i], val);
		} else {
			zassert_equal(regs[i], GUARD, "reg 0x%02x written (0x%08x)", (i * 4),
				      regs[i]);
		}
	}
}

/**
 * @brief Set a register, for an accessor to read.
 *
 * @param off Register offset.
 * @param val Value.
 *
 * @par Global state
 * Changes @c regs.
 */
static void set_reg(uint32_t off, uint32_t val)
{
	regs[off / 4] = val;
}

/* ===== GPIO ========================================================= */

/** @brief hal_gpio_set_input() clears one pin's 3 function bits in the right GPFSEL. */
ZTEST(hal_gpio, test_set_input)
{
	set_reg(0x08, 0xffffffff); /* GPFSEL2: GPIO 20-29 */
	hal_gpio_set_input(base(), 26);
	expect_only(0x08, ~(0x7U << 18));
}

/** @brief hal_gpio_pull_select() writes GPPUD. */
ZTEST(hal_gpio, test_pull_select)
{
	hal_gpio_pull_select(base(), BCM2835_GPIO_PULL_UP);
	expect_only(BCM2835_GPIO_GPPUD, 2);
}

/** @brief hal_gpio_pull_clock() pulses one bit in bank 0's GPPUDCLK0. */
ZTEST(hal_gpio, test_pull_clock_bank0)
{
	hal_gpio_pull_clock(base(), 26);
	expect_only(BCM2835_GPIO_GPPUDCLK0, BIT(26));
}

/** @brief hal_gpio_pull_clock() uses GPPUDCLK1 for GPIO 32 and up. */
ZTEST(hal_gpio, test_pull_clock_bank1)
{
	hal_gpio_pull_clock(base(), 49);
	expect_only(BCM2835_GPIO_GPPUDCLK0 + 4, BIT(17));
}

/** @brief hal_gpio_pull_clock_end() clears the pin's GPPUDCLK bank. */
ZTEST(hal_gpio, test_pull_clock_end)
{
	hal_gpio_pull_clock_end(base(), 26);
	expect_only(BCM2835_GPIO_GPPUDCLK0, 0);
}

/** @brief hal_gpio_level() reads one pin's bit from the right GPLEV bank. */
ZTEST(hal_gpio, test_level)
{
	set_reg(BCM2835_GPIO_GPLEV0, BIT(26));
	set_reg(BCM2835_GPIO_GPLEV0 + 4, 0);
	zassert_true(hal_gpio_level(base(), 26));
	zassert_false(hal_gpio_level(base(), 25));
	zassert_false(hal_gpio_level(base(), 32)); /* bank 1 */
	set_reg(BCM2835_GPIO_GPLEV0 + 4, BIT(0));
	zassert_true(hal_gpio_level(base(), 32));
}

ZTEST_SUITE(hal_gpio, NULL, NULL, before, NULL, NULL);

/* ===== SDHost ======================================================= */

/** @brief The SDHost accessors read SDTOUT, SDCDIV, SDEDM's fill level and SDDATA. */
ZTEST(hal_sdhost, test_reads)
{
	set_reg(BCM2835_SDHOST_SDTOUT, 0x00f00000);
	set_reg(BCM2835_SDHOST_SDCDIV, 3);
	set_reg(BCM2835_SDHOST_SDEDM, 0xfffffe0f | (17U << 4));
	set_reg(BCM2835_SDHOST_SDDATA, 0xa5a5a5a5);

	zassert_equal(hal_sdhost_data_timeout(base()), 0x00f00000);
	zassert_equal(hal_sdhost_clock_divider(base()), 3);
	zassert_equal(hal_sdhost_fifo_level(base()), 17);
	zassert_equal(hal_sdhost_fifo_read(base()), 0xa5a5a5a5);
}

/** @brief hal_sdhost_bus_is_4bit() follows SDHCFG bit 2 only. */
ZTEST(hal_sdhost, test_bus_width)
{
	set_reg(BCM2835_SDHOST_SDHCFG, BIT(2));
	zassert_true(hal_sdhost_bus_is_4bit(base()));
	set_reg(BCM2835_SDHOST_SDHCFG, (uint32_t)~BIT(2));
	zassert_false(hal_sdhost_bus_is_4bit(base()));
}

ZTEST_SUITE(hal_sdhost, NULL, NULL, before, NULL, NULL);
