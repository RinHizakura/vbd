// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The volume: a data log (log.h) over a base cache (cache.h).
 *
 * disk.log  every chunk the volume was written with
 *
 * A read looks in the data log; what the volume never wrote comes from the
 * base cache.
 *
 * The super block in front of the log names the base image (a local path
 * or an http(s) URL) and remembers its size and a validator from the backend
 * (base/base.h), so a log is never paired with the wrong base by mistake.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "vbd.h"
#include "cache.h"
#include "crc32c.h"
#include "log.h"
#include "base/base.h"

#define VBD_VERSION	1

struct super {
	uint32_t crc;		/* crc32c of everything after this field */
	uint32_t magic;
	uint32_t version;
	uint32_t chunk_size;
	uint64_t size;
	uint64_t base_size;	/* size of the base at create */
	char     base_tag[BASE_TAG_LEN];	/* base validator at create (base_ops.probe) */
	char     base[SUPER_SIZE - 32 - BASE_TAG_LEN];	/* path or URL; empty: zeros */
};
_Static_assert(sizeof(struct super) == SUPER_SIZE, "super block");

/* vbd.at value for a normal, writable open */
#define LIVE		ALL_EPOCHS

struct vbd {
	uint64_t size;
	uint32_t chunk_size;
	uint64_t at;		/* LIVE, or the snapshot this read-only view shows */
	struct log data;	/* what the volume was written with */
	struct cache *cache;	/* what it holds where it was never written */
};

/* Opened with vbd_open: the newest content, writable. Not a snapshot view. */
static bool writable(const struct vbd *v)
{
	return v->at == LIVE;
}

static uint32_t super_crc(const struct super *sb)
{
	return crc32c(0, &sb->magic, sizeof(*sb) - offsetof(struct super, magic));
}

/* Everything create enforces, re-checked on open so a damaged super is refused. */
static bool super_valid(const struct super *sb)
{
	return sb->magic == VBD_MAGIC && sb->version == VBD_VERSION &&
	       sb->crc == super_crc(sb) &&
	       IS_POWER_OF_2(sb->chunk_size) && sb->chunk_size >= MIN_CHUNK_SIZE && sb->size &&
	       IS_ALIGNED(sb->size, sb->chunk_size) &&
	       !sb->base[sizeof(sb->base) - 1];
}

int vbd_create(const char *log_path, uint64_t size, uint32_t chunk_size,
	       const char *base_path)
{
	struct super sb = { .magic = VBD_MAGIC, .version = VBD_VERSION,
			    .chunk_size = chunk_size, .size = size };
	int ret;

	if (!IS_POWER_OF_2(chunk_size) || chunk_size < MIN_CHUNK_SIZE ||
	    !IS_ALIGNED(size, chunk_size))
		return -EINVAL;
	if (base_path) {
		ret = base_ops_for(base_path)->probe(base_path, sb.base, sizeof(sb.base),
						     &sb.base_size, sb.base_tag,
						     sizeof(sb.base_tag));
		if (ret)
			return ret;
	}
	sb.crc = super_crc(&sb);
	ret = log_create(log_path, &sb);
	if (ret || !base_path)
		return ret;
	ret = cache_create(log_path, &sb);
	if (ret)
		unlink(log_path);
	return ret;
}

struct vbd *vbd_open_at(const char *log_path, uint64_t snapshot)
{
	struct vbd *v = calloc(1, sizeof(*v));
	struct super sb;
	uint64_t nchunks;
	int err;

	if (!v)
		return NULL;
	v->at = snapshot;
	log_init(&v->data);

	if ((err = log_open(&v->data, log_path, &sb)))
		goto fail;
	if (!super_valid(&sb)) {
		err = -EINVAL;
		goto fail;
	}
	v->size = sb.size;
	v->chunk_size = sb.chunk_size;
	nchunks = sb.size / sb.chunk_size;
	/* before the data log is touched: a base that changed refuses the open */
	v->cache = cache_open(log_path, &sb, sb.base, sb.base_size, sb.base_tag,
			      sb.chunk_size, nchunks);
	if (!v->cache) {
		err = -errno;
		goto fail;
	}
	if ((err = log_load(&v->data, sb.chunk_size, nchunks, v->at)))
		goto fail;
	if (!writable(v) && v->at >= v->data.epoch) {
		err = -ENOENT;	/* snapshots so far are 0 .. epoch - 1 */
		goto fail;
	}
	return v;
fail:
	vbd_close(v);
	errno = -err;
	return NULL;
}

struct vbd *vbd_open(const char *log_path)
{
	return vbd_open_at(log_path, LIVE);
}

void vbd_close(struct vbd *v)
{
	if (!v)
		return;
	log_close(&v->data);
	cache_close(v->cache);
	free(v);
}

uint64_t vbd_size(const struct vbd *v)		{ return v->size; }
uint32_t vbd_chunk_size(const struct vbd *v)	{ return v->chunk_size; }
uint64_t vbd_fetched(const struct vbd *v)	{ return cache_fetched(v->cache); }
int vbd_base_err(const struct vbd *v)		{ return cache_err(v->cache); }

/* Read @len bytes of chunk @chunk starting @within bytes into it. */
static int read_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		      uint32_t within, uint32_t len)
{
	void *e = xa_load(&v->data.map, chunk);

	if (e)
		return log_read(&v->data, xa_to_value(e), buf, within, len);
	return cache_read(v->cache, chunk, buf, within, len);
}

/* Cut @len at the end of the volume. */
static size_t clamp_len(const struct vbd *v, size_t len, uint64_t off)
{
	if (off >= v->size)
		return 0;
	return len > v->size - off ? v->size - off : len;
}

/* Syscall convention: bytes done if any, else the error. */
static ssize_t done_or_err(size_t done, int err)
{
	return done ? (ssize_t)done : err;
}

ssize_t vbd_read(struct vbd *v, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;
	size_t done = 0;
	int ret = 0;

	len = clamp_len(v, len, off);
	while (!ret && len) {
		uint32_t within = off % v->chunk_size;
		uint32_t n = v->chunk_size - within;

		if (n > len)
			n = len;
		ret = read_chunk(v, off / v->chunk_size, p, within, n);
		if (!ret) {
			p += n; off += n; len -= n; done += n;
		}
	}
	return done_or_err(done, ret);
}

ssize_t vbd_write(struct vbd *v, const void *buf, size_t len, uint64_t off)
{
	const uint8_t *p = buf;
	uint8_t *tmp = NULL;
	size_t done = 0;
	int ret = 0;

	if (!writable(v))
		return -EROFS;
	if (off >= v->size)
		return -ENOSPC;
	len = clamp_len(v, len, off);
	while (!ret && len) {
		uint64_t chunk = off / v->chunk_size;
		uint32_t within = off % v->chunk_size;
		uint32_t n = v->chunk_size - within;

		if (n > len)
			n = len;
		if (n == v->chunk_size) {
			ret = log_append(&v->data, chunk, p);
		} else {
			/* partial chunk: read-modify-write */
			if (!tmp && !(tmp = malloc(v->chunk_size))) {
				ret = -ENOMEM;
				break;
			}
			ret = read_chunk(v, chunk, tmp, 0, v->chunk_size);
			if (!ret) {
				memcpy(tmp + within, p, n);
				ret = log_append(&v->data, chunk, tmp);
			}
		}
		if (!ret) {
			p += n; off += n; len -= n; done += n;
		}
	}
	free(tmp);
	return done_or_err(done, ret);
}

int vbd_flush(struct vbd *v)
{
	/* the cache can always be refetched; only the data log needs to last */
	return log_sync(&v->data);
}

uint64_t vbd_epoch(const struct vbd *v)		{ return v->data.epoch; }

int vbd_snapshot(struct vbd *v)
{
	int ret;

	if (!writable(v))
		return -EROFS;
	ret = log_snapshot(&v->data);
	return ret ? ret : vbd_flush(v);
}
