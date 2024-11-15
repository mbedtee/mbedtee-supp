/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2019, KapaXL Limited
 * FS supplicant for mbedtee-reefs
 */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

#include "supp.h"

//#define MSG printf
#define MSG(...) do {} while(0)

#define REEFS_PATH_MAX (1024)

#define REEFS_PATH "/data/mbedtee/reefs"

static int reefs_mkdirs(const char *path, mode_t mode)
{
	int i, len = strlen(path), ret;
	char dir[REEFS_PATH_MAX];

	strcpy(dir, path);

	for (i = 0; i <= len; i++) {
		if ((dir[i] == '/' && i > 0) || (i == len)) {
			dir[i] = 0;
			if (access(dir, R_OK) < 0) {
				MSG("mkdirs %s\n", dir);

				ret = mkdir(dir, mode);
				if (ret < 0) {
					ret = -errno;
					MSG("mkdir %s failed\n", dir);
					return ret;
				}
			}
			dir[i] = '/';
		}
	}

	return 0;
}

static void reefs_path_prefix(char *out, const char *in)
{
	snprintf(out, REEFS_PATH_MAX, "%s%s", REEFS_PATH, in);
}

static inline int reefs_isroot(const char *path)
{
	if ((strlen(path) == 0) ||
		(strlen(path) == 1 && path[0] == '/'))
		return true;

	return false;
}

static int reefs_open(struct reefs_cmd *r)
{
	int ret = -1, l = 0, i = 0;
	char path[REEFS_PATH_MAX];
	char dir[REEFS_PATH_MAX];

	reefs_path_prefix(path, (const char *)r->data);

	MSG("opening file %s flags 0x%x\n", path, r->flags);

	if (r->flags & O_CREAT) {
		/* make parent directory */
		strcpy(dir, path);
		l = strlen(dir);
		for (i = l - 1; i >= 0; i--) {
			if (dir[i] == '/') {
				dir[i] = 0;
				if (i != l -1)
					break;
			}
		}
		ret = reefs_mkdirs(dir, 0700);
		if (ret != 0)
			return ret;
	}

	ret = open(path, r->flags, 0600);
	if (ret < 0) {
		ret = -errno;
		MSG("open %s, errno %d\n", path, errno);
	}

	return ret;
}

static int reefs_close(struct reefs_cmd *r)
{
	int ret = close(r->fd);

	return ret ? -errno : ret;
}

static ssize_t reefs_read(struct reefs_cmd *r)
{
	void *data = r->data;
	off_t rb = 0, rc = -1, offset = 0;

	while (offset < r->len) {
		rb = r->len - offset;
		rc = read(r->fd, data + offset, rb);

		if (rc < 0) {
			rc = -errno;
			MSG("read fail, errno %d\n", errno);
			break;
		} else if (rc == 0) {
			break;
		} else {
			offset += rc;
		}
	}

	return (rc < 0) ? rc : offset;
}

static ssize_t reefs_write(struct reefs_cmd *r)
{
	void *data = r->data;
	off_t wb = 0, rc = -1, offset = 0;

	while (offset < r->len) {
		wb = r->len - offset;
		rc = write(r->fd, data + offset, wb);

		if (rc < 0) {
			rc = -errno;
			MSG("write fail, errno %d\n", errno);
			break;
		} else if (rc == 0) {
			break;
		} else {
			offset += rc;
		}
	}

	return (rc < 0) ? rc : offset;
}

static int reefs_truncate(struct reefs_cmd *r)
{
	int ret = ftruncate(r->fd, r->len);

	return ret ? -errno : ret;
}

static int reefs_unlink(struct reefs_cmd *r)
{
	int ret = -1;

	char path[REEFS_PATH_MAX];

	reefs_path_prefix(path, (const char *)r->data);

	MSG("unlink file %s\n", path);

	ret = unlink(path);

	return ret ? -errno : ret;
}

static int reefs_rename(struct reefs_cmd *r)
{
	int ret = -1;
	char oldpath[REEFS_PATH_MAX];
	char newpath[REEFS_PATH_MAX];
	size_t offset = strnlen((const char *)r->data, REEFS_PATH_MAX) + 1;

	reefs_path_prefix(oldpath, (const char *)r->data);
	reefs_path_prefix(newpath, (const char *)r->data + offset);

	MSG("rename %s -> %s\n", oldpath, newpath);

	if (reefs_isroot(r->data))
		return -EBUSY;

	if (access(newpath, R_OK) == 0)
		return -EEXIST;

	ret = rename(oldpath, newpath);

	return ret ? -errno : ret;
}

static int reefs_mkdir(struct reefs_cmd *r)
{
	char path[REEFS_PATH_MAX];

	reefs_path_prefix(path, (const char *)r->data);

	MSG("mkdir %s\n", path);

	return reefs_mkdirs(path, r->flags);
}

static int reefs_opendir(struct reefs_cmd *r)
{
	int ret = -1;
	DIR *d = NULL;
	pthread_key_t k = 0;
	char path[REEFS_PATH_MAX];

	reefs_path_prefix(path, (const char *)r->data);

	MSG("opendir %s\n", path);

	ret = pthread_key_create(&k, NULL);
	if (ret != 0)
		return -ENOMEM;

	d = opendir(path);
	if (d == NULL) {
		ret = -errno;
		MSG("opendir %s, errno %d\n", path, errno);
		pthread_key_delete(k);
		return ret;
	}

	pthread_setspecific(k, d);

	return k;
}

static int reefs_closedir(struct reefs_cmd *r)
{
	int ret = -1;
	pthread_key_t k = r->fd;
	DIR *d = pthread_getspecific(k);

	if (d == NULL)
		return -EINVAL;

	pthread_key_delete(k);

	ret = closedir(d);

	return ret ? -errno : ret;
}

static int reefs_readdir(struct reefs_cmd *r)
{
	DIR *dir = pthread_getspecific(r->fd);
	struct reefs_dirent *d = (struct reefs_dirent *)r->data;
	off_t str_len = 0, retlen = 0, lastdoff = telldir(dir);
	off_t reclen = -1, structsz = 0, cnt = r->len;
	struct dirent *e = NULL;

	if (d == NULL || dir == NULL)
		return -EINVAL;

	structsz = sizeof(d->d_reclen) + sizeof(d->d_off) + sizeof(d->d_type);

	while (cnt && ((e = readdir(dir)) != NULL)) {
		if ((e->d_name[0] == '.') && \
			(e->d_name[1] == 0 || e->d_name[1] == '.'))
			continue;

		str_len = strlen(e->d_name) + 1;
		reclen = str_len + structsz;
		reclen = ((reclen + sizeof(long) - 1) / sizeof(long)) * sizeof(long);

		if (reclen > cnt) {
			seekdir(dir, lastdoff);
			if (retlen == 0)
				return -E2BIG;
			return retlen;
		}

		lastdoff = telldir(dir);

		d->d_off = e->d_off;
		d->d_type = e->d_type;
		d->d_reclen = reclen;
		memcpy(d->d_name, e->d_name, str_len);

		retlen += reclen;
		cnt -= reclen;
		d = (void *)d + reclen;
	}

	/* got nothing */
	if (retlen == 0)
		retlen = EOF;

	return retlen;
}

static int reefs_seekdir(struct reefs_cmd *r)
{
	DIR *dir = pthread_getspecific(r->fd);

	if (dir == NULL)
		return -EINVAL;

	seekdir(dir, r->len);

	return -errno;
}

static int reefs_rmdir(struct reefs_cmd *r)
{
	int ret = -1;
	char path[REEFS_PATH_MAX];

	reefs_path_prefix(path, (const char *)r->data);

	MSG("rmdir %s\n", path);

	if (reefs_isroot(r->data))
		return -EBUSY;

	ret = rmdir(path);

	return ret ? -errno : ret;
}

static off_t reefs_lseek(struct reefs_cmd *r)
{
	off_t ret = lseek(r->fd, r->len, r->flags);

	if (ret < 0)
		ret = -errno;

	return ret;
}

int reefs_routine(void *data)
{
	int ret = -1;
	struct reefs_cmd *cmd = (struct reefs_cmd *)data;

	if (cmd == NULL)
		return -EINVAL;

	switch (cmd->op) {
	case REEFS_OPEN:
		ret = reefs_open(cmd);
		break;
	case REEFS_CLOSE:
		ret = reefs_close(cmd);
		break;
	case REEFS_READ:
		ret = reefs_read(cmd);
		break;
	case REEFS_WRITE:
		ret = reefs_write(cmd);
		break;
	case REEFS_UNLINK:
		ret = reefs_unlink(cmd);
		break;
	case REEFS_RENAME:
		ret = reefs_rename(cmd);
		break;
	case REEFS_TRUNC:
		ret = reefs_truncate(cmd);
		break;
	case REEFS_MKDIR:
		ret = reefs_mkdir(cmd);
		break;
	case REEFS_OPENDIR:
		ret = reefs_opendir(cmd);
		break;
	case REEFS_CLOSEDIR:
		ret = reefs_closedir(cmd);
		break;
	case REEFS_READDIR:
		ret = reefs_readdir(cmd);
		break;
	case REEFS_SEEKDIR:
		ret = reefs_seekdir(cmd);
		break;
	case REEFS_RMDIR:
		ret = reefs_rmdir(cmd);
		break;
	case REEFS_SEEK:
		ret = reefs_lseek(cmd);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	cmd->ret = ret;
	return ret;
}
