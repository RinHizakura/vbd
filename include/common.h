/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small helpers shared across vbd. */
#ifndef VBD_COMMON_H
#define VBD_COMMON_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define IS_POWER_OF_2(x)	((x) != 0 && ((x) & ((x) - 1)) == 0)
/* @a must be a power of two */
#define IS_ALIGNED(x, a)	(((x) & ((a) - 1)) == 0)

/* pread all of it; what lies past the end of the file reads as zeros. 0 or -errno. */
static inline int full_pread(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = pread(fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0) {
			memset(p, 0, len);
			return 0;
		}
		p += n; len -= n; off += n;
	}
	return 0;
}

#endif /* VBD_COMMON_H */
