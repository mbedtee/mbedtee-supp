// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2019 Xing Loong <xing.xl.loong@gmail.com>
 * FS supplicant for mbedtee, e.g. REEFS
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "supp.h"

#define SUPP_MEMREF_SIZE (32ul * 1024)

#define NR_WORKERS 2

/*
 * Per-thread alternate signal stack size.  Use a fixed value rather than
 * the SIGSTKSZ macro, which is not a compile-time constant on glibc >= 2.34.
 * 8 KB is enough for any reasonable signal handler.
 */
#define SUPP_SIGSTK_SIZE 8192

static volatile sig_atomic_t g_running = 1;
int g_crash_fd = -1;	/* crash log / instance lock fd */

static void sig_msg(int sig)
{
	static const char prefix[] = "mbedtee-supp sig=";
	char buf[32];
	int i, p, s = sig;

	if (g_crash_fd < 0)
		return;

	/* build "prefix + decimal + \n" in one buffer, single write */
	i = sizeof(buf);
	buf[--i] = '\n';
	if (s == 0) {
		buf[--i] = '0';
	} else {
		while (s > 0) {
			buf[--i] = '0' + (s % 10);
			s /= 10;
		}
	}
	for (p = sizeof(prefix) - 1; p >= 0; p--)
		buf[--i] = prefix[p];

	write(g_crash_fd, buf + i, sizeof(buf) - i);
}

static void sig_handler(int sig)
{
	/* Async-signal-safe: only touch volatile sig_atomic_t */
	g_running = 0;
	sig_msg(sig);
}

static void crash_handler(int sig)
{
	sig_msg(sig);
	signal(sig, SIG_DFL);
	raise(sig);
}

/*
 * Install signal handlers with a dedicated alternate stack so that a
 * signal never executes on a worker thread's normal stack.  The main
 * thread's altstack is allocated here; each worker sets up its own
 * altstack at the top of supp_worker_thread().
 */
static void setup_signals(void)
{
	static unsigned char sigstack_buf[SUPP_SIGSTK_SIZE];
	stack_t sigstk;
	struct sigaction sa;

	supp_memset(&sigstk, 0, sizeof(sigstk));
	sigstk.ss_sp = sigstack_buf;
	sigstk.ss_size = sizeof(sigstack_buf);
	if (sigaltstack(&sigstk, NULL) != 0)
		EMSG("sigaltstack: %s\n", strerror(errno));

	supp_memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = sig_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = crash_handler;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigaction(SIGPIPE, &sa, NULL);
}

struct worker_ctx {
	int fd;
	int shm_id;
	void *shm;
	/* per-thread alternate signal stack */
	unsigned char sigstack_buf[SUPP_SIGSTK_SIZE];
};

struct tee_supp_recv {
	struct tee_iocl_supp_recv_arg r;
	struct tee_ioctl_param params[1];
};

struct tee_supp_send {
	struct tee_iocl_supp_send_arg s;
	struct tee_ioctl_param params[1];
};

static int recv_request(int fd, struct tee_supp_recv *recv)
{
	int ret;
	struct tee_ioctl_buf_data data;

	data.buf_ptr = (uintptr_t)recv;
	data.buf_len = sizeof(struct tee_supp_recv);

	ret = ioctl(fd, TEE_IOC_SUPPL_RECV, &data);
	if (ret) {
		ret = errno;
		EMSG("TEE_IOC_SUPPL_RECV FAIL %d\n", ret);
		return -ret;
	}

	return 0;
}

static int send_response(int fd, struct tee_supp_send *res)
{
	int ret;
	struct tee_ioctl_buf_data data;

	data.buf_ptr = (uintptr_t)res;
	data.buf_len = sizeof(struct tee_supp_send);

	ret = ioctl(fd, TEE_IOC_SUPPL_SEND, &data);
	if (ret) {
		ret = errno;
		EMSG("TEE_IOC_SUPPL_SEND FAIL %d\n", ret);
		return -ret;
	}

	return 0;
}

static void *alloc_shm(int fd, int *id)
{
	int shm_fd = -1;
	void *buffer = NULL;
	struct tee_ioctl_shm_alloc_data data;

	supp_memset(&data, 0, sizeof(data));
	data.size = SUPP_MEMREF_SIZE;
	shm_fd = ioctl(fd, TEE_IOC_SHM_ALLOC, &data);
	if (shm_fd < 0) {
		EMSG("TEE_IOC_SHM_ALLOC: %s\n", strerror(errno));
		return NULL;
	}

	buffer = mmap(NULL, SUPP_MEMREF_SIZE,
				PROT_READ | PROT_WRITE,
				MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (buffer == MAP_FAILED) {
		EMSG("mmap: %s\n", strerror(errno));
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
	struct tee_supp_recv recv;
	struct tee_supp_send res;
	struct supp_cmd_hdr *hdr = shm;

	supp_memset(&recv, 0, sizeof(recv));
	supp_memset(&res, 0, sizeof(res));

	/* 1 param, type is shm */
	recv.r.num_params = 1;
	recv.r.params->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

	/* offset, size and id */
	recv.r.params->a = 0;
	recv.r.params->b = SUPP_MEMREF_SIZE;
	recv.r.params->c = shm_id;

	ret = recv_request(fd, &recv);
	if (ret == -EINTR)
		return 0;
	if (ret == -ENOMEM || ret == -EAGAIN) {
		/*
		 * Transient resource shortage -- back off briefly and
		 * let the worker loop retry instead of killing the thread.
		 */
		usleep(100000);
		return 0;
	}
	if (ret != 0)
		return ret;

	switch (recv.r.func) {
	case SUPP_REEFS:
		reefs_routine(shm);
		break;
	case SUPP_RPMB:
		rpmb_routine(shm);
		break;
	default:
		hdr->ret = mbedtee_supp_errno_to_gp(-ENOTSUP);
		break;
	}

	/*
	 * The transport layer succeeded: the actual FS/RPMB result is in
	 * cmd->ret inside the SHM. Always reply TEEC_SUCCESS here so the
	 * kernel does not overwrite the SHM result with a transport error.
	 */
	res.s.ret = TEEC_SUCCESS;
	res.s.num_params = 1;
	res.s.params->attr = recv.r.params->attr;
	res.s.params->b = recv.r.params->b;

	return send_response(fd, &res);
}

static void *supp_worker_thread(void *arg)
{
	int ret = 0;
	struct worker_ctx *ctx = arg;
	struct sched_param p = {.sched_priority = 0};
	stack_t sigstk;

	/*
	 * Each thread needs its own alternate signal stack, otherwise a
	 * signal delivered to this thread would run on the thread's normal
	 * stack and risk corrupting local variables in flight.
	 */
	supp_memset(&sigstk, 0, sizeof(sigstk));
	sigstk.ss_sp = ctx->sigstack_buf;
	sigstk.ss_size = sizeof(ctx->sigstack_buf);
	sigaltstack(&sigstk, NULL);

	p.sched_priority = sched_get_priority_max(SCHED_RR);
	pthread_setschedparam(pthread_self(), SCHED_RR, &p);

	while (g_running) {
		ret = process_request(ctx->fd, ctx->shm_id, ctx->shm);
		if (ret != 0)
			break;
	}

	EMSG("supp_worker_thread FAIL %d\n", ret);
	return NULL;
}

static int instance_lock(bool is_lock)
{
	int fd = g_crash_fd;

	if (!is_lock) {
		flock(fd, LOCK_UN);
		close(fd);
		g_crash_fd = -1;
		return 0;
	}

	fd = open("/var/tmp/mbedtee-supp", O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0)
		return -1;

	g_crash_fd = fd;
	return flock(fd, LOCK_EX | LOCK_NB);
}

int main(int argc, char *argv[])
{
	int fd = -1;
	int ret = -1, i = 0;
	struct tee_ioctl_version_data vers;
	pthread_t threads[NR_WORKERS];
	struct worker_ctx workers[NR_WORKERS];

	ret = instance_lock(true);
	if (ret < 0) {
		EMSG("lock supp: %s\n", strerror(errno));
		return ret;
	}

	/* Probe the TEE device once to verify it is mbedtee. */
	fd = open("/dev/tee0", O_RDWR);
	if (fd < 0) {
		EMSG("open(tee0): %s\n", strerror(errno));
		goto out;
	}

	ret = ioctl(fd, TEE_IOC_VERSION, &vers);
	if (ret != 0) {
		EMSG("TEE_IOC_VERSION: %s\n", strerror(errno));
		close(fd);
		goto out;
	}

	if (vers.impl_id != TEE_IMPL_ID_MBEDTEE) {
		EMSG("impl_id: %d\n", vers.impl_id);
		close(fd);
		goto out;
	}
	close(fd);

	ret = daemon(0, 0);
	if (ret < 0) {
		EMSG("daemon(): %s\n", strerror(errno));
		goto out;
	}

	setup_signals();

restart:
	supp_memset(workers, 0, sizeof(workers));
	supp_memset(threads, 0, sizeof(threads));
	g_running = 1;

	/*
	 * Each worker opens its own /dev/tee0 fd so that one worker's
	 * failure does not affect the other.  The kernel has been updated
	 * (see supp.c) to isolate requests by tee_context.
	 */
	for (i = 0; i < NR_WORKERS; i++) {
		workers[i].fd = open("/dev/tee0", O_RDWR);
		if (workers[i].fd < 0) {
			EMSG("worker[%d] open(tee0): %s\n",
			       i, strerror(errno));
			g_running = 0;
			goto cleanup;
		}
		workers[i].shm = alloc_shm(workers[i].fd,
					   &workers[i].shm_id);
		if (!workers[i].shm) {
			EMSG("worker[%d] alloc_shm failed\n", i);
			g_running = 0;
			goto cleanup;
		}
	}

	for (i = 0; i < NR_WORKERS; i++) {
		pthread_attr_t attr;

		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 64 * 1024);
		ret = pthread_create(&threads[i], &attr,
				supp_worker_thread, &workers[i]);
		pthread_attr_destroy(&attr);
		if (ret != 0) {
			EMSG("worker[%d] pthread_create: %s\n",
			       i, strerror(ret));
			g_running = 0;
			break;
		}
	}

	for (i = 0; i < NR_WORKERS; i++) {
		if (threads[i])
			pthread_join(threads[i], NULL);
	}

cleanup:
	for (i = 0; i < NR_WORKERS; i++) {
		if (workers[i].shm)
			free_shm(workers[i].shm);
		if (workers[i].fd >= 0)
			close(workers[i].fd);
	}

	/* Restart on transient errors, exit on signal */
	if (g_running) {
		DMSG("supp restarting\n");
		goto restart;
	}

out:
	instance_lock(false);
	return 0;
}
