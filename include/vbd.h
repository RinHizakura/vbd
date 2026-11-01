/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vbd: a block device as an XArray over an append-only log.
 *
 * A volume is a log file plus an optional read-only base image, bound at
 * create time. Reads of a chunk that was never written fall through to the
 * base image (or zeros). The base must not change afterwards; open fails
 * with ESTALE if its size or mtime differs from what create recorded.
 * Every write appends a whole chunk to the log; the XArray maps chunk
 * index -> log record, so the newest record for a chunk always wins.
 *
 * Threads that call into vbd must have called rcu_register_thread().
 * Functions return 0 or a negative errno unless stated otherwise.
 */
#ifndef VBD_H
#define VBD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct vbd;

/*
 * Create an empty log for a volume of @size bytes, split in @chunk_size
 * chunks. @base_path may be NULL for an all-zero base; it is stored as an
 * absolute path.
 */
int vbd_create(const char *log_path, uint64_t size, uint32_t chunk_size,
	       const char *base_path);

/* Open a log and its base. Returns NULL and sets errno. */
struct vbd *vbd_open(const char *log_path);
void vbd_close(struct vbd *v);

uint64_t vbd_size(const struct vbd *v);
uint32_t vbd_chunk_size(const struct vbd *v);

/*
 * pread/pwrite semantics: the return value is the number of bytes
 * transferred, or a negative errno if nothing was. A read that starts at or
 * past the end returns 0; a write that starts there returns -ENOSPC; either
 * one that crosses the end is cut short at it.
 */
ssize_t vbd_read(struct vbd *v, void *buf, size_t len, uint64_t off);
ssize_t vbd_write(struct vbd *v, const void *buf, size_t len, uint64_t off);

/* Make every completed write durable. */
int vbd_flush(struct vbd *v);

#endif /* VBD_H */
