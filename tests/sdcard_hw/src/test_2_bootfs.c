/*
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
static struct {
	uint32_t part_start; /**< first block of the partition */
	uint32_t fat;        /**< first block of the first FAT */
	uint32_t data;       /**< first block of cluster 2 */
	uint32_t spc;        /**< blocks per cluster */
	uint32_t root;       /**< root directory's first cluster */
	uint32_t clusters;   /**< data clusters */
	bool ok;
} fs;

/** A root directory file. */
struct file {
	char name[13];  /**< 8.3 name, e.g. "ZEPHYR.BIN" */
	uint32_t first; /**< first cluster */
	uint32_t size;  /**< bytes */
};

static struct file files[MAX_FILES];
static size_t nfiles;

/**
 * @brief Read the MBR and the FAT32 boot sector into fs.
 *
 * @return 0, or a message saying what is wrong.
 */
static const char *read_layout(void)
{
	static uint8_t blk[SDQ_BLK] __aligned(4);
	const uint8_t *e = &blk[446];
	uint32_t reserved;
	uint32_t fat_size;
	uint32_t total;

	if (disk_access_read(SDQ_DISK, blk, 0, 1) != 0) {
		return "MBR read failed";
	}
	if (blk[510] != 0x55U || blk[511] != 0xaaU) {
		return "no MBR";
	}
	if (e[4] != 0x0bU && e[4] != 0x0cU) {
		return "partition 1 is not FAT32";
	}
	fs.part_start = sys_get_le32(e + 8);
	if (disk_access_read(SDQ_DISK, blk, fs.part_start, 1) != 0) {
		return "boot sector read failed";
	}
	if (blk[510] != 0x55U || blk[511] != 0xaaU || sys_get_le16(&blk[11]) != SDQ_BLK) {
		return "partition 1 has no FAT boot sector with 512-byte sectors";
	}
	fs.spc = blk[13];
	reserved = sys_get_le16(&blk[14]);
	fat_size = sys_get_le32(&blk[36]);
	total = sys_get_le32(&blk[32]);
	fs.root = sys_get_le32(&blk[44]);
	if (fs.spc == 0U || fat_size == 0U || blk[16] == 0U) {
		return "boot sector fields are zero";
	}
	fs.fat = fs.part_start + reserved;
	fs.data = fs.fat + (blk[16] * fat_size);
	fs.clusters = (total - (fs.data - fs.part_start)) / fs.spc;
	if (fs.spc * SDQ_BLK > SDQ_MAX_BLOCKS * SDQ_BLK) {
		return "clusters larger than the test's buffer";
	}
	return NULL;
}

/**
 * @brief Follow the FAT from a cluster.
 *
 * @param cluster Cluster.
 * @param[out] next The next cluster, or FAT_EOC or more at the end.
 * @return 0, or the read's error.
 */
static int fat_next(uint32_t cluster, uint32_t *next)
{
	static uint8_t blk[SDQ_BLK] __aligned(4);
	uint32_t off = cluster * 4U;
	int ret = disk_access_read(SDQ_DISK, blk, fs.fat + (off / SDQ_BLK), 1);

	if (ret == 0) {
		*next = sys_get_le32(&blk[off % SDQ_BLK]) & 0x0fffffffU;
	}
	return ret;
}

/**
 * @brief Tell whether a cluster number lies inside the data area.
 *
 * @param c Cluster.
 * @return True if valid.
 */
static bool cluster_valid(uint32_t c)
{
	return c >= 2U && c < fs.clusters + 2U;
}

/**
 * @brief First block of a cluster.
 *
 * @param c Cluster, at least 2.
 * @return Block number.
 */
static uint32_t cluster_lba(uint32_t c)
{
	return fs.data + ((c - 2U) * fs.spc);
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
 * @return 0, or a message saying what is wrong.
 */
static const char *read_root(void)
{
	uint32_t c = fs.root;
	int hops = 0;

	nfiles = 0;
	while (cluster_valid(c) && hops++ < 64) {
		if (disk_access_read(SDQ_DISK, sdq_rbuf, cluster_lba(c), fs.spc) != 0) {
			return "root directory read failed";
		}
		for (uint32_t off = 0; off < fs.spc * SDQ_BLK; off += 32U) {
			const uint8_t *e = &sdq_rbuf[off];

			if (e[0] == 0U) {
				return NULL;
			}
			if (e[0] == 0xe5U || e[11] == ATTR_LFN || (e[11] & (ATTR_DIR | ATTR_VOL)) != 0U ||
			    nfiles == MAX_FILES) {
				continue;
			}
			short_name(e, files[nfiles].name);
			files[nfiles].first = ((uint32_t)sys_get_le16(&e[20]) << 16) |
					      sys_get_le16(&e[26]);
			files[nfiles].size = sys_get_le32(&e[28]);
			nfiles++;
		}
		if (fat_next(c, &c) != 0) {
			return "FAT read failed";
		}
	}
	return NULL;
}

/**
 * @brief Read one cluster's worth of a file in the given way.
 *
 * @param c Cluster.
 * @param mode How.
 * @param[out] out Where the data starts.
 * @return 0, or the read's error.
 */
static int read_cluster(uint32_t c, enum read_mode mode, const uint8_t **out)
{
	uint32_t lba = cluster_lba(c);
	int ret = 0;

	switch (mode) {
	case BY_CLUSTER:
		ret = disk_access_read(SDQ_DISK, sdq_rbuf, lba, fs.spc);
		*out = sdq_rbuf;
		break;
	case BY_BLOCK:
		for (uint32_t b = 0; b < fs.spc && ret == 0; b++) {
			ret = disk_access_read(SDQ_DISK, &sdq_rbuf[(size_t)b * SDQ_BLK], lba + b, 1);
		}
		*out = sdq_rbuf;
		break;
	default:
		ret = disk_access_read(SDQ_DISK, &sdq_rbuf[3], lba, fs.spc);
		*out = &sdq_rbuf[3];
		break;
	}
	return ret;
}

/**
 * @brief Read a whole file and give its CRC-32.
 *
 * @param f File.
 * @param mode How to read it.
 * @param[out] crc CRC-32 (as zlib's crc32()).
 * @param[out] head The first bytes, if not NULL.
 * @param head_len Size of @p head.
 * @return 0, or a negative errno: -EIO for a read error, -EBADF for a
 *         broken cluster chain.
 */
static int file_crc(const struct file *f, enum read_mode mode, uint32_t *crc, char *head,
		    size_t head_len)
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
		uint32_t n = MIN(left, fs.spc * SDQ_BLK);

		if (!cluster_valid(c)) {
			return -EBADF;
		}
		if (read_cluster(c, mode, &p) != 0) {
			return -EIO;
		}
		if (first && head != NULL) {
			size_t h = MIN((size_t)n, head_len - 1U);

			memcpy(head, p, h);
			head[h] = '\0';
		}
		first = false;
		v = crc32_ieee_update(v, p, n);
		left -= n;
		if (left > 0U && fat_next(c, &c) != 0) {
			return -EIO;
		}
	}
	*crc = v;
	return 0;
}

/**
 * @brief Find a root directory file by its 8.3 name.
 *
 * @param name Name, e.g. "ZEPHYR.BIN".
 * @return The file, or NULL.
 */
static const struct file *find(const char *name)
{
	for (size_t i = 0; i < nfiles; i++) {
		if (strcmp(files[i].name, name) == 0) {
			return &files[i];
		}
	}
	return NULL;
}

/**
 * @brief Suite setup: the common setup, then the partition's layout and
 *        root directory.
 *
 * @return NULL (no fixture).
 */
static void *bootfs_setup(void)
{
	const char *err;

	(void)sdq_setup();
	err = read_layout();
	if (err == NULL) {
		err = read_root();
	}
	fs.ok = err == NULL;
	if (fs.ok) {
		TC_PRINT("boot partition at block %u: %u blocks a cluster, %zu files\n",
			 fs.part_start, fs.spc, nfiles);
	} else {
		TC_PRINT("boot partition not readable: %s\n", err);
	}
	return NULL;
}

/** @brief The MBR and FAT32 boot sector of the card's first partition parse. */
ZTEST(sdcard_2_bootfs, test_1_layout)
{
	zassert_true(fs.ok, "the boot partition could not be read: see above");
	zassert_not_null(find("ZEPHYR.BIN"), "no zephyr.bin in the root directory");
	zassert_not_null(find("CONFIG.TXT"), "no config.txt in the root directory");
}

/** @brief config.txt reads back as the text the firmware booted from. */
ZTEST(sdcard_2_bootfs, test_2_config_txt)
{
	const struct file *f = find("CONFIG.TXT");
	static char text[1024];
	uint32_t crc;

	zassert_not_null(f);
	zassert_ok(file_crc(f, BY_CLUSTER, &crc, text, sizeof(text)));
	zassert_not_null(strstr(text, "kernel=zephyr.bin"), "config.txt reads as:\n%s", text);
}

/** @brief Every file reads the same a cluster at a time, a block at a time and misaligned. */
ZTEST(sdcard_2_bootfs, test_3_files_read_consistently)
{
	zassert_true(fs.ok);
	for (size_t i = 0; i < nfiles; i++) {
		uint32_t crc[3] = {0};
		uint64_t t0 = sdq_now_us();

		for (int m = BY_CLUSTER; m <= BY_MISALIGNED; m++) {
			zassert_ok(file_crc(&files[i], (enum read_mode)m, &crc[m], NULL, 0),
				   "%s, mode %d", files[i].name, m);
		}
		TC_PRINT("%-12s %8u bytes  crc32 %08x  (%llu ms for the 3 reads)\n", files[i].name,
			 files[i].size, crc[0], (sdq_now_us() - t0) / 1000U);
		zassert_equal(crc[0], crc[1], "%s: per-block read differs", files[i].name);
		zassert_equal(crc[0], crc[2], "%s: misaligned read differs", files[i].name);
	}
}

/** @brief zephyr.bin's CRC-32 matches the one the host wrote (zephyr.crc). */
ZTEST(sdcard_2_bootfs, test_4_image_crc_matches_host)
{
	const struct file *bin = find("ZEPHYR.BIN");
	const struct file *crcf = find("ZEPHYR.CRC");
	static char text[16];
	uint32_t crc = 0;
	uint32_t ignored = 0;
	unsigned long want;

	if (crcf == NULL) {
		TC_PRINT("no zephyr.crc on the card (only scripts/sd-qual-test.sh writes it)\n");
		ztest_test_skip();
	}
	zassert_not_null(bin);
	zassert_ok(file_crc(crcf, BY_CLUSTER, &ignored, text, sizeof(text)));
	want = strtoul(text, NULL, 16);
	zassert_ok(file_crc(bin, BY_CLUSTER, &crc, NULL, 0));
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
ZTEST(sdcard_2_bootfs, test_5_multi_block_read_stress)
{
	uint32_t base = fs.ok ? fs.part_start : 0U;
	uint32_t s = sdq.seed ^ 0x510e527fU;
	uint64_t blocks_read = 0;
	uint64_t t0;

	for (uint32_t b = 0; b < SDQ_MAX_BLOCKS; b++) {
		zassert_ok(disk_access_read(SDQ_DISK, &sdq_wbuf[(size_t)b * SDQ_BLK], base + b, 1),
			   "reference block %u", base + b);
	}
	t0 = sdq_now_us();
	for (int i = 0; i < 1000; i++) {
		uint32_t n = 2U + (sdq_rand(&s) % 255U);
		uint32_t first = sdq_rand(&s) % (SDQ_MAX_BLOCKS - n);
		int ret = disk_access_read(SDQ_DISK, sdq_rbuf, base + first, n);

		zassert_ok(ret, "read %d (%u+%u) failed (%d) after %d good multi-block reads", i,
			   base + first, n, ret, i);
		zassert_mem_equal(sdq_rbuf, &sdq_wbuf[(size_t)first * SDQ_BLK], (size_t)n * SDQ_BLK,
				  "read %d (%u+%u): wrong data", i, base + first, n);
		blocks_read += n;
	}
	TC_PRINT("1000 multi-block reads, %llu blocks, %u kB/s\n", blocks_read,
		 sdq_kbps(blocks_read * SDQ_BLK, sdq_now_us() - t0));
}

ZTEST_SUITE(sdcard_2_bootfs, NULL, bootfs_setup, sdq_before, sdq_after, NULL);
