// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2019 Xing Loong <xing.xl.loong@gmail.com>
 * FS supplicant for mbedtee-reefs
 */
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

#include "supp.h"

#define REEFS_PATH_MAX (1024)

#define REEFS_PATH "/var/local/mbedtee/"

#define REEFS_MAX_DIRS 128
static DIR *reefs_dirs[REEFS_MAX_DIRS];
static pthread_mutex_t reefs_dirs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reefs_meta_lock = PTHREAD_MUTEX_INITIALIZER;

static int reefs_alloc_dir(DIR *d)
{
	int i = 0;
	pthread_mutex_lock(&reefs_dirs_lock);
	for (i = 0; i < REEFS_MAX_DIRS; i++) {
		if (!reefs_dirs[i]) {
			reefs_dirs[i] = d;
			pthread_mutex_unlock(&reefs_dirs_lock);
			return i + 0x10000;
		}
	}
	pthread_mutex_unlock(&reefs_dirs_lock);
	return -EMFILE;
}

static DIR *reefs_get_dir(int fd)
{
	DIR *d = NULL;
	int idx = fd - 0x10000;
	if (idx < 0 || idx >= REEFS_MAX_DIRS)
		return NULL;

	pthread_mutex_lock(&reefs_dirs_lock);
	d = reefs_dirs[idx];
	pthread_mutex_unlock(&reefs_dirs_lock);
	return d;
}

static int reefs_free_dir(int fd)
{
	int idx = fd - 0x10000;
	if (idx < 0 || idx >= REEFS_MAX_DIRS)
		return -EINVAL;

	pthread_mutex_lock(&reefs_dirs_lock);
	if (!reefs_dirs[idx]) {
		pthread_mutex_unlock(&reefs_dirs_lock);
		return -EBADF;
	}
	reefs_dirs[idx] = NULL;
	pthread_mutex_unlock(&reefs_dirs_lock);
	return 0;
}

static int reefs_mkdirs(char *dir, mode_t mode)
{
	int i, len = supp_strlen(dir), ret;

	DMSG("mkdirs %s\n", dir);

	for (i = 0; i <= len; i++) {
		if ((dir[i] == '/' && i > 0) || (i == len)) {
			dir[i] = 0;
			ret = mkdir(dir, mode);
			if (ret < 0 && errno != EEXIST && errno != EISDIR) {
				ret = errno;
				EMSG("mkdir %s, failed %d\n", dir, ret);
				return -ret;
			}
			dir[i] = '/';
		}
	}

	return 0;
}

static int reefs_build_path(char *out, const char *in, size_t buflen)
{
	int ret;

	/* Skip leading slashes to normalize path */
	if (*in == '/')
		in++;

	/*
	 * Build the full path: prefix + in.
	 */
	ret = supp_strlcpy(out, REEFS_PATH, buflen);
	ret = supp_strlcat(out, in, buflen);
	if (ret >= buflen)
		return -ENAMETOOLONG;

	DMSG("reefs_build_path dst %s ilen %d\n", out, ret);
	if (!supp_strstr(out, in))
		EMSG("reefs_build_path FAIL %s\n", in);

	/* Reject path traversal sequences */
	if (supp_strstr(out, ".."))
		return -EINVAL;

	/* Reject hidden files starting with . (except ./ prefix) */
	if (supp_strstr(out, "/.") && !supp_strstr(out, "/.."))
		return -EINVAL;

	return 0;
}

static inline int reefs_isroot(const char *path)
{
	size_t len = supp_strlen(path);

	if (len == 0 || (len == 1 && path[0] == '/'))
		return true;

	return false;
}

static inline char *reefs_dirname(char *path)
{
	int i = 0;
	unsigned int l = 0;

	if (!path || *path == 0)
		return NULL;

	l = supp_strlen(path);

	while (l && (path[l - 1] == '/')) {
		path[l - 1] = 0;
		l--;
	}

	for (i = l - 1; i >= 0; i--) {
		if (path[i] == '/') {
			path[i] = 0;
			break;
		}
	}

	return (i < 0 || *path == 0) ? "/" : path;
}

static int reefs_flags_from_rpc(int rpc_flags)
{
	int flags = 0;

	if ((rpc_flags & 0x3) == REEFS_O_RDONLY)
		flags |= O_RDONLY;
	else if ((rpc_flags & 0x3) == REEFS_O_WRONLY)
		flags |= O_WRONLY;
	else if ((rpc_flags & 0x3) == REEFS_O_RDWR)
		flags |= O_RDWR;

	if (rpc_flags & REEFS_O_CREAT)
		flags |= O_CREAT;
	if (rpc_flags & REEFS_O_EXCL)
		flags |= O_EXCL;
	if (rpc_flags & REEFS_O_TRUNC)
		flags |= O_TRUNC;
	if (rpc_flags & REEFS_O_APPEND)
		flags |= O_APPEND;

	return flags;
}

static int reefs_whence_from_rpc(int rpc_whence)
{
	if (rpc_whence == REEFS_SEEK_SET)
		return SEEK_SET;
	if (rpc_whence == REEFS_SEEK_CUR)
		return SEEK_CUR;
	if (rpc_whence == REEFS_SEEK_END)
		return SEEK_END;
	return -1;
}

static int reefs_open(struct reefs_cmd *r)
{
	int ret = -1;
	char path[REEFS_PATH_MAX];
	char dir[REEFS_PATH_MAX];
	int flags = reefs_flags_from_rpc(r->flags) | O_NOFOLLOW;

	if (reefs_build_path(path, r->data, sizeof(path)) != 0)
		return -EACCES;

	if (flags & O_CREAT) {
		DMSG("creating %s\n", path);
		/* make parent directory */
		supp_strcpy(dir, path);
		/*
		 * Hold reefs_meta_lock across mkdirs + open to prevent a
		 * concurrent rmdir from removing the parent directory between
		 * the two operations.
		 */
		pthread_mutex_lock(&reefs_meta_lock);
		ret = reefs_mkdirs(reefs_dirname(dir), 0700);
		if (ret != 0) {
			pthread_mutex_unlock(&reefs_meta_lock);
			return ret;
		}
		ret = open(path, flags, 0600);
		pthread_mutex_unlock(&reefs_meta_lock);
	} else {
		DMSG("opening %s\n", path);
		pthread_mutex_lock(&reefs_meta_lock);
		ret = open(path, flags, 0600);
		pthread_mutex_unlock(&reefs_meta_lock);
	}

	if (ret < 0) {
		ret = -errno;
		DMSG("open %s, errno %d\n", path, -ret);
	} else {
		DMSG("open %s, fd %d\n", path, ret);
	}

	return ret;
}

static int reefs_close(struct reefs_cmd *r)
{
	int ret = close(r->fd);

	ret = ret ? -errno : ret;

	DMSG("close %d, ret %d\n", r->fd, ret);

	return ret;
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
			EMSG("read %d fail, errno %d\n", r->fd, errno);
			break;
		} else if (rc == 0) {
			DMSG("read %d rc=0, errno %d\n", r->fd, errno);
			break;
		}

		offset += rc;
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
			EMSG("write %d fail, errno %d\n", r->fd, errno);
			break;
		} else if (rc == 0) {
			DMSG("write %d rc=0, errno %d\n", r->fd, errno);
			break;
		}

		offset += rc;
	}

	return (rc < 0) ? rc : offset;
}

static int reefs_ftruncate(struct reefs_cmd *r)
{
	int ret = ftruncate(r->fd, r->len);

	return ret ? -errno : ret;
}

static int reefs_unlink(struct reefs_cmd *r)
{
	int ret = -1;
	char path[REEFS_PATH_MAX];

	if (reefs_build_path(path, r->data, sizeof(path)) != 0)
		return -EACCES;

	DMSG("unlink file %s\n", path);

	ret = unlink(path);

	return ret ? -errno : ret;
}

static int reefs_rename(struct reefs_cmd *r)
{
	int ret = -1;
	char oldpath[REEFS_PATH_MAX];
	char newpath[REEFS_PATH_MAX];
	size_t first_len = supp_strnlen(r->data, REEFS_PATH_MAX);

	/* Ensure first path is null-terminated within bounds */
	if (first_len >= REEFS_PATH_MAX)
		return -ENAMETOOLONG;

	if (reefs_build_path(oldpath, r->data,
			sizeof(oldpath)) != 0)
		return -EACCES;
	if (reefs_build_path(newpath, r->data +
			first_len + 1, sizeof(newpath)) != 0)
		return -EACCES;

	DMSG("rename %s -> %s\n", oldpath, newpath);

	if (reefs_isroot(r->data))
		return -EBUSY;

	pthread_mutex_lock(&reefs_meta_lock);
	ret = rename(oldpath, newpath);
	pthread_mutex_unlock(&reefs_meta_lock);

	return ret ? -errno : ret;
}

static int reefs_mkdir(struct reefs_cmd *r)
{
	int ret = -1;
	char path[REEFS_PATH_MAX];

	if (reefs_build_path(path, r->data, sizeof(path)) != 0)
		return -EACCES;

	pthread_mutex_lock(&reefs_meta_lock);
	ret = reefs_mkdirs(path, 0700);
	pthread_mutex_unlock(&reefs_meta_lock);

	return ret;
}

static int reefs_opendir(struct reefs_cmd *r)
{
	int ret = -1;
	int dfd = -1;
	DIR *d = NULL;
	char path[REEFS_PATH_MAX];

	if (reefs_build_path(path, r->data, sizeof(path)) != 0)
		return -EACCES;

	DMSG("opendir %s\n", path);

	dfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (dfd < 0) {
		ret = -errno;
		DMSG("opendir %s, errno %d\n", path, -ret);
		return ret;
	}

	d = fdopendir(dfd);
	if (!d) {
		ret = -errno;
		close(dfd);
		return ret;
	}

	ret = reefs_alloc_dir(d);
	if (ret < 0) {
		closedir(d);
		return ret;
	}

	return ret;
}

static int reefs_closedir(struct reefs_cmd *r)
{
	int ret = -1;
	DIR *d = reefs_get_dir(r->fd);

	if (!d)
		return -EINVAL;

	ret = reefs_free_dir(r->fd);
	if (ret != 0)
		return ret;

	ret = closedir(d);

	return ret ? -errno : ret;
}

static int reefs_readdir(struct reefs_cmd *r)
{
	DIR *dir = reefs_get_dir(r->fd);
	struct reefs_dirent *d = (struct reefs_dirent *)r->data;
	off_t str_len = 0, retlen = 0, lastdoff = telldir(dir);
	off_t reclen = -1, structsz = 0, cnt = r->len;
	struct dirent *e = NULL;

	if (!d || !dir)
		return -EINVAL;

	structsz = sizeof(d->d_reclen) + sizeof(d->d_off) + sizeof(d->d_type);

	while (cnt && ((e = readdir(dir)) != NULL)) {
		if ((e->d_name[0] == '.') &&
			(e->d_name[1] == 0 || e->d_name[1] == '.'))
			continue;

		str_len = supp_strlen(e->d_name) + 1;
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
		supp_memcpy(d->d_name, e->d_name, str_len);

		retlen += reclen;
		cnt -= reclen;
		d = (void *)d + reclen;
	}

	return retlen;
}

static int reefs_seekdir(struct reefs_cmd *r)
{
	DIR *dir = reefs_get_dir(r->fd);

	if (!dir)
		return -EINVAL;

	seekdir(dir, r->len);

	return 0;
}

static int reefs_rmdir(struct reefs_cmd *r)
{
	int ret = -1;
	char path[REEFS_PATH_MAX];

	if (reefs_build_path(path, r->data, sizeof(path)) != 0)
		return -EACCES;

	DMSG("rmdir %s\n", path);

	if (reefs_isroot(r->data))
		return -EBUSY;

	pthread_mutex_lock(&reefs_meta_lock);
	ret = rmdir(path);
	if (ret != 0)
		ret = -errno;
	pthread_mutex_unlock(&reefs_meta_lock);

	return ret;
}

static off_t reefs_lseek(struct reefs_cmd *r)
{
	int whence = reefs_whence_from_rpc(r->flags);
	off_t ret = -EINVAL;

	if (whence < 0)
		return ret;

	ret = lseek(r->fd, r->len, whence);

	if (ret < 0)
		ret = -errno;

	return ret;
}

static ssize_t reefs_pread(struct reefs_cmd *r)
{
	void *data = r->data;
	off_t off = r->flags;
	off_t rb = 0, rc = -1, offset = 0;

	while (offset < r->len) {
		rb = r->len - offset;
		rc = pread(r->fd, data + offset, rb, off + offset);

		if (rc < 0) {
			rc = -errno;
			EMSG("pread %d fail, errno %d\n", r->fd, errno);
			break;
		} else if (rc == 0) {
			DMSG("pread %d rc=0, errno %d\n", r->fd, errno);
			break;
		}

		offset += rc;
	}

	return (rc < 0) ? rc : offset;
}

static ssize_t reefs_pwrite(struct reefs_cmd *r)
{
	void *data = r->data;
	off_t off = r->flags;
	off_t wb = 0, rc = -1, offset = 0;

	while (offset < r->len) {
		wb = r->len - offset;
		rc = pwrite(r->fd, data + offset, wb, off + offset);

		if (rc < 0) {
			rc = -errno;
			EMSG("pwrite %d fail, errno %d\n", r->fd, errno);
			break;
		} else if (rc == 0) {
			DMSG("pwrite %d rc=0, errno %d\n", r->fd, errno);
			break;
		}

		offset += rc;
	}

	return (rc < 0) ? rc : offset;
}

static int reefs_fstat(struct reefs_cmd *r)
{
	int ret = -1;
	struct stat st;
	struct reefs_stat *rst = (struct reefs_stat *)r->data;
	int fd = r->fd;

	if (r->flags & REEFS_O_DIRECTORY) {
		DIR *d = reefs_get_dir(r->fd);
		if (!d)
			return -EINVAL;
		fd = dirfd(d);
	}

	ret = fstat(fd, &st);
	if (ret == 0) {
		rst->rst_size = st.st_size;
		rst->rst_atime = st.st_atime;
		rst->rst_mtime = st.st_mtime;
		rst->rst_ctime = st.st_ctime;
	} else {
		ret = -errno;
	}

	return ret;
}

int reefs_routine(struct reefs_cmd *cmd)
{
	int ret = -EINVAL;

	if (!cmd)
		return ret;

	switch (cmd->hdr.op) {
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
		ret = reefs_ftruncate(cmd);
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
	case REEFS_FSTAT:
		ret = reefs_fstat(cmd);
		break;
	case REEFS_PREAD:
		ret = reefs_pread(cmd);
		break;
	case REEFS_PWRITE:
		ret = reefs_pwrite(cmd);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	cmd->hdr.ret = mbedtee_supp_errno_to_gp(ret);
	return ret;
}
