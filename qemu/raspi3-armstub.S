/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stand-in for the Raspberry Pi firmware's ARM boot stub, used only
 * when running the rpi_3a_plus build on QEMU's raspi3ap machine without
 * the GPU firmware. Like the firmware's armstub8 it sets the generic timer
 * frequency (QEMU's counter runs at 62.5 MHz; real hardware's at 19.2 MHz),
 * parks CPUs 1-3, and enters the kernel on CPU 0 in
 * non-secure EL2. Entering in EL3 instead would make Zephyr overwrite
 * CNTFRQ with its build-time value, which is 0 for this board.
 *
 * Build with ZEPHYR_ENTRY defined, see scripts/run-qemu.sh.
 */

	.text
	.globl	_start
/*
 * Reset entry, on all four CPUs in EL3: park CPUs 1-3, set CNTFRQ, then
 * eret to ZEPHYR_ENTRY in non-secure EL2h with interrupts masked.
 */
_start:
	mrs	x0, mpidr_el1
	and	x0, x0, #3
	cbnz	x0, park

	ldr	x1, =62500000
	msr	cntfrq_el0, x1

	/* SCR_EL3: RW (AArch64 below), HCE, SMD, RES1 bits, NS */
	mov	x0, #0x5b1
	msr	scr_el3, x0
	/* Return to EL2h with all exceptions masked */
	mov	x0, #0x3c9
	msr	spsr_el3, x0
	ldr	x0, =ZEPHYR_ENTRY
	msr	elr_el3, x0
	eret

park:
	wfe
	b	park

	.ltorg
