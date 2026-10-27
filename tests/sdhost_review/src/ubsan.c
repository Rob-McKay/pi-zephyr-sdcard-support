/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal handlers for -fsanitize=signed-integer-overflow (sdhost.c is
 * built with it). Instead of a sanitizer runtime, they count overflows
 * whose source location is in the driver under test, so a test can assert
 * there were none.
 */

#include <stdint.h>
#include <string.h>

#include "ubsan.h"

struct source_location {
	const char *file;
	uint32_t line;
	uint32_t column;
};

/* The UBSan runtime ABI's layout for overflow checks (GCC and clang):
 * the location comes first.
 */
struct overflow_data {
	struct source_location loc;
	const void *type;
};

int ubsan_driver_overflows;
uint32_t ubsan_last_line;

/* Interface: ubsan.h.
 * Global state: changes ubsan_driver_overflows and ubsan_last_line.
 */
void ubsan_reset(void)
{
	ubsan_driver_overflows = 0;
	ubsan_last_line = 0;
}

/**
 * @brief Count an overflow if it is in the driver under test.
 *
 * @param data The compiler's struct overflow_data for the check.
 *
 * @par Global state
 * Changes @c ubsan_driver_overflows and @c ubsan_last_line.
 */
static void record(void *data)
{
	const struct overflow_data *d = data;

	if (d->loc.file != NULL && strstr(d->loc.file, "sdhc_bcm2835_sdhost.c") != NULL) {
		ubsan_driver_overflows++;
		ubsan_last_line = d->loc.line;
	}
}

/* Interface: ubsan.h. */
void __ubsan_handle_add_overflow(void *data, void *lhs, void *rhs)
{
	(void)lhs;
	(void)rhs;
	record(data);
}

/* Interface: ubsan.h. */
void __ubsan_handle_sub_overflow(void *data, void *lhs, void *rhs)
{
	(void)lhs;
	(void)rhs;
	record(data);
}

/* Interface: ubsan.h. */
void __ubsan_handle_mul_overflow(void *data, void *lhs, void *rhs)
{
	(void)lhs;
	(void)rhs;
	record(data);
}

/* Interface: ubsan.h. */
void __ubsan_handle_negate_overflow(void *data, void *val)
{
	(void)val;
	record(data);
}

/* Interface: ubsan.h. */
void __ubsan_handle_divrem_overflow(void *data, void *lhs, void *rhs)
{
	(void)lhs;
	(void)rhs;
	record(data);
}
