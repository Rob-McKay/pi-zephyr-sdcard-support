/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr's SD stack checks that a read or write lies within the card
 * before sending it (card_read_blocks() and card_write_blocks() in
 * subsys/sd/sd_ops.c). Unpatched, the check was
 * start_block + num_blocks > block_count in 32 bits, so a range ending
 * past block 0xffffffff wrapped round to a small sum and went to the card.
 * Here a fake host controller records what reaches it, and each test
 * checks that a good range reaches it and a bad one is refused with
 * -EINVAL first.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sd/sdmmc.h>
#include <zephyr/ztest.h>

#define BLOCKS 1000U
#define BLOCK_SIZE 512U

/** Data transfers the fake controller received since the last reset. */
static unsigned int transfers;
/** First block of the last data transfer. */
static uint32_t last_block;

/** A transfer buffer, word aligned so the SD stack uses it directly. */
static uint8_t buf[2U * BLOCK_SIZE] __aligned(4);

static struct sd_card card;

/**
 * @brief The fake controller's request handler: records data transfers,
 *        and answers SEND_STATUS with "ready, in transfer state".
 *
 * @param dev Unused.
 * @param cmd The command.
 * @param data The data phase, or NULL for a command without one.
 * @retval 0 Always.
 */
static int fake_request(const struct device *dev, struct sdhc_command *cmd,
			struct sdhc_data *data)
{
	ARG_UNUSED(dev);
	if (data != NULL) {
		transfers++;
		last_block = data->block_addr;
	}
	cmd->response[0] = (uint32_t)SD_R1_RDY_DATA | ((uint32_t)SDMMC_R1_TRANSFER << 9U);
	return 0;
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

/* Only the operations a read or write uses. */
static DEVICE_API(sdhc, fake_sdhc_api) = {
	.request = fake_request,
	.card_busy = fake_card_busy,
};

/*
 * DEVICE_DEFINE's expansion ends in its own ';', so -Wpedantic flags the
 * ';' as extra. Nothing else is exempted.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
DEVICE_DEFINE(fake_sdhc, "fake_sdhc", NULL, NULL, NULL, NULL, POST_KERNEL, 0, &fake_sdhc_api);
#pragma GCC diagnostic pop

/**
 * @brief Set up an initialised high-capacity card of BLOCKS blocks on the
 *        fake controller, and clear the transfer record.
 *
 * @param f Unused.
 */
static void before(void *f)
{
	ARG_UNUSED(f);
	memset(&card, 0, sizeof(card));
	card.sdhc = DEVICE_GET(fake_sdhc);
	card.block_count = BLOCKS;
	card.block_size = BLOCK_SIZE;
	card.type = CARD_SDMMC;
	card.flags = SD_HIGH_CAPACITY_FLAG;
	k_mutex_init(&card.lock);
	transfers = 0;
	last_block = 0;
}

/**
 * @brief Check that a read of a range reaches the controller.
 *
 * @param start First block.
 * @param count Number of blocks.
 */
static void expect_read_sent(uint32_t start, uint32_t count)
{
	zassert_ok(sdmmc_read_blocks(&card, buf, start, count), "read %u+%u", start, count);
	zassert_equal(transfers, 1U, "read %u+%u: %u transfers", start, count, transfers);
	zassert_equal(last_block, start);
}

/**
 * @brief Check that a read and a write of a range are refused before
 *        reaching the controller.
 *
 * @param start First block.
 * @param count Number of blocks.
 */
static void expect_refused(uint32_t start, uint32_t count)
{
	zassert_equal(sdmmc_read_blocks(&card, buf, start, count), -EINVAL, "read %u+%u",
		      start, count);
	zassert_equal(sdmmc_write_blocks(&card, buf, start, count), -EINVAL, "write %u+%u",
		      start, count);
	zassert_equal(transfers, 0U, "%u+%u reached the controller", start, count);
}

/** @brief The first and last blocks, and the whole card, are read. */
ZTEST(sd_bounds, test_in_range_sent)
{
	expect_read_sent(0, 1);
	transfers = 0;
	expect_read_sent(BLOCKS - 1U, 1);
	transfers = 0;
	expect_read_sent(BLOCKS - 2U, 2);
}

/** @brief A write of the last block reaches the controller. */
ZTEST(sd_bounds, test_write_last_block_sent)
{
	zassert_ok(sdmmc_write_blocks(&card, buf, BLOCKS - 1U, 1));
	zassert_equal(transfers, 1U);
	zassert_equal(last_block, BLOCKS - 1U);
}

/** @brief Ranges starting at or running past the end are refused. */
ZTEST(sd_bounds, test_past_end_refused)
{
	expect_refused(BLOCKS, 1);
	expect_refused(BLOCKS - 1U, 2);
	expect_refused(0, BLOCKS + 1U);
}

/** @brief Ranges whose end wraps past block 0xffffffff are refused. */
ZTEST(sd_bounds, test_wrapping_range_refused)
{
	expect_refused(UINT32_MAX, 1);   /* end wraps to 0 */
	expect_refused(UINT32_MAX, 2);   /* end wraps to 1 */
	expect_refused(0xfffffff0U, 0x20U); /* end wraps to 0x10 */
	expect_refused(1, UINT32_MAX);   /* count alone exceeds the card */
}

ZTEST_SUITE(sd_bounds, NULL, NULL, before, NULL, NULL);
