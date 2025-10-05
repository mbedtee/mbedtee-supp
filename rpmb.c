// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Xing Loong <xing.xl.loong@gmail.com>
 * RPMB supplicant for mbedtee-rpmbfs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/mmc/ioctl.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>

#include "supp.h"

#define MMC_RSP_PRESENT	(1 << 0)
#define MMC_RSP_CRC	(1 << 1)
#define MMC_RSP_OPCODE	(1 << 4)
#define MMC_RSP_R1	(MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_CMD_ADTC	(1 << 5)
#define MMC_CMD_AC	(0)

#define RPMB_UNIT_SIZE	(128 * 1024)
#define MMC_BLK_SIZE	512
#define RPMB_BLOCK_SIZE	256
#define RPMB_MAX_BLOCKS_PER_CMD	65535
#define RPMB_MAX_TOTAL_BLOCKS	65536
#define RPMB_IOCTL_MAX_RETRIES	3
#define RPMB_IOCTL_RETRY_DELAY_US	10000

static int rpmb_fd = -1;
static char rpmb_dev_path[256];
static int rpmb_rel_wr_sec_c = 0;
static size_t rpmb_cached_size = 0;
static bool rpmb_in_rpmb_partition = false;  /* Track current partition state */
static pthread_mutex_t rpmb_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Read a uint64 value from a sysfs file */
static int read_sysfs_u64(const char *path, unsigned long long *out)
{
	FILE *fp = fopen(path, "r");
	char buf[64];
	char *end = NULL;

	if (!fp)
		return -ENOENT;

	if (!fgets(buf, sizeof(buf), fp)) {
		fclose(fp);
		return -EIO;
	}
	fclose(fp);

	*out = strtoull(buf, &end, 0);
	return (end == buf) ? -EINVAL : 0;
}

static int rpmb_probe_device(size_t *size_out)
{
	DIR *dir;
	struct dirent *d;
	char path[512];
	char blk_name[32];
	const char *p;
	unsigned long long mult, rel;
	int i = 0, slen = 0;

	if (rpmb_cached_size > 0) {
		if (size_out)
			*size_out = rpmb_cached_size;
		return 0;
	}

	if (rpmb_dev_path[0] == '\0')
		return -ENODEV;

	/* Extract mmcblkX from /dev/mmcblkXrpmb */
	p = supp_strstr(rpmb_dev_path, "mmcblk");
	if (!p)
		return -EINVAL;

	for (i = 0; p[i] && p[i] != 'r' && i < sizeof(blk_name) - 1; i++)
		blk_name[i] = p[i];

	blk_name[i] = '\0';

	/* Scan /sys/bus/mmc/devices/ for matching device */
	dir = opendir("/sys/bus/mmc/devices");
	if (!dir) {
		EMSG("Failed to open /sys/bus/mmc/devices\n");
		return -ENOENT;
	}

	while ((d = readdir(dir)) != NULL) {
		if (d->d_name[0] == '.')
			continue;

		/* Match the sysfs device that owns blk_name */
		slen = supp_strlcpy(path, "/sys/bus/mmc/devices/", sizeof(path));
		slen = supp_strlcat(path, d->d_name, sizeof(path));
		slen = supp_strlcat(path, "/block/", sizeof(path));
		slen = supp_strlcat(path, blk_name, sizeof(path));
		if (slen >= sizeof(path))
			continue;
		if (stat(path, &(struct stat){0}) != 0)
			continue;

		slen = supp_strlcpy(path, "/sys/bus/mmc/devices/", sizeof(path));
		slen = supp_strlcat(path, d->d_name, sizeof(path));
		slen = supp_strlcat(path, "/raw_rpmb_size_mult", sizeof(path));
		if (slen >= sizeof(path))
			continue;

		if (read_sysfs_u64(path, &mult) != 0) {
			slen = supp_strlcpy(path, "/sys/bus/mmc/devices/", sizeof(path));
			slen = supp_strlcat(path, d->d_name, sizeof(path));
			slen = supp_strlcat(path, "/rpmb_size_mult", sizeof(path));
			if (slen >= sizeof(path))
				continue;
			if (read_sysfs_u64(path, &mult) != 0)
				continue;
		}

		if (mult > 0) {
			if (mult > (RPMB_MAX_TOTAL_BLOCKS * RPMB_BLOCK_SIZE) / RPMB_UNIT_SIZE)
				mult = (RPMB_MAX_TOTAL_BLOCKS * RPMB_BLOCK_SIZE) / RPMB_UNIT_SIZE;
			rpmb_cached_size = mult * RPMB_UNIT_SIZE;

			if (size_out)
				*size_out = rpmb_cached_size;

			/* Read rel_sectors if available */
			slen = supp_strlcpy(path, "/sys/bus/mmc/devices/", sizeof(path));
			slen = supp_strlcat(path, d->d_name, sizeof(path));
			slen = supp_strlcat(path, "/rel_sectors", sizeof(path));

			if (slen < sizeof(path) && read_sysfs_u64(path, &rel) == 0)
				rpmb_rel_wr_sec_c = rel * 2; /* 512B to 256B */
			else
				rpmb_rel_wr_sec_c = 0;

			DMSG("RPMB size: %zu bytes (mult: 0x%llx), rel_frames: %u\n",
				rpmb_cached_size, mult, rpmb_rel_wr_sec_c);

			closedir(dir);
			return 0;
		}
	}

	closedir(dir);
	return -ENODEV;
}

static int try_open_rpmb(const char *path)
{
	struct stat sb;
	int fd = -1;

	if (lstat(path, &sb) < 0 || (!S_ISCHR(sb.st_mode) && !S_ISBLK(sb.st_mode)))
		return -1;

#ifdef O_NOFOLLOW
	fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
#else
	fd = open(path, O_RDWR | O_CLOEXEC);
#endif

	if (fd >= 0) {
		supp_strlcpy(rpmb_dev_path, path, sizeof(rpmb_dev_path));
		DMSG("Opened RPMB device: %s\n", path);
		rpmb_probe_device(NULL);
	}

	return fd;
}

static int rpmb_open_dev(void)
{
	static const char *known_paths[] = {
		"/dev/mmcblk0rpmb", "/dev/mmcblk1rpmb",
		"/dev/mmcblk2rpmb", NULL
	};
	DIR *dir = NULL;
	struct dirent *d = NULL;
	char path[512];
	int i = 0, slen = 0;

	if (rpmb_fd >= 0)
		return 0;

	/* Try known paths first */
	for (i = 0; known_paths[i]; i++) {
		rpmb_fd = try_open_rpmb(known_paths[i]);
		if (rpmb_fd >= 0)
			return 0;
	}

	/* Scan /dev for mmcblk*rpmb */
	dir = opendir("/dev");
	if (!dir)
		return -ENODEV;

	while ((d = readdir(dir)) != NULL) {
		if (supp_strncmp(d->d_name, "mmcblk", 6) || !supp_strstr(d->d_name, "rpmb"))
			continue;

		slen = supp_strlcpy(path, "/dev/", sizeof(path));
		slen = supp_strlcat(path, d->d_name, sizeof(path));
		if (slen >= sizeof(path))
			continue;

		rpmb_fd = try_open_rpmb(path);
		if (rpmb_fd >= 0) {
			closedir(dir);
			return 0;
		}
	}

	closedir(dir);
	EMSG("No RPMB device found\n");
	return -ENODEV;
}

static int rpmb_switch_partition(void)
{
	struct mmc_ioc_cmd ioc;
	int ret = -1;

	supp_memset(&ioc, 0, sizeof(ioc));

	/* Skip if already in RPMB partition */
	if (rpmb_in_rpmb_partition)
		return 0;

	/* CMD6 (SWITCH): Set EXT_CSD[179] (PART_CONFIG) to 3 (RPMB partition) */
	ioc.opcode = 6;  /* MMC_SWITCH */
	ioc.arg = (3 << 24) |  /* Access mode: Write byte */
	          (179 << 16) | /* EXT_CSD index: PART_CONFIG */
	          (3 << 8) |    /* Value: RPMB partition (0x3) */
	          (0 << 0);     /* Cmd set */
	ioc.flags = MMC_RSP_R1 | MMC_CMD_AC;
	ioc.write_flag = 0;

	ret = ioctl(rpmb_fd, MMC_IOC_CMD, &ioc);
	if (ret < 0) {
		ret = -errno;
		EMSG("RPMB: Failed to switch to RPMB partition: %s\n",
			strerror(-ret));
		return ret;
	}

	rpmb_in_rpmb_partition = true;

	DMSG("RPMB: Switched to RPMB partition (CMD6 response: 0x%08x)\n",
			ioc.response[0]);
	return 0;
}

static int rpmb_ioctl(struct rpmb_cmd *cmd)
{
	struct mmc_ioc_multi_cmd *multi = NULL;
	struct mmc_ioc_cmd *ioc = NULL;
	struct rpmb_frame *frames = cmd->frames;
	struct rpmb_frame status_frame;
	unsigned int req_type = -1, cmd_count = 0, idx = 0;
	unsigned int i = 0, transfer_blocks = 0;
	unsigned int request_blocks = 0, response_blocks = 0;
	size_t multi_size = 0, bc = 0;
	int ret = -1, attempt = 0;

	if (!cmd || !frames || cmd->nframes == 0 ||
	    cmd->nframes > RPMB_MAX_BLOCKS_PER_CMD) {
		EMSG("Invalid nframes: %u\n", cmd->nframes);
		return -EINVAL;
	}

	/* Ensure we're in RPMB partition */
	ret = rpmb_switch_partition();
	if (ret < 0)
		return ret;

	req_type = ntohs(frames[0].req_resp);

	/* Decide the exact MMC command sequence */
	switch (req_type) {
	case RPMB_REQ_WRITE:
	case RPMB_REQ_KEY:
		/* CMD23 + CMD25(request) + CMD25(status) + CMD18(response) */
		cmd_count = 4;
		break;
	case RPMB_REQ_READ:
	case RPMB_REQ_WCOUNTER:
		/* CMD23 + CMD25(request) + CMD18(response) */
		cmd_count = 3;
		break;
	default:
		EMSG("Unknown RPMB request: 0x%04x\n", req_type);
		return -EINVAL;
	}

	/*
	 * Validate frame block_count per RPMB spec:
	 * - KEY/WCOUNTER: block_count should be 0 (not used by these requests)
	 * - READ: block_count specifies blocks to read (>0, <=nframes)
	 * - WRITE: block_count must equal nframes being written
	 */
	bc = ntohs(frames[0].block_count);

	if (bc > RPMB_MAX_BLOCKS_PER_CMD)
		return -EINVAL;

	/* Validate request shape and keep semantics simple */
	switch (req_type) {
	case RPMB_REQ_WRITE:
		if (bc != cmd->nframes)
			return -EINVAL;
		for (i = 1; i < cmd->nframes; i++) {
			if (ntohs(frames[i].block_count) != bc)
				return -EINVAL;
		}
		break;
	case RPMB_REQ_READ:
		if (bc == 0 || bc != cmd->nframes)
			return -EINVAL;
		break;
	case RPMB_REQ_KEY:
	case RPMB_REQ_WCOUNTER:
		if (cmd->nframes != 1 || bc != 0)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	transfer_blocks = cmd->nframes;
	request_blocks = (req_type == RPMB_REQ_WRITE) ? cmd->nframes : 1;
	response_blocks = (req_type == RPMB_REQ_READ) ? cmd->nframes : 1;

	multi_size = sizeof(*multi) + cmd_count * sizeof(struct mmc_ioc_cmd);
	multi = malloc(multi_size);
	if (!multi)
		return -ENOMEM;
	supp_memset(multi, 0, multi_size);

	multi->num_of_cmds = cmd_count;
	ioc = multi->cmds;

	/* CMD23: SET_BLOCK_COUNT (always sent for RPMB operations) */
	ioc[idx].opcode = 23;
	ioc[idx].arg = transfer_blocks;
	if ((req_type == RPMB_REQ_WRITE || req_type == RPMB_REQ_KEY) &&
	    rpmb_rel_wr_sec_c > 0 && transfer_blocks <= (unsigned int)rpmb_rel_wr_sec_c)
		ioc[idx].arg |= (1u << 31);  /* Reliable write bit */
	ioc[idx].flags = MMC_RSP_R1 | MMC_CMD_AC;

	DMSG("RPMB: CMD23(SET_BLOCK_COUNT): arg=0x%08x, req_type=0x%04x\n",
		ioc[idx].arg, req_type);
	idx++;

	/* CMD25: Write Request */
	ioc[idx].opcode = 25;
	ioc[idx].flags = MMC_RSP_R1 | MMC_CMD_ADTC;
	ioc[idx].write_flag = 1;
	ioc[idx].blksz = MMC_BLK_SIZE;
	ioc[idx].blocks = request_blocks;
	ioc[idx].data_ptr = (__u64)(uintptr_t)frames;
	if (req_type == RPMB_REQ_WRITE || req_type == RPMB_REQ_KEY) {
		ioc[idx].postsleep_min_us = 20000;
		ioc[idx].postsleep_max_us = 50000;
	}
	idx++;

	/* CMD25: Status Request (for Write/Key) */
	if (req_type == RPMB_REQ_WRITE || req_type == RPMB_REQ_KEY) {
		supp_memset(&status_frame, 0, sizeof(status_frame));
		status_frame.req_resp = htons(RPMB_REQ_STATUS);
		ioc[idx].opcode = 25;
		ioc[idx].flags = MMC_RSP_R1 | MMC_CMD_ADTC;
		ioc[idx].write_flag = 1;
		ioc[idx].blksz = MMC_BLK_SIZE;
		ioc[idx].blocks = 1;
		ioc[idx].data_ptr = (__u64)(uintptr_t)&status_frame;
		idx++;
	}

	/* CMD18: Read Response */
	ioc[idx].opcode = 18;
	ioc[idx].flags = MMC_RSP_R1 | MMC_CMD_ADTC;
	ioc[idx].write_flag = 0;
	ioc[idx].blksz = MMC_BLK_SIZE;
	ioc[idx].blocks = response_blocks;
	ioc[idx].data_ptr = (__u64)(uintptr_t)frames;

	DMSG("RPMB: Executing multi_cmd, cmd_count=%u, req_type=0x%04x\n", cmd_count, req_type);

	DMSG("RPMB: CMD18(read): opcode=%u, write_flag=%u, blksz=%u, blocks=%u\n",
	    ioc[idx].opcode, ioc[idx].write_flag, ioc[idx].blksz, ioc[idx].blocks);

	if (idx + 1 != cmd_count) {
		free(multi);
		return -EINVAL;
	}

	/* Execute with retry on transient errors (e.g. MMC timeout) */
	for (attempt = 0; attempt < RPMB_IOCTL_MAX_RETRIES; attempt++) {
		ret = ioctl(rpmb_fd, MMC_IOC_MULTI_CMD, multi);
		if (ret == 0)
			break;

		ret = -errno;

		/* Only retry on transient errors: timeout or I/O error */
		if (ret != -ETIMEDOUT && ret != -EIO)
			break;

		DMSG("RPMB: ioctl attempt %d/%d failed: %s, retrying...\n",
			attempt + 1, RPMB_IOCTL_MAX_RETRIES, strerror(-ret));

		/* Allow hardware to recover before retry */
		usleep(RPMB_IOCTL_RETRY_DELAY_US);
	}

	DMSG("RPMB: ioctl result %d (attempt %d/%d)\n",
		ret, attempt + 1, RPMB_IOCTL_MAX_RETRIES);

	free(multi);

	if (ret < 0) {
		EMSG("RPMB ioctl failed after %d attempts: %s\n",
			RPMB_IOCTL_MAX_RETRIES, strerror(-ret));
		close(rpmb_fd);
		rpmb_fd = -1;
		rpmb_cached_size = 0;
		rpmb_rel_wr_sec_c = 0;
		rpmb_in_rpmb_partition = false;  /* Reset partition state on error */
		supp_memset(rpmb_dev_path, 0, sizeof(rpmb_dev_path));
	}

	return ret;
}

static int rpmb_get_dev_info(struct rpmb_dev_info *info)
{
	size_t size = 0;
	int ret = -1;

	if (!info)
		return -EINVAL;

	supp_memset(info, 0, sizeof(*info));

	ret = rpmb_probe_device(&size);
	if (ret < 0 || size == 0)
		return (ret < 0) ? ret : -ENODEV;

	/* Fill in device information */
	info->total_blocks = size / RPMB_BLOCK_SIZE;
	info->rpmb_size_mult = size / RPMB_UNIT_SIZE;

	/*
	 * rel_wr_sec_c: Reliable Write Sector Count from eMMC ext_csd[222]
	 * Already probed in rpmb_probe_device() and stored in rpmb_rel_wr_sec_c
	 * This indicates how many 256-byte frames can be written atomically
	 */
	info->rel_wr_sec_c = rpmb_rel_wr_sec_c;

	/*
	 * Max blocks per command - hardware/driver dependent
	 * Conservative defaults for compatibility
	 */
	info->max_wr_blkcnt = 32;  /* Typical eMMC limit */
	info->max_rd_blkcnt = 32;

	info->flags = 0;

	DMSG("RPMB dev_info: blocks=%u, rel_wr_sec_c=%u, size_mult=%u\n",
		info->total_blocks, info->rel_wr_sec_c, info->rpmb_size_mult);

	return 0;
}

int rpmb_routine(struct rpmb_cmd *cmd)
{
	int ret = -EINVAL;

	if (!cmd)
		return ret;

	pthread_mutex_lock(&rpmb_mutex);

	ret = rpmb_open_dev();
	if (ret < 0)
		goto out;

	switch (cmd->hdr.op) {
	case RPMB_EXEC:
		ret = rpmb_ioctl(cmd);
		break;
	case RPMB_GET_DEV_INFO:
		ret = rpmb_get_dev_info((struct rpmb_dev_info *)cmd->frames);
		break;
	default:
		ret = -ENOTSUP;
	}

out:
	cmd->hdr.ret = mbedtee_supp_errno_to_gp(ret);
	pthread_mutex_unlock(&rpmb_mutex);
	return ret;
}
