/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr's SD stack switches a card to a 4-bit bus in sdmmc_init_hs()
 * (subsys/sd/sdmmc.c). Unpatched, it did so only after switching the card
 * to High Speed, so a card without High Speed stayed on one data line, at
 * a quarter of the speed. Every SD memory card supports 4-bit transfers:
 * "Since the SD Memory Card shall support at least the two bus modes 1-bit
 * or 4-bit width, then any SD Card shall set at least bits 0 and 2
 * (SD_BUS_WIDTH="0101")" (SD Physical Layer Simplified Specification
 * v9.10, section 5.6, SD_BUS_WIDTHS, p. 244).
 *
 * Here sd_init() runs against a fake card on a fake host controller, which
 * answers the identification and set-up commands and records the bus
 * width the card and the host end up with.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/ztest.h>

/** The fake card's relative card address. */
#define RCA 0x1234U
/** C_SIZE of a 2 GB-class card: (3771 + 1) * 1024 = 3862528 blocks. */
#define C_SIZE 3771U

/** What the fake card and host support, set by each test. */
struct fake_config {
	/** The card supports High Speed (CMD6 function group 1, function 1). */
	bool card_hs;
	/** SD_BUS_WIDTHS in the card's SCR. */
	uint8_t card_widths;
	/** The host supports a 4-bit bus. */
	bool host_4bit;
};

/** What the fake saw. */
struct fake_record {
	/** The last command was CMD55, so the next is an application command. */
	bool app_cmd;
	/** ACMD6 (SET_BUS_WIDTH) commands received. */
	unsigned int acmd6_count;
	/** The last ACMD6 argument. */
	uint32_t acmd6_arg;
	/** The host's I/O settings as last set. */
	struct sdhc_io io;
};

/** The fake controller's device data: what it supports and what it saw. */
struct fake_sdhc {
	struct fake_config cfg;
	struct fake_record rec;
};

/** The tests' state. */
struct sd_bus_width_fixture {
	struct fake_sdhc *fake; /* the fake controller's data */
	struct sd_card card;
};

/** @brief R1 card status: ready for data, in transfer state. @return The status. */
static uint32_t r1_tran(void)
{
	return (uint32_t)SD_R1_RDY_DATA | ((uint32_t)SDMMC_R1_TRANSFER << 9U);
}

/**
 * @brief Zero a command's data block, if it has one.
 *
 * @param[out] data The data phase, or NULL.
 * @return The data block, or NULL.
 */
static uint8_t *clear_data(struct sdhc_data *data)
{
	if (data == NULL) {
		return NULL;
	}
	memset(data->data, 0, (size_t)data->block_size * data->blocks);
	return (uint8_t *)data->data;
}

/**
 * @brief Answer an application command (one following CMD55).
 *
 * @param[in,out] f The fake controller's data.
 * @param[in,out] cmd The command; its response is filled in.
 * @param[out] data The data phase, or NULL.
 * @retval 0 Answered.
 * @retval -EINVAL An SCR read without a data block.
 */
static int fake_app_command(struct fake_sdhc *f, struct sdhc_command *cmd, struct sdhc_data *data)
{
	uint8_t *buf = clear_data(data);

	switch (cmd->opcode) {
	case SD_APP_SEND_OP_COND:
		/* Powered up (bit 31), high capacity (bit 30), 2.7-3.6 V. */
		cmd->response[0] = BIT(31) | BIT(30) | 0x00ff8000U;
		break;
	case SD_APP_SEND_SCR:
		/* SCR (big-endian): structure 0 and SD_SPEC 2 (Version 2.00 or
		 * later); SD_SECURITY 3 and SD_BUS_WIDTHS; SD_SPEC3 (Version
		 * 3.00 or later), so the switch function's speed bits count.
		 */
		if (buf == NULL) {
			return -EINVAL;
		}
		buf[0] = 0x02U;
		buf[1] = (uint8_t)(0x30U | f->cfg.card_widths);
		buf[2] = 0x80U;
		cmd->response[0] = r1_tran();
		break;
	case SD_APP_SET_BUS_WIDTH:
		f->rec.acmd6_count++;
		f->rec.acmd6_arg = cmd->arg;
		cmd->response[0] = r1_tran();
		break;
	default:
		cmd->response[0] = r1_tran();
		break;
	}
	return 0;
}

/**
 * @brief Fill in a CMD6 (SWITCH_FUNC) status block.
 *
 * Function group 1 (bus speed) reports Default Speed, and High Speed if
 * the card supports it; a switch to High Speed reports it selected.
 *
 * @param card_hs The card supports High Speed.
 * @param arg The CMD6 argument.
 * @param[out] status The 64-byte status block.
 */
static void fake_switch_status(bool card_hs, uint32_t arg, uint8_t *status)
{
	memset(status, 0, 64);
	/* Bits 415:400: group 1 support, big-endian, so byte 13 is bits 407:400. */
	status[13] = card_hs ? 0x03U : 0x01U;
	/* Bits 379:376: group 1 function selected (0xf: no change). */
	if ((arg & BIT(31)) != 0U && (arg & 0xfU) == 1U && card_hs) {
		status[16] = 0x01U;
	} else {
		status[16] = 0x00U;
	}
}

/**
 * @brief Fill in the CSD of a version 2.0 (high capacity) card.
 *
 * @param[out] response The response words, as the SD stack decodes them.
 */
static void fake_csd(uint32_t *response)
{
	response[3] = 0x40000000U | 0x32U;        /* CSD_STRUCTURE 1; TRAN_SPEED 25 MHz */
	response[2] = (0x5b5U << 20) | (0x9U << 16) | ((C_SIZE >> 16) & 0x3fU);
	response[1] = (C_SIZE & 0xffffU) << 16;  /* C_SIZE 15:0 */
	response[0] = 0U;
}

/**
 * @brief The fake controller's request handler: the fake card's answers.
 *
 * @param dev The fake controller; its data is the struct fake_sdhc.
 * @param[in,out] cmd The command; its response is filled in.
 * @param[out] data The data phase, or NULL.
 * @retval 0 Answered.
 * @retval -EINVAL A CMD6 without a data block.
 */
static int fake_request(const struct device *dev, struct sdhc_command *cmd,
			struct sdhc_data *data)
{
	struct fake_sdhc *f = dev->data;

	if (f->rec.app_cmd) {
		f->rec.app_cmd = false;
		return fake_app_command(f, cmd, data);
	}
	memset(cmd->response, 0, sizeof(cmd->response));
	switch (cmd->opcode) {
	case SD_GO_IDLE_STATE:
		break;
	case SD_SEND_IF_COND:
		cmd->response[0] = cmd->arg & 0xfffU; /* echo: 3.3 V accepted */
		break;
	case SD_APP_CMD:
		f->rec.app_cmd = true;
		cmd->response[0] = (uint32_t)SD_R1_APP_CMD | r1_tran();
		break;
	case SD_ALL_SEND_CID:
		cmd->response[0] = 0x12345678U;
		break;
	case SD_SEND_RELATIVE_ADDR:
		cmd->response[0] = RCA << 16;
		break;
	case SD_SEND_CSD:
		fake_csd(cmd->response);
		break;
	case SD_SWITCH:
		if (data == NULL) {
			return -EINVAL;
		}
		fake_switch_status(f->cfg.card_hs, cmd->arg, (uint8_t *)data->data);
		cmd->response[0] = r1_tran();
		break;
	default:
		(void)clear_data(data);
		cmd->response[0] = r1_tran();
		break;
	}
	return 0;
}

/**
 * @brief The fake controller's I/O settings: recorded.
 *
 * @param dev The fake controller; its data is the struct fake_sdhc.
 * @param io The settings.
 * @retval 0 Applied.
 */
static int fake_set_io(const struct device *dev, struct sdhc_io *io)
{
	struct fake_sdhc *f = dev->data;

	f->rec.io = *io;
	return 0;
}

/**
 * @brief The fake controller's properties: 400 kHz to 50 MHz, 3.3 V, High
 *        Speed, and a 4-bit bus if the test says so.
 *
 * @param dev The fake controller; its data is the struct fake_sdhc.
 * @param[out] props The properties.
 * @retval 0 Filled in.
 */
static int fake_get_host_props(const struct device *dev, struct sdhc_host_props *props)
{
	const struct fake_sdhc *f = dev->data;

	memset(props, 0, sizeof(*props));
	props->f_min = SDMMC_CLOCK_400KHZ;
	props->f_max = SD_CLOCK_50MHZ;
	props->host_caps.vol_330_support = true;
	props->host_caps.high_spd_support = true;
	props->bus_4_bit_support = f->cfg.host_4bit;
	return 0;
}

/**
 * @brief The fake controller's card detect: a card is present.
 *
 * @param dev Unused.
 * @retval 1 Present.
 */
static int fake_get_card_present(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 1;
}

/**
 * @brief The fake controller's busy check: never busy.
 *
 * @param dev Unused.
 * @retval 0 Not busy.
 */
static int fake_card_busy(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

/**
 * @brief The fake controller's reset: nothing to do.
 *
 * @param dev Unused.
 * @retval 0 Reset.
 */
static int fake_reset(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static DEVICE_API(sdhc, fake_sdhc_api) = {
	.reset = fake_reset,
	.request = fake_request,
	.set_io = fake_set_io,
	.get_card_present = fake_get_card_present,
	.card_busy = fake_card_busy,
	.get_host_props = fake_get_host_props,
};

/** The fake controller's device data. */
static struct fake_sdhc fake_sdhc_data;

/*
 * DEVICE_DEFINE's expansion ends in its own ';', so -Wpedantic flags the
 * ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
DEVICE_DEFINE(fake_sdhc, "fake_sdhc", NULL, NULL, &fake_sdhc_data, NULL, POST_KERNEL, 0,
	      &fake_sdhc_api);
#pragma GCC diagnostic pop

/**
 * @brief Initialise the fake card through the SD stack and check it is
 *        identified.
 *
 * @param[in,out] fx The fixture.
 * @param card_hs The card supports High Speed.
 * @param card_widths SD_BUS_WIDTHS in its SCR.
 * @param host_4bit The host supports a 4-bit bus.
 */
static void init_card(struct sd_bus_width_fixture *fx, bool card_hs, uint8_t card_widths,
		      bool host_4bit)
{
	fx->fake->cfg = (struct fake_config){
		.card_hs = card_hs,
		.card_widths = card_widths,
		.host_4bit = host_4bit,
	};
	memset(&fx->fake->rec, 0, sizeof(fx->fake->rec));
	memset(&fx->card, 0, sizeof(fx->card));
	zassert_ok(sd_init(DEVICE_GET(fake_sdhc), &fx->card), "sd_init");
	zassert_equal(fx->card.type, CARD_SDMMC);
	zassert_equal(fx->card.block_count, ((C_SIZE + 1U) * 1024U), "block count %u",
		      fx->card.block_count);
}

/**
 * @brief Suite setup: the fixture, pointing at the fake controller's data.
 *
 * @return The fixture.
 *
 * @par Global state
 * Changes @c fixture (its controller pointer).
 */
static void *setup(void)
{
	static struct sd_bus_width_fixture fixture;

	fixture.fake = DEVICE_GET(fake_sdhc)->data;
	return &fixture;
}

/** @brief A card without High Speed is switched to 4 bits, at Default Speed. */
ZTEST_F(sd_bus_width, test_default_speed_card_gets_4bit)
{
	init_card(fixture, false, 0x5U, true);
	zassert_equal(fixture->fake->rec.acmd6_count, 1U, "ACMD6 sent %u times",
		      fixture->fake->rec.acmd6_count);
	zassert_equal(fixture->fake->rec.acmd6_arg, 2U, "ACMD6 argument %u, not 2 (4 bits)",
		      fixture->fake->rec.acmd6_arg);
	zassert_equal(fixture->fake->rec.io.bus_width, SDHC_BUS_WIDTH4BIT, "host bus width %d",
		      fixture->fake->rec.io.bus_width);
	zassert_equal(fixture->fake->rec.io.timing, SDHC_TIMING_LEGACY, "timing %d",
		      fixture->fake->rec.io.timing);
	zassert_equal(fixture->fake->rec.io.clock, SD_CLOCK_25MHZ, "clock %u",
		      fixture->fake->rec.io.clock);
}

/** @brief A High Speed card is switched to High Speed and 4 bits, as before. */
ZTEST_F(sd_bus_width, test_high_speed_card_gets_4bit)
{
	init_card(fixture, true, 0x5U, true);
	zassert_equal(fixture->fake->rec.acmd6_count, 1U, "ACMD6 sent %u times",
		      fixture->fake->rec.acmd6_count);
	zassert_equal(fixture->fake->rec.acmd6_arg, 2U);
	zassert_equal(fixture->fake->rec.io.bus_width, SDHC_BUS_WIDTH4BIT);
	zassert_equal(fixture->fake->rec.io.timing, SDHC_TIMING_HS, "timing %d",
		      fixture->fake->rec.io.timing);
	zassert_equal(fixture->fake->rec.io.clock, SD_CLOCK_50MHZ, "clock %u",
		      fixture->fake->rec.io.clock);
}

/** @brief A host without a 4-bit bus keeps the card on 1 bit, and sends no ACMD6. */
ZTEST_F(sd_bus_width, test_1bit_host_stays_1bit)
{
	init_card(fixture, false, 0x5U, false);
	zassert_equal(fixture->fake->rec.acmd6_count, 0U);
	zassert_equal(fixture->fake->rec.io.bus_width, SDHC_BUS_WIDTH1BIT);
}

/** @brief A card whose SCR doesn't list 4 bits (against the spec) stays on 1 bit. */
ZTEST_F(sd_bus_width, test_1bit_card_stays_1bit)
{
	init_card(fixture, false, 0x1U, true);
	zassert_equal(fixture->fake->rec.acmd6_count, 0U);
	zassert_equal(fixture->fake->rec.io.bus_width, SDHC_BUS_WIDTH1BIT);
}

ZTEST_SUITE(sd_bus_width, NULL, setup, NULL, NULL, NULL);
