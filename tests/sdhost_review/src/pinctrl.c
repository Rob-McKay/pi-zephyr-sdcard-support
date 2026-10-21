/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Finding 7 of docs/reviews/pr110196-sdhost-review.md: the PR's SDHost pins
 * ask for pull-ups (bias-pull-up on CMD and DAT0-3), but Zephyr's BCM2711
 * pinctrl driver, which BCM2710 boards also use, programs the BCM2711 pull
 * registers (GPIO_PUP_PDN_CNTRL, 0xe4..0xf0). The Pi 3 family's SoCs (the
 * BCM2837, the 3A+'s BCM2837B0 and the Zero 2 W's RP3A0) have the older
 * GPPUD (0x94) + GPPUDCLK0/1 (0x98/0x9c) sequence instead, so the pulls
 * are never set there.
 *
 * pinctrl_bcm2711.c is compiled here against a fake GPIO block that logs
 * every register write.
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/bcm2711-pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#define GPIO_BASE DT_REG_ADDR(DT_NODELABEL(fake_pinctrl))

static uint32_t gpio_regs[0xf4 / 4];
static struct {
	uint32_t off, val;
} gpio_log[64];
static int gpio_nlog;

/**
 * @brief The driver's sys_read32(): read a fake GPIO register.
 *
 * @param addr Register address.
 * @return Register value.
 */
static uint32_t fake_gpio_read32(uintptr_t addr)
{
	uint32_t off = addr - GPIO_BASE;

	zassert_true(off < sizeof(gpio_regs), "read outside the GPIO block: 0x%x", off);
	return gpio_regs[off / 4];
}

/**
 * @brief The driver's sys_write32(): write a fake GPIO register and log it.
 *
 * @param val Value written.
 * @param addr Register address.
 */
static void fake_gpio_write32(uint32_t val, uintptr_t addr)
{
	uint32_t off = addr - GPIO_BASE;

	zassert_true(off < sizeof(gpio_regs), "write outside the GPIO block: 0x%x", off);
	gpio_regs[off / 4] = val;
	if (gpio_nlog < (int)ARRAY_SIZE(gpio_log)) {
		gpio_log[gpio_nlog].off = off;
		gpio_log[gpio_nlog].val = val;
		gpio_nlog++;
	}
}

#define sys_read32(addr)       fake_gpio_read32((uintptr_t)(addr))
#define sys_write32(val, addr) fake_gpio_write32((val), (uintptr_t)(addr))

/* The driver under test, as in the pinned Zephyr. */
#include "pinctrl_bcm2711.c" /* NOLINT(bugprone-suspicious-include): the upstream driver is the code under test, built in to reach its static functions */

#undef sys_read32
#undef sys_write32

/* BCM2837/BCM2837B0 legacy pull control (BCM2835 ARM Peripherals, section 6.1). */
#define GPPUD          0x94
#define GPPUDCLK1      0x9c
#define GPPUD_PULL_UP  2
#define BCM2711_PUPPDN 0xe4 /* first BCM2711-only pull register */

static const pinctrl_soc_pin_t sd_cmd_pin = {
	.pin = 49, .func = BCM2711_FSEL_ALT0, .pull = BCM2711_PULL_UP,
};

/**
 * @brief Clear the fake GPIO block and its write log before each test.
 *
 * @param f Unused fixture.
 */
static void before(void *f)
{
	ARG_UNUSED(f);
	memset(gpio_regs, 0, sizeof(gpio_regs));
	gpio_nlog = 0;
}

/**
 * @brief Tell whether the driver wrote a value to a register.
 *
 * @param off Register offset.
 * @param mask Bits compared.
 * @param val Expected value of those bits.
 * @return True if any logged write to @p off matches.
 */
static bool wrote(uint32_t off, uint32_t mask, uint32_t val)
{
	for (int i = 0; i < gpio_nlog; i++) {
		if (gpio_log[i].off == off && (gpio_log[i].val & mask) == val) {
			return true;
		}
	}
	return false;
}

/** @brief Function select for GPIO 49 = ALT0 lands in GPFSEL4 bits 27-29. */
ZTEST(pinctrl_sanity, test_function_select)
{
	zassert_ok(pinctrl_configure_pins(&sd_cmd_pin, 1, 0));
	zassert_equal((gpio_regs[0x10 / 4] >> 27) & 0x7, BCM2711_FSEL_ALT0);
}

ZTEST_SUITE(pinctrl_sanity, NULL, NULL, before, NULL, NULL);

/** @brief Finding 7: a pull-up uses the BCM2837's GPPUD/GPPUDCLK registers. */
ZTEST(pinctrl_review, test_f7_pull_up_uses_bcm2837_registers)
{
	zassert_ok(pinctrl_configure_pins(&sd_cmd_pin, 1, 0));

	for (int i = 0; i < gpio_nlog; i++) {
		zassert_true(gpio_log[i].off < BCM2711_PUPPDN,
			     "write to 0x%x, a BCM2711-only pull register (no such register on "
			     "the BCM2837)", gpio_log[i].off);
	}
	zassert_true(wrote(GPPUD, 0x3, GPPUD_PULL_UP), "GPPUD never set to pull-up");
	zassert_true(wrote(GPPUDCLK1, BIT(49 - 32), BIT(49 - 32)), "GPPUDCLK1 never clocked");
}

ZTEST_EXPECT_FAIL(pinctrl_review, test_f7_pull_up_uses_bcm2837_registers);

ZTEST_SUITE(pinctrl_review, NULL, NULL, before, NULL, NULL);
