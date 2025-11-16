/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2019 Xing Loong <xing.xl.loong@gmail.com>
 * FS supplicant for mbedtee
 */

#ifndef _MBEDTEE_SUPP_H
#define _MBEDTEE_SUPP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <linux/tee.h>

#include "mbedtee_msg_supplicant.h"

/* Crash-location checkpoint, set by process_request and reefs_build_path. */
extern int g_crash_fd;

/*
 * EMSG  – always-on error/diagnostic messages, written to crash-log fd.
 * DMSG  – compile-time optional debug messages (off by default).
 */
#define EMSG(fmt, ...) \
	do { \
		if (g_crash_fd >= 0) { \
			char _lb[256]; \
			int _n = snprintf(_lb, sizeof(_lb), \
					  fmt "\n", ##__VA_ARGS__); \
			write(g_crash_fd, _lb, _n); \
		} \
	} while (0)

/* Set to 1 to enable DMSG (verbose debug) output at compile time. */
#define DEBUG_SUPP 0

#if DEBUG_SUPP
#define DMSG(fmt, ...) EMSG(fmt, ##__VA_ARGS__)
#else
#define DMSG(...) do {} while (0)
#endif

/* Some inline helpers to avoid the NEON/SIMD libc */
static inline size_t supp_strlen(const char *s)
{
	const char *sc = NULL;

	for (sc = s; *sc; ++sc)
		;

	return sc - s;
}

static inline size_t supp_strnlen(const char *s, size_t n)
{
	const char *str = NULL;

	for (str = s; ((*str) && (n--)); ++str)
		;

	return str - s;
}

static inline int supp_strncmp(const char *s1, const char *s2, size_t n)
{
	unsigned char c = 0;

	if (n == 0)
		return 0;

	while (n-- && ((c = *s1) == (unsigned char)*s2)) {
		if (n == 0 || c == 0)
			return 0;
		s1++;
		s2++;
	}

	return c - (unsigned char)*s2;
}

static inline char *supp_strcpy(char *dst, const char *src)
{
	char *tmp = dst;

	while ((*dst++ = *src++))
		;

	return tmp;
}

static inline size_t supp_strlcpy(char *dst, const char *src, size_t n)
{
	char *d = dst;
	const char *s = src;

	if (n != 0) {
		while (--n && (*d++ = *s++))
			;
		if (n == 0)
			*d = 0;
	}

	if (n == 0) {
		while (*s++)
			;
	}

	return s - src - 1;
}

static inline size_t supp_strlcat(char *dst, const char *src, size_t cnt)
{
	char *d = dst;
	const char *s = src;
	size_t n = cnt, oridlen = 0, left = 0;

	while (n && *d) {
		n--;
		d++;
	}

	oridlen = cnt - n;
	left = cnt - oridlen;
	if (left == 0) {
		while (*s) {
			s++;
			oridlen++;
		}
		return oridlen;
	}

	while (--left && *s)
		*d++ = *s++;

	if (left == 0) {
		while (*s)
			s++;
	}

	*d = 0;

	return oridlen + (s - src);
}

static inline int supp_memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *pa = a, *pb = b;
	size_t i;

	for (i = 0; i < n; i++) {
		if (pa[i] != pb[i])
			return (int)pa[i] - (int)pb[i];
	}
	return 0;
}

static inline void *supp_memset(void *s, int c, size_t n)
{
	unsigned char *p = s;
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)c;
	return s;
}

static inline void *supp_memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	size_t i;

	for (i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

static inline void *supp_memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d <= s || d >= s + n) {
		size_t i;
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		while (n--)
			d[n] = s[n];
	}
	return dst;
}

static inline char *supp_strstr(const char *s1, const char *s2)
{
	size_t l1 = 0, l2 = 0;

	l2 = supp_strlen(s2);
	if (l2 == 0)
		return (char *)s1;

	l1 = supp_strlen(s1);

	while (l1 >= l2) {
		l1--;
		if (supp_memcmp(s1, s2, l2) == 0)
			return (char *)s1;
		s1++;
	}

	return NULL;
}

int reefs_routine(struct reefs_cmd *cmd);
int rpmb_routine(struct rpmb_cmd *data);

#ifdef __cplusplus
}
#endif

#endif /* _MBEDTEE_SUPP_H */
