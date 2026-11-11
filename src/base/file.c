// SPDX-License-Identifier: GPL-2.0-or-later
/* Base image in a local file. The validator is st_mtim in nanoseconds. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base.h"

static void mtime_tag(const struct stat *st, char *tag, size_t tag_len)
{
	snprintf(tag, tag_len, "%lld", st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec);
}

static int file_probe(const char *name, char *stored, size_t stored_len,
		      uint64_t *size, char *tag, size_t tag_len)
{
	char *abs = realpath(name, NULL);
	struct stat st;

	if (!abs)
		return -errno;
	if (strlen(abs) >= stored_len) {
		free(abs);
		return -ENAMETOOLONG;
	}
	strcpy(stored, abs);
	free(abs);
	if (stat(stored, &st))
		return -errno;
	*size = st.st_size;
	mtime_tag(&st, tag, tag_len);
	return 0;
}

static void *file_open(const char *stored)
{
	int fd = open(stored, O_RDONLY);

	return fd < 0 ? NULL : (void *)(intptr_t)fd;
}

static int file_read(void *h, uint64_t off, void *buf, size_t len)
{
	int fd = (intptr_t)h;
	uint8_t *p = buf;

	while (len) {
		ssize_t n = pread(fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0) {		/* short file: rest reads as zeros */
			memset(p, 0, len);
			return 0;
		}
		p += n; len -= n; off += n;
	}
	return 0;
}

static void file_close(void *h)
{
	close((intptr_t)h);
}

const struct base_ops base_file_ops = {
	.probe = file_probe,
	.open = file_open,
	.read = file_read,
	.close = file_close,
};

const struct base_ops *base_ops_for(const char *name)
{
	if (!strncmp(name, "http://", 7) || !strncmp(name, "https://", 8))
		return &base_http_ops;
	return &base_file_ops;
}
