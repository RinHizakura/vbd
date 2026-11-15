// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The volume: a data log (log.h) over a base cache (cache.h), or for a
 * fork, over the volume it was forked from.
 *
 * disk.log  every chunk the volume was written with
 *
 * A read looks in the data log; what the volume never wrote comes from the
 * base cache, or from the parent, which is itself such a volume.
 *
 * The super block in front of the log (super.h) says how big the volume is
 * and what its base is.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vbd.h"
#include "cache.h"
#include "log.h"
#include "super.h"

/* vbd.at value for a normal, writable open */
#define LIVE		ALL_EPOCHS

struct vbd {
	uint64_t size;
	uint32_t chunk_size;
	uint64_t at;		/* LIVE, or the snapshot this read-only view shows */
	struct log data;	/* what the volume was written with */
	/* what it holds where it was never written comes from one of these two: */
	struct vbd *parent;	/* a fork: the volume it was forked from, as of the fork */
	struct cache *cache;	/* otherwise: the base cache */
};

/* Opened with vbd_open: the newest content, writable. Not a snapshot view. */
static bool writable(const struct vbd *v)
{
	return v->at == LIVE;
}

int vbd_create(const char *log_path, uint64_t size, uint32_t chunk_size,
	       const char *base_path, const char *manifest_path)
{
	struct super sb;
	uint8_t *manifest = NULL;
	int ret = super_init(&sb, size, chunk_size);

	if (ret)
		return ret;
	if (manifest_path && !base_path)
		return -EINVAL;
	if (base_path && (ret = super_set_base(&sb, base_path)))
		return ret;
	if (manifest_path && (ret = super_set_manifest(&sb, manifest_path, &manifest)))
		return ret;
	super_seal(&sb);
	ret = log_create(log_path, &sb);
	if (!ret && base_path && (ret = cache_create(log_path, &sb, manifest)))
		unlink(log_path);
	free(manifest);
	return ret;
}

/*
 * Open the volume as of @snapshot, LIVE for as it is now and writable. With
 * @before, as of the newest snapshot taken by that time instead.
 */
int vbd_fork(const char *parent_path, uint64_t snapshot, const char *log_path)
{
	struct super sb;
	struct vbd *p;
	int ret;

	if (snapshot == VBD_HEAD) {	/* from the parent as it is now: freeze that first */
		p = vbd_open(parent_path);
		if (!p)
			return -errno;
		snapshot = vbd_epoch(p);
		ret = vbd_snapshot(p);
		vbd_close(p);
		if (ret)
			return ret;
	}
	p = vbd_open_at(parent_path, snapshot);	/* is there such a snapshot, and of what shape */
	if (!p)
		return -errno;
	ret = super_init(&sb, p->size, p->chunk_size);
	vbd_close(p);
	if (!ret)
		ret = super_set_parent(&sb, parent_path, snapshot);
	if (ret)
		return ret;
	super_seal(&sb);
	return log_create(log_path, &sb);
}

static struct vbd *open_volume(const char *log_path, uint64_t snapshot,
			       const int64_t *before)
{
	struct vbd *v = calloc(1, sizeof(*v));
	struct super sb;
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
	if (before && (err = log_snapshot_before(&v->data, sb.chunk_size, *before, &v->at)))
		goto fail;	/* -ENOENT: no snapshot that old */
	if (writable(v) && (err = log_lock(&v->data)))
		goto fail;	/* someone else is writing it */
	v->size = sb.size;
	v->chunk_size = sb.chunk_size;
	if (super_parent(&sb)) {
		/* a view of the parent, which opens its own parent in turn */
		v->parent = vbd_open_at(super_parent(&sb), sb.parent_at);
		if (!v->parent) {
			err = -errno;
			goto fail;
		}
		if (v->parent->size != v->size || v->parent->chunk_size != v->chunk_size) {
			err = -EINVAL;	/* not the volume this was forked from */
			goto fail;
		}
	} else {
		/* before the data log is touched: a base that changed refuses the open */
		v->cache = cache_open(log_path, &sb);
		if (!v->cache) {
			err = -errno;
			goto fail;
		}
	}
	if ((err = log_load(&v->data, sb.chunk_size, sb.size / sb.chunk_size, v->at)))
		goto fail;
	if (!writable(v) && v->at >= log_epoch(&v->data)) {
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
	return open_volume(log_path, LIVE, NULL);
}

struct vbd *vbd_open_at(const char *log_path, uint64_t snapshot)
{
	return open_volume(log_path, snapshot, NULL);
}

struct vbd *vbd_open_before(const char *log_path, int64_t when)
{
	return open_volume(log_path, 0, &when);
}

void vbd_close(struct vbd *v)
{
	if (!v)
		return;
	log_close(&v->data);
	vbd_close(v->parent);
	cache_close(v->cache);
	free(v);
}

uint64_t vbd_size(const struct vbd *v)		{ return v->size; }
uint32_t vbd_chunk_size(const struct vbd *v)	{ return v->chunk_size; }
uint64_t vbd_at(const struct vbd *v)		{ return v->at; }

/* The base cache at the bottom of a chain of forks; the one all of them fetch into. */
static struct cache *base_cache(const struct vbd *v)
{
	while (v->parent)
		v = v->parent;
	return v->cache;
}

uint64_t vbd_fetched(const struct vbd *v)	{ return cache_fetched(base_cache(v)); }
int vbd_base_err(const struct vbd *v)		{ return cache_err(base_cache(v)); }

int vbd_prefetch(struct vbd *v, uint32_t chunks)
{
	return cache_prefetch(base_cache(v), chunks);
}

/* Read @len bytes of chunk @chunk starting @within bytes into it. */
static int read_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		      uint32_t within, uint32_t len)
{
	if (log_has(&v->data, chunk))
		return log_read(&v->data, chunk, buf, within, len);
	if (v->parent)		/* whatever the parent held when this was forked from it */
		return read_chunk(v->parent, chunk, buf, within, len);
	return cache_read(v->cache, chunk, buf, within, len);
}

/*
 * Replace @len bytes at @within of @chunk. A whole chunk is appended as it
 * is; a part of one is read-modify-write, since a record holds a whole
 * chunk.
 */
static int write_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		       uint32_t within, uint32_t len)
{
	uint8_t *tmp;
	int ret;

	if (len == v->chunk_size)
		return log_append(&v->data, chunk, buf);
	tmp = malloc(v->chunk_size);
	if (!tmp)
		return -ENOMEM;
	ret = read_chunk(v, chunk, tmp, 0, v->chunk_size);
	if (!ret) {
		memcpy(tmp + within, buf, len);
		ret = log_append(&v->data, chunk, tmp);
	}
	free(tmp);
	return ret;
}

/* What to do with the part of a byte range that lies in one chunk. */
typedef int span_fn(struct vbd *v, uint64_t chunk, uint8_t *buf,
		    uint32_t within, uint32_t len);

/*
 * Walk the byte range [@off, @off + @len), cut short at the end of the
 * volume, one chunk at a time, handing each piece and its place in @buf to
 * @fn. Stops at the first error. Syscall convention: the bytes done if
 * any, else the error; 0 for a range that starts at or past the end.
 */
static ssize_t each_span(struct vbd *v, uint8_t *buf, size_t len, uint64_t off,
			 span_fn *fn)
{
	size_t done = 0;
	int ret = 0;

	if (off >= v->size)
		return 0;
	if (len > v->size - off)
		len = v->size - off;
	while (!ret && len) {
		uint32_t within = off % v->chunk_size;
		uint32_t n = v->chunk_size - within;

		if (n > len)
			n = len;
		ret = fn(v, off / v->chunk_size, buf, within, n);
		if (!ret) {
			buf += n; off += n; len -= n; done += n;
		}
	}
	return done ? (ssize_t)done : ret;
}

ssize_t vbd_read(struct vbd *v, void *buf, size_t len, uint64_t off)
{
	return each_span(v, buf, len, off, read_chunk);
}

ssize_t vbd_write(struct vbd *v, const void *buf, size_t len, uint64_t off)
{
	if (!writable(v))
		return -EROFS;
	if (off >= v->size)
		return -ENOSPC;
	/* write_chunk only reads from the buffer */
	return each_span(v, (uint8_t *)buf, len, off, write_chunk);
}

int vbd_flush(struct vbd *v)
{
	/* the cache can always be refetched; only the data log needs to last */
	return log_sync(&v->data);
}

uint64_t vbd_epoch(const struct vbd *v)		{ return log_epoch(&v->data); }

int vbd_snapshot(struct vbd *v)
{
	int ret;

	if (!writable(v))
		return -EROFS;
	ret = log_snapshot(&v->data);
	return ret ? ret : vbd_flush(v);
}
