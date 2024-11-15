/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2019, KapaXL Limited
 * FS supplicant for mbedtee, e.g. REEFS
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "supp.h"

#define SUPP_MEMREF_SIZE (32ul * 1024)

union tee_iocl_supp {
	struct tee_iocl_supp_recv_arg r;
	struct tee_iocl_supp_send_arg s;
	unsigned char param_max_size[TEE_MAX_ARG_SIZE];
};

static int recv_request(int fd, union tee_iocl_supp *supp)
{
	struct tee_ioctl_buf_data data;

	memset(&data, 0, sizeof(data));

	data.buf_ptr = (uintptr_t)supp;
	data.buf_len = sizeof(struct tee_iocl_supp_recv_arg) +
			sizeof(struct tee_ioctl_param) * supp->r.num_params;

	if (ioctl(fd, TEE_IOC_SUPPL_RECV, &data)) {
		printf("recv_request: %s\n", strerror(errno));
		return -errno;
	}

	return 0;
}

static int send_response(int fd, union tee_iocl_supp *supp)
{
	struct tee_ioctl_buf_data data;

	memset(&data, 0, sizeof(data));

	data.buf_ptr = (uintptr_t)&supp->s;
	data.buf_len = sizeof(struct tee_iocl_supp_send_arg) +
	       sizeof(struct tee_ioctl_param) * supp->s.num_params;

	if (ioctl(fd, TEE_IOC_SUPPL_SEND, &data)) {
		printf("send_response: %s\n", strerror(errno));
		return -errno;
	}

	return 0;
}

static void *alloc_shm(int fd, int *id)
{
	int shm_fd = -1;
	void *buffer = NULL;
	struct tee_ioctl_shm_alloc_data data;

	memset(&data, 0, sizeof(data));

	data.size = SUPP_MEMREF_SIZE;
	shm_fd = ioctl(fd, TEE_IOC_SHM_ALLOC, &data);
	if (shm_fd < 0) {
		printf("TEE_IOC_SHM_ALLOC: %s\n", strerror(errno));
		return NULL;
	}

	buffer = mmap(NULL, SUPP_MEMREF_SIZE,
				PROT_READ | PROT_WRITE,
				MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (buffer == MAP_FAILED) {
		printf("mmap: %s\n", strerror(errno));
		return NULL;
	}

	*id = data.id;

	return buffer;
}

static void free_shm(void *buffer)
{
	munmap(buffer, SUPP_MEMREF_SIZE);
}

static int process_request(int fd, int shm_id, void *shm)
{
	int ret = -1;
	union tee_iocl_supp supp;

	memset(&supp, 0, sizeof(supp));

	/* 1 param, type is shm */
	supp.r.num_params = 1;
	supp.r.params->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

	/* offset, size and id */
	supp.r.params->a = 0;
	supp.r.params->b = SUPP_MEMREF_SIZE;
	supp.r.params->c = shm_id;

	ret = recv_request(fd, &supp);
	if (ret != 0) {
		printf("recv_request: ret %d %s\n", ret, strerror(errno));
		return ret;
	}

	switch (supp.r.func) {
	case SUPP_REEFS:
		ret = reefs_routine(shm);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	supp.s.ret = ret;
	send_response(fd, &supp);

	return 0;
}

int main(int argc, char *argv[])
{
	int fd = -1;
	int ret = -1, shm_id = -1;
	void *shm = NULL;
	struct tee_ioctl_version_data vers;
	struct sched_param p = {.sched_priority = 0};

restart:
	fd = open("/dev/tee0", O_RDWR);
	if (fd < 0) {
		printf("open(tee0): %s\n", strerror(errno));
		return fd;
	}

	ret = ioctl(fd, TEE_IOC_VERSION, &vers);
	if (ret != 0)
		goto out;

	if (vers.impl_id != TEE_IMPL_ID_MBEDTEE) {
		printf("impl_id: %d\n", vers.impl_id);
		goto out;
	}

	p.sched_priority = sched_get_priority_max(SCHED_FIFO);
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);

	if ((ret = daemon(0, 0)) < 0) {
		printf("daemon(): %s\n", strerror(errno));
		goto out;
	}

	shm = alloc_shm(fd, &shm_id);
	if (shm == NULL)
		goto out;

	for (;;) {
		ret = process_request(fd, shm_id, shm);
		if (ret < 0) {
			free_shm(shm);
			close(fd);
			goto restart;
		}
	}

out:
	close(fd);
	return -1;
}
