/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suite 2, read-only: the card's boot partition, as the Pi's firmware
 * wrote it, read through the driver. The FAT32 file system is walked by
 * hand (no file system library), and every file in its root directory is
 * read three ways: a cluster per request, a block per request, and into a
 * misaligned buffer. The three CRCs must agree, and zephyr.bin's must
 * match zephyr.crc, written to the card by the host
 * (scripts/sd-qual-test.sh). Runs on any card, written or not.
 */

#include <string.h>

#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "common.h"

/* Directory entry attributes. */
#define ATTR_DIR 0x10U
#define ATTR_VOL 0x08U
#define ATTR_LFN 0x0fU

#define FAT_EOC  0x0ffffff8U
#define MAX_FILES 16

/** How a file is read. */
enum read_mode {
	BY_CLUSTER,    /**< one request per cluster */
	BY_BLOCK,      /**< one request per block */
	BY_MISALIGNED, /**< one per cluster, into a buffer at an odd address */
};

/** The partition's FAT32 layout. */
struct fat_layout {
	uint32_t part_start; /**< first block of the partition */
	uint32_t fat;        /**< first block of the first FAT */
	uint32_t data;       /**< first block of cluster 2 */
	uint32_t spc;        /**< blocks per cluster */
	uint32_t root;       /**< root directory's first cluster */
	uint32_t clusters;   /**< data clusters */
	bool ok;
};

/** A root directory file. */
struct file {
	char name[13];  /**< 8.3 name, e.g. "ZEPHYR.BIN" */
	uint32_t first; /**< first cluster */
	uint32_t size;  /**< bytes */
};

/** The suite's state: the shared card, and what was read of its boot partition. */
struct sdcard_2_bootfs_fixture {
	struct sdq *q;
	struct fat_layout fs;
	struct file files[MAX_FILES]; /**< the root directory's files */
	size_t nfiles;
	uint8_t blk[SDQ_BLK] __aligned(4); /**< one block, for the MBR, boot sector and FAT */
	char text[1024];                   /**< config.txt's start */
};

/**
 * @brief Read the MBR and the FAT32 boot sector into fs.
 *
 * @param[in,out] bf The suite's state.
 * @return 0, or a message saying what is wrong.
 */
static const char *read_layout(struct sdcard_2_bootfs_fixture *bf)
{
	const uint8_t *e = &bf->blk[446];
	uint32_t reserved;
	uint32_t fat_size;
	uint32_t total;

	if (disk_access_read(SDQ_DISK, bf->blk, 0, 1) != 0) {
		return "MBR read failed";
	}
	if (bf->blk[510] != 0x55U || bf->blk[511] != 0xaaU) {
		return "no MBR";
	}
	if (e[4] != 0x0bU && e[4] != 0x0cU) {
		return "partition 1 is not FAT32";
	}
	bf->fs.part_start = sys_get_le32(e + 8);
	if (disk_access_read(SDQ_DISK, bf->blk, bf->fs.part_start, 1) != 0) {
		return "boot sector read failed";
	}
	if (bf->blk[510] != 0x55U || bf->blk[511] != 0xaaU ||
	    sys_get_le16(&bf->blk[11]) != SDQ_BLK) {
		return "partition 1 has no FAT boot sector with 512-byte sectors";
	}
	bf->fs.spc = bf->blk[13];
	reserved = sys_get_le16(&bf->blk[14]);
	fat_size = sys_get_le32(&bf->blk[36]);
	total = sys_get_le32(&bf->blk[32]);
	bf->fs.root = sys_get_le32(&bf->blk[44]);
	if (bf->fs.spc == 0U || fat_size == 0U || bf->blk[16] == 0U) {
		return "boot sector fields are zero";
	}
	bf->fs.fat = bf->fs.part_start + reserved;
	bf->fs.data = bf->fs.fat + (bf->blk[16] * fat_size);
	bf->fs.clusters = (total - (bf->fs.data - bf->fs.part_start)) / bf->fs.spc;
	if (bf->fs.spc * SDQ_BLK > SDQ_MAX_BLOCKS * SDQ_BLK) {
		return "clusters larger than the test's buffer";
	}
	return NULL;
}

/**
 * @brief Follow the FAT from a cluster.
 *
 * @param[in,out] bf The suite's state.
 * @param cluster Cluster.
 * @param[out] next The next cluster, or FAT_EOC or more at the end.
 * @return 0, or the read's error.
 */
static int fat_next(struct sdcard_2_bootfs_fixture *bf, uint32_t cluster, uint32_t *next)
{
	uint32_t off = cluster * 4U;
	int ret = disk_access_read(SDQ_DISK, bf->blk, bf->fs.fat + (off / SDQ_BLK), 1);

	if (ret == 0) {
		*next = sys_get_le32(&bf->blk[off % SDQ_BLK]) & 0x0fffffffU;
	}
	return ret;
}

/**
 * @brief Tell whether a cluster number lies inside the data area.
 *
 * @param[in,out] bf The suite's state.
 * @param c Cluster.
 * @return True if valid.
 */
static bool cluster_valid(struct sdcard_2_bootfs_fixture *bf, uint32_t c)
{
	return c >= 2U && c < bf->fs.clusters + 2U;
}

/**
 * @brief First block of a cluster.
 *
 * @param[in,out] bf The suite's state.
 * @param c Cluster, at least 2.
 * @return Block number.
 */
static uint32_t cluster_lba(struct sdcard_2_bootfs_fixture *bf, uint32_t c)
{
	return bf->fs.data + ((c - 2U) * bf->fs.spc);
}

/**
 * @brief Make an 8.3 name ("CONFIG.TXT") from a directory entry.
 *
 * @param e Directory entry.
 * @param[out] out 13 bytes.
 */
static void short_name(const uint8_t *e, char *out)
{
	size_t n = 0;

	for (size_t i = 0; i < 8U && e[i] != ' '; i++) {
		out[n++] = (char)e[i];
	}
	if (e[8] != ' ') {
		out[n++] = '.';
		for (size_t i = 8; i < 11U && e[i] != ' '; i++) {
			out[n++] = (char)e[i];
		}
	}
	out[n] = '\0';
}

/**
 * @brief Collect the root directory's regular files into files[].
 *
 * @param[in,out] bf The suite's state.
 * @return 0, or a message saying what is wrong.
 */
static const char *read_root(struct sdcard_2_bootfs_fixture *bf)
{
	uint32_t c = bf->fs.root;
	int hops = 0;

	bf->nfiles = 0;
	while (cluster_valid(bf, c) && hops++ < 64) {
		if (disk_access_read(SDQ_DISK, bf->q->rbuf, cluster_lba(bf, c), bf->fs.spc) != 0) {
			return "root directory read failed";
		}
		for (uint32_t off = 0; off < bf->fs.spc * SDQ_BLK; off += 32U) {
			const uint8_t *e = &bf->q->rbuf[off];

			if (e[0] == 0U) {
				return NULL;
			}
			if (e[0] == 0xe5U || e[11] == ATTR_LFN ||
			    (e[11] & (ATTR_DIR | ATTR_VOL)) != 0U || bf->nfiles == MAX_FILES) {
				continue;
			}
			short_name(e, bf->files[bf->nfiles].name);
			bf->files[bf->nfiles].first =
				((uint32_t)sys_get_le16(&e[20]) << 16) | sys_get_le16(&e[26]);
			bf->files[bf->nfiles].size = sys_get_le32(&e[28]);
			bf->nfiles++;
		}
		if (fat_next(bf, c, &c) != 0) {
			return "FAT read failed";
		}
	}
	return NULL;
}

/**
 * @brief Read one cluster's worth of a file in the given way.
 *
 * @param[in,out] bf The suite's state.
 * @param c Cluster.
 * @param mode How.
 * @param[out] out Where the data starts.
 * @return 0, or the read's error.
 */
static int read_cluster(struct sdcard_2_bootfs_fixture *bf, uint32_t c, enum read_mode mode,
			const uint8_t **out)
{
	uint32_t lba = cluster_lba(bf, c);
	int ret = 0;

	switch (mode) {
	case BY_CLUSTER:
		ret = disk_access_read(SDQ_DISK, bf->q->rbuf, lba, bf->fs.spc);
		*out = bf->q->rbuf;
		break;
	case BY_BLOCK:
		for (uint32_t b = 0; b < bf->fs.spc && ret == 0; b++) {
			ret = disk_access_read(SDQ_DISK, &bf->q->rbuf[(size_t)b * SDQ_BLK], lba + b,
					       1);
		}
		*out = bf->q->rbuf;
		break;
	default:
		ret = disk_access_read(SDQ_DISK, &bf->q->rbuf[3], lba, bf->fs.spc);
		*out = &bf->q->rbuf[3];
		break;
	}
	return ret;
}

/**
 * @brief Read a whole file and give its CRC-32.
 *
 * @param[in,out] bf The suite's state.
 * @param f File.
 * @param mode How to read it.
 * @param[out] crc CRC-32 (as zlib's crc32()).
 * @param[out] head The first bytes, if not NULL.
 * @param head_len Size of @p head.
 * @return 0, or a negative errno: -EIO for a read error, -EBADF for a
 *         broken cluster chain.
 */
static int file_crc(struct sdcard_2_bootfs_fixture *bf, const struct file *f, enum read_mode mode,
		    uint32_t *crc, char *head, size_t head_len)
{
	uint32_t left;
	uint32_t c;
	uint32_t v = 0;
	bool first = true;

	if (f == NULL) {
		return -EINVAL;
	}
	left = f->size;
	c = f->first;

	while (left > 0U) {
		const uint8_t *p;
		uint32_t n = MIN(left, (bf->fs.spc * SDQ_BLK));

		if (!cluster_valid(bf, c)) {
			return -EBADF;
		}
		if (read_cluster(bf, c, mode, &p) != 0) {
			return -EIO;
		}
		if (first && head != NULL) {
			size_t h = MIN((size_t)n, (head_len - 1U));

			memcpy(head, p, h);
			head[h] = '\0';
		}
		first = false;
		v = crc32_ieee_update(v, p, n);
		left -= n;
		if (left > 0U && fat_next(bf, c, &c) != 0) {
			return -EIO;
		}
	}
	*crc = v;
	return 0;
}

/**
 * @brief Find a root directory file by its 8.3 name.
 *
 * @param[in,out] bf The suite's state.
 * @param name Name, e.g. "ZEPHYR.BIN".
 * @return The file, or NULL.
 */
static const struct file *find(struct sdcard_2_bootfs_fixture *bf, const char *name)
{
	for (size_t i = 0; i < bf->nfiles; i++) {
		if (strcmp(bf->files[i].name, name) == 0) {
			return &bf->files[i];
		}
	}
	return NULL;
}

/**
 * @brief Suite setup: the common setup, then the partition's layout and
 *        root directory.
 *
 * @return The fixture.
 *
 * @par Global state
 * Changes @c fixture.
 */
static void *bootfs_setup(void)
{
	static struct sdcard_2_bootfs_fixture fixture;
	struct sdcard_2_bootfs_fixture *bf = &fixture;
	const char *err;

	bf->q = sdq_setup();
	err = read_layout(bf);
	if (err == NULL) {
		err = read_root(bf);
	}
	bf->fs.ok = err == NULL;
	if (bf->fs.ok) {
		TC_PRINT("boot partition at block %u: %u blocks a cluster, %zu files\n",
			 bf->fs.part_start, bf->fs.spc, bf->nfiles);
	} else {
		TC_PRINT("boot partition not readable: %s\n", err);
	}
	return bf;
}

/**
 * @brief Before each test: check the card (sdq_before()).
 *
 * @param f The fixture.
 */
static void bootfs_before(void *f)
{
	sdq_before(((struct sdcard_2_bootfs_fixture *)f)->q);
}

/**
 * @brief After each test: check the driver (sdq_after()).
 *
 * @param f The fixture.
 */
static void bootfs_after(void *f)
{
	sdq_after(((struct sdcard_2_bootfs_fixture *)f)->q);
}

/** @brief The MBR and FAT32 boot sector of the card's first partition parse. */
ZTEST_F(sdcard_2_bootfs, test_1_layout)
{
	zassert_true(fixture->fs.ok, "the boot partition could not be read: see above");
	zassert_not_null(find(fixture, "ZEPHYR.BIN"), "no zephyr.bin in the root directory");
	zassert_not_null(find(fixture, "CONFIG.TXT"), "no config.txt in the root directory");
}

/** @brief config.txt reads back as the text the firmware booted from. */
ZTEST_F(sdcard_2_bootfs, test_2_config_txt)
{
	const struct file *f = find(fixture, "CONFIG.TXT");
	char *text = fixture->text;
	uint32_t crc;

	zassert_not_null(f);
	zassert_ok(file_crc(fixture, f, BY_CLUSTER, &crc, text, sizeof(fixture->text)));
	zassert_not_null(strstr(text, "kernel=zephyr.bin"), "config.txt reads as:\n%s", text);
}

/** @brief Every file reads the same a cluster at a time, a block at a time and misaligned. */
ZTEST_F(sdcard_2_bootfs, test_3_files_read_consistently)
{
	zassert_true(fixture->fs.ok);
	for (size_t i = 0; i < fixture->nfiles; i++) {
		uint32_t crc[3] = {0};
		uint64_t t0 = sdq_now_us();

		for (int m = BY_CLUSTER; m <= BY_MISALIGNED; m++) {
			zassert_ok(file_crc(fixture, &fixture->files[i], (enum read_mode)m, &crc[m],
					    NULL, 0),
				   "%s, mode %d", fixture->files[i].name, m);
		}
		TC_PRINT("%-12s %8u bytes  crc32 %08x  (%llu ms for the 3 reads)\n",
			 fixture->files[i].name, fixture->files[i].size, crc[0],
			 ((sdq_now_us() - t0) / 1000U));
		zassert_equal(crc[0], crc[1], "%s: per-block read differs", fixture->files[i].name);
		zassert_equal(crc[0], crc[2], "%s: misaligned read differs",
			      fixture->files[i].name);
	}
}

/** @brief zephyr.bin's CRC-32 matches the one the host wrote (zephyr.crc). */
ZTEST_F(sdcard_2_bootfs, test_4_image_crc_matches_host)
{
	const struct file *bin = find(fixture, "ZEPHYR.BIN");
	const struct file *crcf = find(fixture, "ZEPHYR.CRC");
	char text[16];
	uint32_t crc = 0;
	uint32_t ignored = 0;
	unsigned long want;

	if (crcf == NULL) {
		TC_PRINT("no zephyr.crc on the card (only scripts/sd-qual-test.sh writes it)\n");
		ztest_test_skip();
	}
	zassert_not_null(bin);
	zassert_ok(file_crc(fixture, crcf, BY_CLUSTER, &ignored, text, sizeof(text)));
	want = strtoul(text, NULL, 16);
	zassert_ok(file_crc(fixture, bin, BY_CLUSTER, &crc, NULL, 0));
	zassert_equal(crc, (uint32_t)want, "zephyr.bin crc32 %08x, the host computed %08lx", crc,
		      want);
}

/**
 * @brief 1000 multi-block reads (2-256 blocks) at random places in the
 *        partition's first 1 MB match a block-at-a-time reference copy.
 *
 * Read-only, so it runs on any card. An open-ended multi-block read (CMD18
 * then CMD12) has to stop the card between blocks; this checks that every
 * one ends cleanly and returns the right data.
 */
ZTEST_F(sdcard_2_bootfs, test_5_multi_block_read_stress)
{
	uint32_t base = fixture->fs.ok ? fixture->fs.part_start : 0U;
	uint32_t s = fixture->q->seed ^ 0x510e527fU;
	uint64_t blocks_read = 0;
	uint64_t t0;

	for (uint32_t b = 0; b < SDQ_MAX_BLOCKS; b++) {
		zassert_ok(disk_access_read(SDQ_DISK, &fixture->q->wbuf[(size_t)b * SDQ_BLK],
					    base + b, 1),
			   "reference block %u", (base + b));
	}
	t0 = sdq_now_us();
	for (int i = 0; i < 1000; i++) {
		uint32_t n = 2U + (sdq_rand(&s) % 255U);
		uint32_t first = sdq_rand(&s) % (SDQ_MAX_BLOCKS - n);
		int ret = disk_access_read(SDQ_DISK, fixture->q->rbuf, base + first, n);

		zassert_ok(ret, "read %d (%u+%u) failed (%d) after %d good multi-block reads", i,
			   (base + first), n, ret, i);
		zassert_mem_equal(fixture->q->rbuf, &fixture->q->wbuf[(size_t)first * SDQ_BLK],
				  ((size_t)n * SDQ_BLK), "read %d (%u+%u): wrong data", i,
				  (base + first), n);
		blocks_read += n;
	}
	TC_PRINT("1000 multi-block reads, %llu blocks, %u kB/s\n", blocks_read,
		 sdq_kbps(blocks_read * SDQ_BLK, sdq_now_us() - t0));
}

ZTEST_SUITE(sdcard_2_bootfs, NULL, bootfs_setup, bootfs_before, bootfs_after, NULL);
