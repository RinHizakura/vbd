/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A base image: an immutable byte range the volume falls back to. Two
 * backends, a local file and an HTTP URL, behind one set of operations.
 */
#ifndef VBD_BASE_H
#define VBD_BASE_H

#include <stddef.h>
#include <stdint.h>

#define BASE_TAG_LEN	128

struct base_ops {
	/*
	 * Resolve @name into the form to store (absolute path, URL as given),
	 * its size in bytes and a validator (@tag) that changes when the
	 * content might have. Returns 0 or -errno.
	 */
	int (*probe)(const char *name, char *stored, size_t stored_len,
		     uint64_t *size, char *tag, size_t tag_len);
	/* Open a stored name for reading. Returns NULL and sets errno. */
	void *(*open)(const char *stored);
	/* Read [off, off + len); bytes past the end read as zeros. 0 or -errno. */
	int (*read)(void *h, uint64_t off, void *buf, size_t len);
	void (*close)(void *h);
};

extern const struct base_ops base_file_ops;
extern const struct base_ops base_http_ops;

/* Pick the backend from the name: http(s):// is remote, anything else a path. */
const struct base_ops *base_ops_for(const char *name);

#endif /* VBD_BASE_H */
