/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UBSAN_H_
#define UBSAN_H_

#include <stdint.h>

/* Signed overflows detected in sdhc_bcm2835_sdhost.c, and the last line. */
extern int ubsan_driver_overflows;
extern uint32_t ubsan_last_line;

/**
 * @brief Sanitizer handler for a signed addition overflow: record it.
 *
 * @param data Check data, starting with the source location.
 * @param lhs Left operand (unused).
 * @param rhs Right operand (unused).
 */
void __ubsan_handle_add_overflow(void *data, void *lhs, void *rhs); /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): name fixed by the compiler's sanitizer ABI */

/**
 * @brief Sanitizer handler for a signed subtraction overflow: record it.
 *
 * @param data Check data, starting with the source location.
 * @param lhs Left operand (unused).
 * @param rhs Right operand (unused).
 */
void __ubsan_handle_sub_overflow(void *data, void *lhs, void *rhs); /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): name fixed by the compiler's sanitizer ABI */

/**
 * @brief Sanitizer handler for a signed multiplication overflow: record it.
 *
 * @param data Check data, starting with the source location.
 * @param lhs Left operand (unused).
 * @param rhs Right operand (unused).
 */
void __ubsan_handle_mul_overflow(void *data, void *lhs, void *rhs); /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): name fixed by the compiler's sanitizer ABI */

/**
 * @brief Sanitizer handler for a signed negation overflow: record it.
 *
 * @param data Check data, starting with the source location.
 * @param val Operand (unused).
 */
void __ubsan_handle_negate_overflow(void *data, void *val); /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): name fixed by the compiler's sanitizer ABI */

/**
 * @brief Sanitizer handler for a signed division or remainder overflow: record it.
 *
 * @param data Check data, starting with the source location.
 * @param lhs Left operand (unused).
 * @param rhs Right operand (unused).
 */
void __ubsan_handle_divrem_overflow(void *data, void *lhs, void *rhs); /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): name fixed by the compiler's sanitizer ABI */

#endif /* UBSAN_H_ */
