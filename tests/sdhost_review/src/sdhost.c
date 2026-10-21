/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests reproducing the findings of docs/reviews/pr110196-sdhost-review.md
 * in the BCM283x SDHost driver of Zephyr PR #110196.
 *
 * The driver source itself is compiled into this file with its register
 * accesses redirected to a model of the controller and card (fake_*
 * below), which can inject the conditions a real card produces: long busy
 * periods, CRC errors, slow data, a faster core clock, a corrupt FIFO
 * level.
 *
 * Sanity tests check the model with normal transfers, which must pass.
 * Each finding's test asserts what a correct driver would do; today they
 * fail, so they are marked ZTEST_EXPECT_FAIL. When the driver is fixed, a
 * test passes "unexpectedly" and fails the suite, as a reminder to drop
 * its marker.
 */

/* Everything the driver includes, first, so the redirect below doesn't
 * touch the kernel's own definitions of sys_read32/sys_write32.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/ztest.h>

#include "ubsan.h"

static uint32_t fake_read32(uintptr_t addr);
static void fake_write32(uint32_t val, uintptr_t addr);

#define sys_read32(addr)       fake_read32((uintptr_t)(addr))
#define sys_write32(val, addr) fake_write32((val), (uintptr_t)(addr))

/* The driver under test, as applied from PR #110196. Its init() is
 * unused: the tests build the device by hand (no devicetree instance).
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "sdhc_bcm2835_sdhost.c" /* NOLINT(bugprone-suspicious-include): the upstream driver is the code under test, built in to reach its static functions */
#pragma GCC diagnostic pop

#undef sys_read32
#undef sys_write32

/* ===== Controller and card model ===================================== */

#define FAKE_BASE  0x20000000UL
#define BLOCK      512
#define WORDS(n)   ((n) / 4)
#define QUEUE_LEN  8192

/* The SD stack's default command timeout (CONFIG_SD_CMD_TIMEOUT). */
#define SD_CMD_TIMEOUT_MS 200

/* SD spec write busy limits: 250 ms SDSC/SDHC, 500 ms SDXC (SD Physical
 * Layer Simplified Spec v9.10, 4.6.2.2).
 */
#define SPEC_WRITE_BUSY_MS 500

static struct fake_hw {
	uint32_t regs[0x60 / 4];
	uint32_t core_hz;          /* the real VPU core clock */

	/* Command in progress: NEW_FLAG clears at cmd_done_at. */
	bool cmd_active;
	int64_t cmd_done_at;
	int64_t resp_delay_us;     /* every command */
	int64_t resp_delay_once_us;/* the next command only */
	int64_t busy_stop_us;      /* DAT0 busy after CMD12 (programming) */
	int64_t busy_r1b_us;       /* other busy commands */

	/* Card -> host words, oldest first; visible from avail_at. */
	uint32_t q[QUEUE_LEN];
	int64_t q_avail[QUEUE_LEN];
	int qh, qt;
	int64_t data_delay_us;     /* before a read's data appears */

	/* Data phase of the current data command. */
	bool data_active;
	bool data_read;
	uint32_t data_left;        /* words still to move */
	uint32_t data_moved;
	uint32_t err_after;        /* CRC error once this many words moved */
	int64_t write_busy_until;

	bool card_in_data;         /* open-ended CMD18/25 not yet stopped */
	uint32_t data_arg;         /* the data command's argument */

	/* Finding 12: an open-ended read runs into the next block before the
	 * host can stop it, as seen on a real Pi 3A+.
	 */
	bool run_on;               /* inject: always run on */
	bool overrun;              /* sending a block nobody asked for */
	bool stopped_mid_block;    /* CMD12 cut that block short */
	int64_t abort_at;          /* the controller's data timeout then ends it */

	/* After a read's last word the FSM stays in READDATA briefly while the
	 * block's CRC arrives; only then is a CRC error flagged.
	 */
	int64_t tail_us;           /* inject: length of that tail */
	int64_t tail_until;
	bool crc_err_last;         /* inject: CRC error on the last block */
	uint32_t written[QUEUE_LEN];
	int nwritten;

	int fill_override;         /* SDEDM fill level to report, -1: real */
	int overruns;              /* FIFO writes while full */
	int violations;            /* commands issued mid data phase */
	uint8_t log[512];          /* opcodes issued */
	int nlog;
} hw;

/**
 * @brief Read the simulated time.
 *
 * @return Microseconds since boot.
 */
static int64_t now_us(void)
{
	return k_cyc_to_us_floor64(k_cycle_get_64());
}

/**
 * @brief Give the model card's content of one word.
 *
 * @param lba Block number (the read command's argument).
 * @param i Word index from the start of the read.
 * @return The word the card holds there.
 */
static uint32_t pattern(uint32_t lba, uint32_t i)
{
	return 0xa5000000U ^ (lba << 16) ^ i;
}

/**
 * @brief Count the queued card-to-host words that have arrived by now.
 *
 * @return Number of words at the head of the queue whose time has come.
 */
static int q_count_avail(void)
{
	int n = 0;
	int64_t now = now_us();

	for (int i = hw.qh; i != hw.qt; i = (i + 1) % QUEUE_LEN) {
		if (hw.q_avail[i] > now) {
			break;
		}
		n++;
	}
	return n;
}

/**
 * @brief Queue one card-to-host word.
 *
 * @param w The word.
 * @param at Time (us) from which the host can read it.
 */
static void q_push(uint32_t w, int64_t at)
{
	hw.q[hw.qt] = w;
	hw.q_avail[hw.qt] = at;
	hw.qt = (hw.qt + 1) % QUEUE_LEN;
}

/** @brief Put the controller and card model back to its power-on state. */
static void fake_reset(void)
{
	hw = (struct fake_hw){0};
	hw.core_hz = 250000000;
	hw.fill_override = -1;
}

/**
 * @brief Compute the SD bus clock the model's divider produces.
 *
 * @return Bus clock in Hz: the real core clock over SDCDIV + 2.
 */
static uint32_t bus_hz(void)
{
	return hw.core_hz / (hw.regs[SDCDIV / 4] + 2);
}

/**
 * @brief Inject a CRC error: the controller abandons the data phase.
 */
static void data_crc_error(void)
{
	hw.regs[SDHSTS / 4] |= SDHSTS_CRC16_ERROR;
	hw.data_active = false;
	hw.qh = hw.qt;
}

/**
 * @brief The card starts the next block of an open-ended read: one word a
 *        microsecond, into the FIFO.
 */
static void run_on_next_block(void)
{
	int64_t at = now_us();

	for (uint32_t i = 0; i < WORDS(BLOCK); i++) {
		q_push(pattern(hw.data_arg, hw.data_moved + i), at + i);
	}
	hw.data_left = WORDS(BLOCK);
	hw.overrun = true;
}

/**
 * @brief Compute the controller's data timeout (SDTOUT) in microseconds.
 *
 * @return SDTOUT cycles at the current bus clock.
 */
static int64_t sdtout_us(void)
{
	return ((int64_t)hw.regs[SDTOUT / 4] * 1000000) / bus_hz();
}

/**
 * @brief Account for one word moved through SDDATA in the data phase.
 *
 * Injects the CRC error set by err_after, and starts the card's write
 * busy once the last word of a write has gone.
 */
static void data_moved_one(void)
{
	hw.data_moved++;
	if (hw.data_left > 0) {
		hw.data_left--;
	}
	if (hw.err_after != 0 && hw.data_moved == hw.err_after) {
		data_crc_error();
		return;
	}
	if (hw.data_read && hw.data_left == 0 && hw.tail_us != 0) {
		hw.tail_until = now_us() + hw.tail_us;
	}
	if (hw.data_read && hw.data_left == 0 && hw.card_in_data && hw.run_on) {
		run_on_next_block();
		return;
	}
	if (hw.data_left == 0 && !hw.data_read) {
		hw.write_busy_until = now_us() + 50;
	}
}

/**
 * @brief Give the SDEDM state machine field for the current data phase.
 *
 * Ends a write's data phase once its busy period is over.
 *
 * @return SDEDM_FSM_* value.
 */
static uint32_t edm_fsm(void)
{
	uint32_t fsm = SDEDM_FSM_DATAMODE;

	if (hw.stopped_mid_block && now_us() >= hw.abort_at) {
		/* The rest of the block never came: the data timeout ends it. */
		hw.regs[SDHSTS / 4] |= SDHSTS_REW_TIME_OUT;
		hw.data_active = false;
		hw.stopped_mid_block = false;
		hw.overrun = false;
	}
	if (hw.tail_until != 0 && now_us() >= hw.tail_until) {
		/* The last block's CRC has arrived. */
		hw.tail_until = 0;
		if (hw.crc_err_last) {
			hw.regs[SDHSTS / 4] |= SDHSTS_CRC16_ERROR;
		}
	}
	if (hw.data_active) {
		if (hw.data_read) {
			fsm = hw.data_left > 0 || hw.tail_until != 0 ? SDEDM_FSM_READDATA
								    : SDEDM_FSM_READWAIT;
		} else if (hw.data_left > 0) {
			fsm = SDEDM_FSM_WRITEDATA;
		} else if (now_us() < hw.write_busy_until) {
			fsm = SDEDM_FSM_WRITEWAIT1;
		} else {
			hw.data_active = false;
		}
	}
	return fsm;
}

/**
 * @brief Give the SDEDM FIFO fill level, or the injected one.
 *
 * @return Words in the FIFO, as SDEDM reports them.
 */
static uint32_t edm_fill(void)
{
	uint32_t fill = 0;

	if (hw.data_read || !hw.data_active) {
		fill = MIN(q_count_avail(), SDDATA_FIFO_WORDS);
	}
	if (hw.fill_override >= 0) {
		fill = hw.fill_override;
	}
	return fill;
}

/**
 * @brief Read SDEDM: the written thresholds plus the live FSM and fill level.
 *
 * @return Register value.
 */
static uint32_t edm_value(void)
{
	uint32_t fsm = edm_fsm();
	uint32_t fill = edm_fill();

	return (hw.regs[SDEDM / 4] & ~(uint32_t)(SDEDM_FSM_MASK | (0x1fU << 4))) | fsm | (fill << 4);
}

/**
 * @brief Read SDCMD, completing the command in progress if its time has come.
 *
 * On completion NEW_FLAG clears, and a busy-wait command raises BUSY_IRPT.
 *
 * @return Register value.
 */
static uint32_t sdcmd_read(void)
{
	/* With the FIFO full during a read the controller stops the bus
	 * clock, so no command completes (on the Pi: "NEW_FLAG never
	 * cleared" after a run-on read).
	 */
	bool clock_stopped = hw.overrun && hw.data_active && q_count_avail() >= SDDATA_FIFO_WORDS;

	if (hw.cmd_active && !clock_stopped && now_us() >= hw.cmd_done_at) {
		hw.cmd_active = false;
		if (hw.regs[SDCMD / 4] & SDCMD_BUSYWAIT) {
			hw.regs[SDHSTS / 4] |= SDHSTS_BUSY_IRPT;
		}
		hw.regs[SDCMD / 4] &= ~(uint32_t)SDCMD_NEW_FLAG;
	}
	return hw.regs[SDCMD / 4];
}

/**
 * @brief Read SDDATA: pop the oldest arrived word from the FIFO.
 *
 * @return The word, or 0xdeadbeef if the FIFO is empty.
 */
static uint32_t sddata_read(void)
{
	if (hw.qh != hw.qt && q_count_avail() > 0) {
		uint32_t w = hw.q[hw.qh];

		hw.qh = (hw.qh + 1) % QUEUE_LEN;
		if (hw.data_active && hw.data_read) {
			data_moved_one();
		}
		return w;
	}
	return 0xdeadbeef;
}

/**
 * @brief The driver's sys_read32(): read a model register.
 *
 * Each access takes 1 us of simulated time.
 *
 * @param addr Register address.
 * @return Register value.
 */
static uint32_t fake_read32(uintptr_t addr)
{
	uint32_t off = addr - FAKE_BASE;

	zassert_true(addr >= FAKE_BASE && off < sizeof(hw.regs), "bad read 0x%lx", addr);
	k_busy_wait(1); /* register access time; lets simulated time run */

	switch (off) {
	case SDCMD:
		return sdcmd_read();
	case SDEDM:
		return edm_value();
	case SDDATA:
		return sddata_read();
	default:
		return hw.regs[off / 4];
	}
}

/**
 * @brief Write SDDATA: log the word and count it against a write.
 *
 * @param val The word.
 */
static void sddata_write(uint32_t val)
{
	if (hw.fill_override >= SDDATA_FIFO_WORDS) {
		hw.overruns++;
	}
	if (hw.nwritten < QUEUE_LEN) {
		hw.written[hw.nwritten++] = val;
	}
	if (hw.data_active && !hw.data_read) {
		data_moved_one();
	}
}

/**
 * @brief Write SDEDM: thresholds and FORCE_DATA_MODE; FSM bits are read-only.
 *
 * @param val Value written.
 */
static void sdedm_write(uint32_t val)
{
	hw.regs[SDEDM / 4] = val & ~(uint32_t)(SDEDM_FSM_MASK | (0x1fU << 4));
	if ((val & SDEDM_FORCE_DATA_MODE) && hw.data_left == 0) {
		hw.data_active = false;
	}
}

/**
 * @brief Log a new command, counting it as a violation if one is in progress.
 *
 * @param op Command opcode.
 * @param now Time of issue (us).
 */
static void cmd_log(uint32_t op, int64_t now)
{
	bool stopping_read = op == SD_STOP_TRANSMISSION && hw.data_read;

	if ((hw.cmd_active && now < hw.cmd_done_at) ||
	    (hw.data_active && hw.data_left > 0 && !stopping_read)) {
		hw.violations++;
	}
	if (hw.nlog < (int)sizeof(hw.log)) {
		hw.log[hw.nlog++] = op;
	}
}

/**
 * @brief Schedule a new command's completion and set its R1 response.
 *
 * @param val SDCMD value written.
 * @param op Command opcode.
 * @param now Time of issue (us).
 */
static void cmd_schedule(uint32_t val, uint32_t op, int64_t now)
{
	hw.cmd_active = true;
	hw.cmd_done_at = now + 20 + hw.resp_delay_us + hw.resp_delay_once_us;
	hw.resp_delay_once_us = 0;
	if (val & SDCMD_BUSYWAIT) {
		hw.cmd_done_at += op == SD_STOP_TRANSMISSION ? hw.busy_stop_us : hw.busy_r1b_us;
	}
	hw.regs[SDRSP0 / 4] = 0x900; /* R1: ready, transfer state */
}

/**
 * @brief Start a data command's data phase; a read queues the card's data.
 *
 * The read data arrives data_delay_us after the command completes.
 *
 * @param val SDCMD value written.
 * @param op Command opcode.
 * @param arg Command argument (the block number).
 */
static void data_start(uint32_t val, uint32_t op, uint32_t arg)
{
	uint32_t words = WORDS(hw.regs[SDHBCT / 4]) * hw.regs[SDHBLC / 4];

	hw.data_active = true;
	hw.data_read = (val & SDCMD_READ_CMD) != 0;
	hw.data_left = words;
	hw.data_moved = 0;
	hw.data_arg = arg;
	if (op == SD_READ_MULTIPLE_BLOCK || op == SD_WRITE_MULTIPLE_BLOCK) {
		hw.card_in_data = true;
	}
	if (hw.data_read) {
		int64_t at = hw.cmd_done_at + hw.data_delay_us;

		for (uint32_t i = 0; i < words; i++) {
			q_push(pattern(arg, i), at);
		}
	}
}

/**
 * @brief CMD12 during a run-on block: the card stops sending. Only what
 *        reached the FIFO stays; the controller waits for the rest until
 *        its data timeout.
 *
 * @param now Time of the CMD12 (us).
 */
static void card_stops_mid_block(int64_t now)
{
	int keep = MIN(q_count_avail(), SDDATA_FIFO_WORDS);

	hw.qt = (hw.qh + keep) % QUEUE_LEN;
	hw.stopped_mid_block = true;
	hw.abort_at = now + sdtout_us();
}

/**
 * @brief Write SDCMD; with NEW_FLAG set, the card model runs the command.
 *
 * @param val Value written.
 * @param now Time of the write (us).
 */
static void sdcmd_write(uint32_t val, int64_t now)
{
	uint32_t op = val & SDCMD_CMD_MASK;

	hw.regs[SDCMD / 4] = val;
	if (!(val & SDCMD_NEW_FLAG)) {
		return;
	}
	cmd_log(op, now);
	cmd_schedule(val, op, now);
	if (op == SD_STOP_TRANSMISSION) {
		hw.card_in_data = false;
		if (hw.overrun && hw.data_active && hw.data_left > 0) {
			card_stops_mid_block(now);
		}
	}
	if (val & (uint32_t)(SDCMD_READ_CMD | SDCMD_WRITE_CMD)) {
		data_start(val, op, hw.regs[SDARG / 4]);
	}
}

/**
 * @brief The driver's sys_write32(): write a model register.
 *
 * Each access takes 1 us of simulated time.
 *
 * @param val Value written.
 * @param addr Register address.
 */
static void fake_write32(uint32_t val, uintptr_t addr)
{
	uint32_t off = addr - FAKE_BASE;
	int64_t now;

	zassert_true(addr >= FAKE_BASE && off < sizeof(hw.regs), "bad write 0x%lx", addr);
	if (off >= sizeof(hw.regs)) {
		return; /* the assertion failed: ignore the write */
	}
	k_busy_wait(1);
	now = now_us();

	switch (off) {
	case SDHSTS:
		hw.regs[off / 4] &= ~val; /* write 1 to clear */
		break;
	case SDDATA:
		sddata_write(val);
		break;
	case SDEDM:
		sdedm_write(val);
		break;
	case SDCMD:
		sdcmd_write(val, now);
		break;
	default:
		hw.regs[off / 4] = val;
		break;
	}
}

/* ===== The device under test ========================================= */

static struct sdhc_bcm2835_sdhost_config dut_cfg = {
	._mmio = { .addr = FAKE_BASE },
	.clock_freq = 250000000, /* as in bcm2710.dtsi */
	.bus_width = 4,
};
static struct sdhc_bcm2835_sdhost_data dut_data;
static struct device dut = {
	.name = "sdhost",
	.config = &dut_cfg,
	.data = &dut_data,
};

/**
 * @brief Ask the driver for an SD bus clock (4-bit bus, powered).
 *
 * @param hz Requested clock in Hz.
 * @return The driver's set_io() result.
 */
static int set_clock(uint32_t hz)
{
	struct sdhc_io io = {
		.clock = hz,
		.bus_width = SDHC_BUS_WIDTH4BIT,
		.power_mode = SDHC_POWER_ON,
		.bus_mode = SDHC_BUSMODE_PUSHPULL,
		.timing = SDHC_TIMING_LEGACY,
		.signal_voltage = SD_VOL_3_3_V,
		.driver_type = SD_DRIVER_TYPE_B,
	};

	return sdhc_bcm2835_sdhost_set_io(&dut, &io);
}

/**
 * @brief Suite setup: nothing to share.
 *
 * @return NULL (no fixture).
 */
static void *suite_setup(void)
{
	return NULL;
}

/**
 * @brief Reset the model and driver, at 25 MHz, before each test.
 *
 * @param f Unused fixture.
 */
static void before(void *f)
{
	ARG_UNUSED(f);
	fake_reset();
	sdhost_soft_reset(&dut);
	zassert_ok(set_clock(25000000));
	ubsan_driver_overflows = 0;
}

/**
 * @brief Send one command, with a data phase if @p buf is given.
 *
 * @param opcode Command opcode.
 * @param lba Argument and block address.
 * @param blocks Block count.
 * @param block_size Block size in bytes.
 * @param buf Data buffer, or NULL for a command without data.
 * @param cmd_timeout_ms Command timeout.
 * @param data_timeout_ms Data timeout.
 * @return The driver's request() result.
 */
static int xfer(uint32_t opcode, uint32_t lba, uint32_t blocks, uint32_t block_size,
		void *buf, int cmd_timeout_ms, int data_timeout_ms)
{
	struct sdhc_command cmd = {
		.opcode = opcode,
		.arg = lba,
		.response_type = SD_RSP_TYPE_R1,
		.timeout_ms = cmd_timeout_ms,
	};
	struct sdhc_data data = {
		.block_addr = lba,
		.block_size = block_size,
		.blocks = blocks,
		.data = buf,
		.timeout_ms = data_timeout_ms,
	};

	return sdhc_bcm2835_sdhost_request(&dut, &cmd, buf ? &data : NULL);
}

/**
 * @brief Send CMD13 with the SD stack's default command timeout.
 *
 * @return The driver's request() result.
 */
static int send_status(void)
{
	return xfer(SD_SEND_STATUS, 0x10000, 0, 0, NULL, SD_CMD_TIMEOUT_MS, 0);
}

/**
 * @brief Tell whether the model saw a command.
 *
 * @param op Command opcode.
 * @return True if @p op was issued since the last reset.
 */
static bool logged(uint8_t op)
{
	for (int i = 0; i < hw.nlog; i++) {
		if (hw.log[i] == op) {
			return true;
		}
	}
	return false;
}

static uint32_t buf[WORDS(BLOCK) * 8];

/**
 * @brief Assert that buf holds the card's data from a block on.
 *
 * @param lba First block read.
 * @param words Number of words to check.
 */
static void check_pattern(uint32_t lba, uint32_t words)
{
	for (uint32_t i = 0; i < words; i++) {
		zassert_equal(buf[i], pattern(lba, i), "word %u: 0x%08x, expected 0x%08x", i,
			      buf[i], pattern(lba, i));
	}
}

/* ===== Sanity: the model and normal transfers ======================== */

/** @brief A single-block read returns the block's data. */
ZTEST(sdhost_sanity, test_single_block_read)
{
	zassert_ok(xfer(SD_READ_SINGLE_BLOCK, 7, 1, BLOCK, buf, 200, 1000));
	check_pattern(7, WORDS(BLOCK));
	zassert_equal(hw.violations, 0);
}

/** @brief A multi-block read returns the data and ends with CMD12. */
ZTEST(sdhost_sanity, test_multi_block_read_and_stop)
{
	zassert_ok(xfer(SD_READ_MULTIPLE_BLOCK, 9, 4, BLOCK, buf, 200, 1000));
	check_pattern(9, WORDS(BLOCK) * 4);
	zassert_true(logged(SD_STOP_TRANSMISSION), "CMD12 sent");
	zassert_false(hw.card_in_data);
}

/** @brief A multi-block write with 5 ms busy succeeds. */
ZTEST(sdhost_sanity, test_multi_block_write_short_busy)
{
	for (uint32_t i = 0; i < WORDS(BLOCK) * 2; i++) {
		buf[i] = pattern(3, i);
	}
	hw.busy_stop_us = 5000; /* 5 ms programming */
	zassert_ok(xfer(SD_WRITE_MULTIPLE_BLOCK, 3, 2, BLOCK, buf, 200, 1000));
	zassert_equal(hw.nwritten, WORDS(BLOCK) * 2);
	zassert_equal(hw.written[5], pattern(3, 5));
	zassert_false(hw.card_in_data);
	zassert_ok(send_status());
}

/** @brief The bus clock never exceeds the requested one. */
ZTEST(sdhost_sanity, test_clock_divider)
{
	zassert_ok(set_clock(25000000));
	zassert_true(bus_hz() <= 25000000, "%u Hz", bus_hz());
	zassert_ok(set_clock(400000));
	zassert_true(bus_hz() <= 400000, "%u Hz", bus_hz());
}

ZTEST_SUITE(sdhost_sanity, NULL, suite_setup, before, NULL, NULL);

/* ===== Review findings =============================================== */

/**
 * @brief Finding 1: a write survives 250 ms of busy after CMD12.
 *
 * Cards may hold DAT0 busy for up to 250 ms (500 ms SDXC) after CMD12 ends
 * a write (SD Physical Layer Simplified Spec v9.10, 4.6.2.2). The driver
 * waits 100 ms, fails the write and leaves the controller busy.
 */
ZTEST(sdhost_review, test_f1_write_busy_250ms_after_stop)
{
	hw.busy_stop_us = INT64_C(250) * 1000;
	zassert_ok(xfer(SD_WRITE_MULTIPLE_BLOCK, 3, 2, BLOCK, buf, 200, 10000),
		   "spec-compliant 250 ms busy after CMD12 reported as an error");
	zassert_ok(send_status(), "next command fails");
}

/**
 * @brief Finding 2a: a multi-block read failing part-way still sends CMD12.
 *
 * The PR's driver sent no CMD12 after a CRC error, so the card stayed in
 * the data state. Fixed by zephyr/patches/zephyr/sdhost-stop-open-read.patch
 * (finding 12's fix stops a failed open-ended read too).
 */
ZTEST(sdhost_review, test_f2_failed_multiblock_read_is_stopped)
{
	hw.err_after = 200; /* CRC error in the second block */
	zassert_not_ok(xfer(SD_READ_MULTIPLE_BLOCK, 9, 4, BLOCK, buf, 200, 1000));
	zassert_false(hw.card_in_data, "card left in data state (no CMD12)");
}

/**
 * @brief Finding 2b: a read after a timed-out read returns its own block.
 *
 * Nothing is reset after a timeout: the abandoned read's late data sits in
 * the FIFO and the next read returns it as its own.
 */
ZTEST(sdhost_review, test_f2_stale_fifo_after_timeout)
{
	hw.resp_delay_once_us = INT64_C(300) * 1000; /* the card answers late */
	zassert_equal(xfer(SD_READ_SINGLE_BLOCK, 5, 1, BLOCK, buf, 100, 1000), -ETIMEDOUT);
	k_msleep(300); /* the abandoned read's data has arrived meanwhile */

	memset(buf, 0, sizeof(buf));
	int ret = xfer(SD_READ_SINGLE_BLOCK, 6, 1, BLOCK, buf, 200, 1000);

	if (ret == 0) {
		zassert_equal(buf[0], pattern(6, 0),
			      "read of block 6 returned block 5's data (0x%08x)", buf[0]);
	}
	zassert_ok(ret, "read after a timeout fails: %d", ret);
}

/**
 * @brief Finding 12: an open-ended read that runs into the next block is
 *        stopped, returns its own data, and leaves the controller usable.
 *
 * On a real Pi 3A+ the data FSM sometimes runs on into the block after
 * the last one asked for, before the driver can stop the card ("data FSM
 * still busy after read", FIFO full). The model does it every time.
 */
ZTEST(sdhost_review, test_f12_read_running_on_is_stopped)
{
	hw.run_on = true;
	zassert_ok(xfer(SD_READ_MULTIPLE_BLOCK, 9, 4, BLOCK, buf, 200, 2000),
		   "a read that ran on into the next block failed");
	check_pattern(9, WORDS(BLOCK) * 4);
	zassert_false(hw.card_in_data, "card left sending (no CMD12)");
	zassert_ok(send_status(), "the next command fails");

	memset(buf, 0, sizeof(buf));
	zassert_ok(xfer(SD_READ_SINGLE_BLOCK, 6, 1, BLOCK, buf, 200, 1000), "the next read fails");
	check_pattern(6, WORDS(BLOCK));
}

/**
 * @brief A CRC error on the last block of a multi-block read is reported.
 *
 * The FSM shows READDATA for a moment after the last word, while the
 * block's CRC arrives. That is not a run-on: the error found at its end
 * must still fail the read.
 */
ZTEST(sdhost_sanity, test_multi_block_read_crc_error_on_last_block)
{
	hw.tail_us = 50;
	hw.crc_err_last = true;
	zassert_equal(xfer(SD_READ_MULTIPLE_BLOCK, 9, 4, BLOCK, buf, 200, 1000), -EILSEQ,
		      "a CRC error on the last block was not reported");
	zassert_false(hw.card_in_data, "card left sending (no CMD12)");
	zassert_ok(send_status(), "the next command fails");
}

/**
 * @brief Finding 3: a 50 MHz request stays within 50 MHz at a 400 MHz core.
 *
 * The input clock is taken from devicetree (250 MHz), so the bus runs at
 * 80 MHz, over the 50 MHz High Speed maximum (SD Physical Layer Simplified
 * Spec v9.10, 4.3.11).
 */
ZTEST(sdhost_review, test_f3_core_clock_not_250mhz)
{
	hw.core_hz = 400000000;
	zassert_ok(set_clock(50000000));
	zassert_true(bus_hz() <= 50000000, "bus runs at %u Hz for a 50 MHz request",
		     bus_hz());
}

/**
 * @brief Finding 4: the hardware data timeout covers 500 ms at 50 MHz.
 *
 * SDTOUT stays at 0x00f00000 clock cycles: 0.31 s at 50 MHz, less than an
 * SDXC card's 500 ms write busy (SD Physical Layer Simplified Spec v9.10,
 * 4.6.2.2).
 */
ZTEST(sdhost_review, test_f4_data_timeout_follows_clock)
{
	zassert_ok(set_clock(50000000));
	uint64_t ms = (uint64_t)hw.regs[SDTOUT / 4] * 1000 / bus_hz();

	zassert_true(ms >= SPEC_WRITE_BUSY_MS, "hardware data timeout is %llu ms at %u Hz",
		     ms, bus_hz());
}

/**
 * @brief Finding 5a: a read with SDHC_TIMEOUT_FOREVER waits for slow data.
 *
 * Today -1 is a deadline already past: the first FIFO stall fails the read.
 */
ZTEST(sdhost_review, test_f5_data_timeout_forever)
{
	hw.data_delay_us = INT64_C(50) * 1000; /* data after 50 ms */
	zassert_ok(xfer(SD_READ_SINGLE_BLOCK, 4, 1, BLOCK, buf, 200, SDHC_TIMEOUT_FOREVER),
		   "SDHC_TIMEOUT_FOREVER read gave up");
	check_pattern(4, WORDS(BLOCK));
}

/**
 * @brief Finding 5b: a 3,000,000 ms timeout causes no signed overflow.
 *
 * timeout_ms * 1000 is int arithmetic: undefined above 2147483 ms.
 */
ZTEST(sdhost_review, test_f5_timeout_multiplication_overflow)
{
	(void)xfer(SD_SEND_STATUS, 0x10000, 0, 0, NULL, 3000000, 0);
	zassert_equal(ubsan_driver_overflows, 0,
		      "signed overflow (UB) in the driver at line %u", ubsan_last_line);
}

/* Finding 6: the driver spins for the whole transfer. */
static volatile uint32_t low_prio_runs;
static volatile bool low_prio_stop;

/**
 * @brief Lower-priority thread: count how often it gets the CPU.
 *
 * @param a Unused.
 * @param b Unused.
 * @param c Unused.
 */
static void low_prio_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (!low_prio_stop) {
		low_prio_runs++;
		k_msleep(1);
	}
}

/* Zephyr's definition macro can end in an empty statement: ISO C -Wpedantic
 * flags the ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static K_THREAD_STACK_DEFINE(low_prio_stack, 4096);
#pragma GCC diagnostic pop
static struct k_thread low_prio_thread;

/** @brief Start the lower-priority thread and let it run for 5 ms. */
static void low_prio_start(void)
{
	low_prio_stop = false;
	k_thread_create(&low_prio_thread, low_prio_stack, K_THREAD_STACK_SIZEOF(low_prio_stack),
			low_prio_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
	k_msleep(5);
}

/** @brief Stop the lower-priority thread and wait for it to exit. */
static void low_prio_finish(void)
{
	low_prio_stop = true;
	k_thread_join(&low_prio_thread, K_FOREVER);
}

/**
 * @brief Finding 6: a lower-priority thread runs during a 300 ms read.
 */
ZTEST(sdhost_review, test_f6_slow_read_starves_lower_priority)
{
	uint32_t before_runs;

	low_prio_start();

	hw.data_delay_us = INT64_C(300) * 1000;
	before_runs = low_prio_runs;
	int ret = xfer(SD_READ_SINGLE_BLOCK, 4, 1, BLOCK, buf, 200, 1000);
	uint32_t during = low_prio_runs - before_runs;

	low_prio_finish();
	zassert_ok(ret);
	zassert_true(during > 0, "lower-priority thread starved for 300 ms (%u runs)", during);
}

/**
 * @brief Finding 8: a FIFO level above 16 doesn't overrun the FIFO.
 *
 * The 5-bit level is trusted: above 16 the free space wraps and the driver
 * writes a whole block into a full FIFO.
 */
ZTEST(sdhost_review, test_f8_fifo_level_out_of_range)
{
	hw.fill_override = 20;
	(void)xfer(SD_WRITE_SINGLE_BLOCK, 2, 1, BLOCK, buf, 200, 50);
	zassert_equal(hw.overruns, 0, "%d words written into a full FIFO", hw.overruns);
}

/* Finding 9: requests aren't serialised. */
static volatile int race_errors;

/**
 * @brief Read one block and check its first and last words.
 *
 * @param lba Block to read.
 * @param[out] b2 One block of buffer.
 * @return True if the read succeeded with the block's own data.
 */
static bool read_block_ok(uint32_t lba, uint32_t *b2)
{
	return xfer(SD_READ_SINGLE_BLOCK, lba, 1, BLOCK, b2, 200, 1000) == 0 &&
	       b2[0] == pattern(lba, 0) && b2[WORDS(BLOCK) - 1] == pattern(lba, WORDS(BLOCK) - 1);
}

/**
 * @brief Reader thread: read 10 blocks, counting failed or wrong reads.
 *
 * @param a Points to the first block number (100 or 200), which also picks
 *          the buffer.
 * @param b Unused.
 * @param c Unused.
 */
static void reader_fn(void *a, void *b, void *c)
{
	uint32_t lba0 = *(const uint32_t *)a;
	static uint32_t bufs[2][WORDS(BLOCK)];
	uint32_t *b2 = bufs[lba0 == 100 ? 0 : 1];

	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (uint32_t n = 0; n < 10; n++) {
		if (!read_block_ok(lba0 + n, b2)) {
			race_errors++;
		}
	}
}

/* Zephyr's definition macro can end in an empty statement: ISO C -Wpedantic
 * flags the ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static K_THREAD_STACK_ARRAY_DEFINE(reader_stacks, 2, 8192);
#pragma GCC diagnostic pop
static struct k_thread readers[2];

/* First block number of each reader thread. */
static uint32_t reader_lba0[2] = {100, 200};

/**
 * @brief Finding 9: two threads reading at once don't interleave.
 *
 * Today one issues a command mid data phase of the other, and reads get
 * each other's data.
 */
ZTEST(sdhost_review, test_f9_concurrent_requests)
{
	race_errors = 0;
	for (int i = 0; i < 2; i++) {
		k_thread_create(&readers[i], reader_stacks[i], K_THREAD_STACK_SIZEOF(reader_stacks[i]),
				reader_fn, &reader_lba0[i], NULL, NULL,
				K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	}
	for (int i = 0; i < 2; i++) {
		k_thread_join(&readers[i], K_FOREVER);
	}
	zassert_equal(hw.violations, 0, "%d commands issued mid data phase", hw.violations);
	zassert_equal(race_errors, 0, "%d failed or wrong reads", race_errors);
}

/**
 * @brief Finding 10: a block size over the host's 1024-byte limit is refused.
 *
 * The limit is get_host_props()' max_blk_len = 1.
 */
ZTEST(sdhost_review, test_f10_block_size_over_limit)
{
	static uint32_t big[2048 / 4];

	zassert_equal(xfer(SD_READ_SINGLE_BLOCK, 1, 1, 2048, big, 200, 1000), -EINVAL,
		      "2048-byte block accepted");
}

ZTEST_EXPECT_FAIL(sdhost_review, test_f1_write_busy_250ms_after_stop);
ZTEST_EXPECT_FAIL(sdhost_review, test_f2_stale_fifo_after_timeout);
ZTEST_EXPECT_FAIL(sdhost_review, test_f3_core_clock_not_250mhz);
ZTEST_EXPECT_FAIL(sdhost_review, test_f4_data_timeout_follows_clock);
ZTEST_EXPECT_FAIL(sdhost_review, test_f5_data_timeout_forever);
ZTEST_EXPECT_FAIL(sdhost_review, test_f5_timeout_multiplication_overflow);
ZTEST_EXPECT_FAIL(sdhost_review, test_f6_slow_read_starves_lower_priority);
ZTEST_EXPECT_FAIL(sdhost_review, test_f8_fifo_level_out_of_range);
ZTEST_EXPECT_FAIL(sdhost_review, test_f9_concurrent_requests);
ZTEST_EXPECT_FAIL(sdhost_review, test_f10_block_size_over_limit);

ZTEST_SUITE(sdhost_review, NULL, suite_setup, before, NULL, NULL);
