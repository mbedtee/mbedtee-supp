/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2019 Xing Loong <xing.xl.loong@gmail.com>
 *
 * MbedTEE supplicant protocol definitions.
 *
 * These definitions must match the definitions on the TEE side.
 * (Canonical source: mbedtee-os/include/rpc/)
 */

#ifndef _MBEDTEE_MSG_SUPPLICANT_H
#define _MBEDTEE_MSG_SUPPLICANT_H

#include <errno.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Supplicant types (TEE->REE RPC function IDs)
 */
#define SUPP_REEFS		1
#define SUPP_RPMB		2
#define SUPP_MAX		10

#ifndef TEEC_SUCCESS
#define TEEC_SUCCESS					0x00000000
#define TEEC_ERROR_GENERIC				0xFFFF0000
#define TEEC_ERROR_ACCESS_DENIED		0xFFFF0001
#define TEEC_ERROR_CANCEL				0xFFFF0002
#define TEEC_ERROR_ACCESS_CONFLICT		0xFFFF0003
#define TEEC_ERROR_EXCESS_DATA			0xFFFF0004
#define TEEC_ERROR_BAD_FORMAT			0xFFFF0005
#define TEEC_ERROR_BAD_PARAMETERS		0xFFFF0006
#define TEEC_ERROR_ITEM_NOT_FOUND		0xFFFF0008
#define TEEC_ERROR_NOT_IMPLEMENTED		0xFFFF0009
#define TEEC_ERROR_NOT_SUPPORTED		0xFFFF000A
#define TEEC_ERROR_NO_DATA				0xFFFF000B
#define TEEC_ERROR_OUT_OF_MEMORY		0xFFFF000C
#define TEEC_ERROR_BUSY					0xFFFF000D
#define TEEC_ERROR_COMMUNICATION		0xFFFF000E
#define TEEC_ERROR_SHORT_BUFFER			0xFFFF0010
#define TEEC_ERROR_TARGET_DEAD			0xFFFF3024
#define TEEC_ERROR_STORAGE_NO_SPACE		0xFFFF3041
#endif

/*
 * Payload-bearing supplicant RPCs store either a non-negative success value
 * (for example a file descriptor or byte count) or a GP result in cmd->ret.
 * Convert only local errno failures and leave successful payload values as-is.
 */
static inline int mbedtee_supp_errno_to_gp(int ret)
{
	if (ret >= 0 || ret < -4095)
		return ret;

	switch (ret) {
	case -EPERM:
	case -EACCES:
	case -EISDIR:
		return TEEC_ERROR_ACCESS_DENIED;
	case -ENOENT:
		return TEEC_ERROR_ITEM_NOT_FOUND;
	case -EINTR:
	case -ECANCELED:
		return TEEC_ERROR_CANCEL;
	case -EEXIST:
		return TEEC_ERROR_ACCESS_CONFLICT;
	case -E2BIG:
		return TEEC_ERROR_EXCESS_DATA;
	case -EBADMSG:
	case -EPROTO:
	case -ENAMETOOLONG:
		return TEEC_ERROR_BAD_FORMAT;
	case -ENOMEM:
	case -EMFILE:
	case -ENFILE:
		return TEEC_ERROR_OUT_OF_MEMORY;
	case -EFAULT:
	case -EINVAL:
	case -ENOEXEC:
		return TEEC_ERROR_BAD_PARAMETERS;
	case -ENODATA:
		return TEEC_ERROR_NO_DATA;
	case -EAGAIN:
	case -EBUSY:
		return TEEC_ERROR_BUSY;
	case -EMSGSIZE:
	#ifdef ENOBUFS
	case -ENOBUFS:
	#endif
		return TEEC_ERROR_SHORT_BUFFER;
	case -ESRCH:
		return TEEC_ERROR_TARGET_DEAD;
	case -ENOSPC:
	case -EDQUOT:
		return TEEC_ERROR_STORAGE_NO_SPACE;
	case -EOPNOTSUPP:
#ifdef ENOTSUP
	#if ENOTSUP != EOPNOTSUPP
	case -ENOTSUP:
	#endif
#endif
		return TEEC_ERROR_NOT_SUPPORTED;
	case -EIO:
	#ifdef ECOMM
	case -ECOMM:
	#endif
	#ifdef EREMOTEIO
	case -EREMOTEIO:
	#endif
		return TEEC_ERROR_COMMUNICATION;
	default:
		return TEEC_ERROR_GENERIC;
	}
}

/*
 * REEFS functions
 */
#define REEFS_OPEN       1
#define REEFS_CLOSE      2
#define REEFS_READ       3
#define REEFS_WRITE      4
#define REEFS_SEEK       5
#define REEFS_UNLINK     6
#define REEFS_RENAME     7
#define REEFS_TRUNC      8
#define REEFS_MKDIR      9
#define REEFS_OPENDIR   10
#define REEFS_CLOSEDIR  11
#define REEFS_READDIR   12
#define REEFS_SEEKDIR   13
#define REEFS_RMDIR     14
#define REEFS_FSTAT     15
#define REEFS_PREAD     16
#define REEFS_PWRITE    17

/*
 * REEFS open flags (Fixed values for RPC)
 */
#define REEFS_O_RDONLY      00000000
#define REEFS_O_WRONLY      00000001
#define REEFS_O_RDWR        00000002
#define REEFS_O_CREAT       00000100
#define REEFS_O_EXCL        00000200
#define REEFS_O_TRUNC       00001000
#define REEFS_O_APPEND      00002000
#define REEFS_O_DIRECTORY   00200000

/*
 * REEFS seek flags (Fixed values for RPC)
 */
#define REEFS_SEEK_SET      0
#define REEFS_SEEK_CUR      1
#define REEFS_SEEK_END      2

struct supp_cmd_hdr {
	int ret;
	int op;
};

struct reefs_cmd {
	struct supp_cmd_hdr hdr;

	int flags;
	int fd;

	uint64_t len;

	char data[];
};

struct reefs_dirent {
	uint64_t	d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char		d_name[];
};

struct reefs_stat {
	uint64_t rst_size;
	uint64_t rst_atime;
	uint64_t rst_mtime;
	uint64_t rst_ctime;
};

/*
 * RPMB functions
 */
#define RPMB_EXEC         1
#define RPMB_GET_DEV_INFO 2

/* RPMB Request/Response Types */
#define RPMB_REQ_KEY		0x0001
#define RPMB_REQ_WCOUNTER	0x0002
#define RPMB_REQ_WRITE		0x0003
#define RPMB_REQ_READ		0x0004
#define RPMB_REQ_STATUS		0x0005
#define RPMB_RESP_KEY		0x0100
#define RPMB_RESP_WCOUNTER	0x0200
#define RPMB_RESP_WRITE		0x0300
#define RPMB_RESP_READ		0x0400

/*
 * RPMB Frame Definition (512 bytes)
 */
struct rpmb_frame {
	unsigned char stuff[196];
	unsigned char key_mac[32];
	unsigned char data[256];
	unsigned char nonce[16];
	unsigned int write_counter;
	unsigned short addr;
	unsigned short block_count;
	unsigned short result;
	unsigned short req_resp;
} __attribute__((packed));

/*
 * RPMB Device Information
 */
struct rpmb_dev_info {
	unsigned int total_blocks;
	unsigned int rel_wr_sec_c;
	unsigned int rpmb_size_mult;
	unsigned int max_wr_blkcnt;
	unsigned int max_rd_blkcnt;
	unsigned char flags;
	unsigned char reserved[3];
};

/*
 * RPMB Command Structure
 */
struct rpmb_cmd {
	struct supp_cmd_hdr hdr;
	unsigned int nframes;

	struct rpmb_frame frames[];
};

#ifdef __cplusplus
}
#endif

#endif
