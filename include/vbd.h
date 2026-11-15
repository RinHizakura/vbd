/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vbd: a block device as an XArray over an append-only log.
 *
 * A volume is a log file plus an optional read-only base image, bound at
 * create time. Reads of a chunk that was never written fall through to the
 * base image (or zeros); base chunks are fetched on first use into a cache
 * file next to the log and served from there afterwards. The base must not
 * change; open fails with ESTALE if its size or validator (mtime, ETag)
 * differs from what create recorded. A base that cannot be reached at all
 * still opens, offline: cached chunks read normally, a cache miss fails
 * with the error that reaching the base gave.
 * A volume can instead be a fork of another: it has no base of its own and
 * reads what it never wrote from its parent, as the parent was at the
 * snapshot it was forked from. Forks of forks form a chain that ends at a
 * volume with a base (or none), whose cache serves all of them.
 * Every write appends a whole chunk to the log; the XArray maps chunk
 * index -> log record, so the newest record for a chunk always wins.
 * Each record is stamped with the volume's epoch, the number of snapshots
 * taken so far; vbd_snapshot advances it.
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
 * chunks: a power of two, 16 at least, that divides @size. @base_path may be NULL for an all-zero base; it is stored as an
 * absolute path.
 *
 * @manifest_path may name a manifest of the base, as `vbd manifest` writes
 * it: the sha256 of each of its chunks. It is kept with the volume, and
 * from then on a chunk fetched from the base that does not match it is not
 * cached and the read that needed it fails with -EIO. NULL for none;
 * -EINVAL if it was not made from this base at this chunk size, or if
 * there is no base.
 */
int vbd_create(const char *log_path, uint64_t size, uint32_t chunk_size,
	       const char *base_path, const char *manifest_path);

/* For vbd_fork: from the parent as it is now. What vbd_at says of a writable volume. */
#define VBD_HEAD	UINT64_MAX

/*
 * Create @log_path as a fork of the volume at @parent_path: a volume of
 * the same size that starts out with the parent's content as of @snapshot
 * and is written independently from then on. It costs one super block.
 * With VBD_HEAD the parent is snapshotted first, for which it must not be
 * open for writing elsewhere (-EBUSY). The parent stays usable and can go
 * on being written; the fork never sees what it writes after the snapshot.
 * The parent's log has to stay where it is.
 */
int vbd_fork(const char *parent_path, uint64_t snapshot, const char *log_path);

/*
 * Open a log and its base, for reading and writing. One open at a time: a
 * second one, in any process, fails with EBUSY until the first is closed.
 * Returns NULL and sets errno.
 */
struct vbd *vbd_open(const char *log_path);

/*
 * Open the volume as it was when snapshot @snapshot was taken: a read-only
 * view, writes and snapshots return -EROFS. ENOENT if no such snapshot.
 * Views can be open while the volume is open for writing.
 */
struct vbd *vbd_open_at(const char *log_path, uint64_t snapshot);

/*
 * The same, for the newest snapshot taken at or before @when, in seconds
 * since 1970. ENOENT if no snapshot is that old.
 */
struct vbd *vbd_open_before(const char *log_path, int64_t when);
void vbd_close(struct vbd *v);

uint64_t vbd_size(const struct vbd *v);
/* The snapshot a view shows; VBD_HEAD for a volume open for writing. */
uint64_t vbd_at(const struct vbd *v);
uint32_t vbd_chunk_size(const struct vbd *v);
/* Base chunks fetched into the local cache so far (0 without a base). */
uint64_t vbd_fetched(const struct vbd *v);
/* 0 when the base is reachable (or there is none), else the negative errno. */
int vbd_base_err(const struct vbd *v);

/*
 * pread/pwrite semantics: the return value is the number of bytes
 * transferred, or a negative errno if nothing was. A read that starts at or
 * past the end returns 0; a write that starts there returns -ENOSPC; either
 * one that crosses the end is cut short at it.
 */
ssize_t vbd_read(struct vbd *v, void *buf, size_t len, uint64_t off);
ssize_t vbd_write(struct vbd *v, const void *buf, size_t len, uint64_t off);

/*
 * Make [@off, @off + @len) read as zeros. The whole chunks in the range
 * cost one record between them, however many there are; part of a chunk at
 * either end is rewritten. What the base or the parent holds there is no
 * longer seen. -EINVAL if the range leaves the volume.
 */
int vbd_zero(struct vbd *v, uint64_t off, uint64_t len);

/*
 * Fetch ahead: from now on, when a read has to fetch a chunk from the base,
 * up to @chunks of the chunks after it are fetched in the background as
 * well, stopping at one that is already there. Off by default, since it
 * downloads what has not been asked for. Does nothing without a reachable
 * base. 0 or -errno.
 */
int vbd_prefetch(struct vbd *v, uint32_t chunks);

/* Make every completed write durable. */
int vbd_flush(struct vbd *v);

/*
 * The epoch new writes are stamped with. Snapshot N froze the content as of
 * epoch N; writes made since belong to epoch N + 1.
 */
uint64_t vbd_epoch(const struct vbd *v);

/* Freeze the current content as a snapshot and advance the epoch. Flushes. */
int vbd_snapshot(struct vbd *v);

#endif /* VBD_H */
