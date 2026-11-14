// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The volume: two record logs (log.h) and a base image.
 *
 * disk.log        every chunk the volume was written with
 * disk.log.cache  every base chunk fetched so far (only with a base image)
 *
 * A read looks in the data log, then in the cache, then fetches from the
 * base into the cache; with no base an unwritten chunk reads as zeros.
 *
 * The super block in front of both files names the base image (a local path
 * or an http(s) URL) and remembers its size and a validator from the backend
 * (base/base.h), so a log is never paired with the wrong base by mistake.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "vbd.h"
#include "crc32c.h"
#include "ckpt.h"
#include "log.h"
#include "base/base.h"

#define VBD_VERSION	1
#define CACHE_SUFFIX	".cache"

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
#define LIVE		UINT64_MAX

struct vbd {
	uint64_t size;
	uint32_t chunk_size;
	bool ready;		/* open finished; the maps describe the files */
	const struct base_ops *base_ops;	/* NULL: all-zero base */
	void *base;		/* NULL while base_ops is set: base unreachable, offline */
	int base_err;		/* why the base is unreachable, 0 when it is open */
	uint64_t base_size;	/* bytes the base actually has; the rest reads as zeros */
	uint64_t at;		/* LIVE, or the snapshot this read-only view shows */
	struct log data;	/* what the volume was written with */
	struct log cache;	/* base chunks fetched so far; unused without a base */
	pthread_mutex_t fetch_lock;	/* guards INFLIGHT marks in cache.map */
	pthread_cond_t fetch_done;	/* a fetch finished, marks may have changed */
};

/*
 * Cache map entry for a chunk being fetched right now. A pointer, so
 * xa_is_value() tells it apart from a record number.
 */
static int inflight;
#define INFLIGHT	((void *)&inflight)

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
	       IS_POWER_OF_2(sb->chunk_size) && sb->size &&
	       IS_ALIGNED(sb->size, sb->chunk_size) &&
	       !sb->base[sizeof(sb->base) - 1];
}

static char *cache_path(const char *log_path)
{
	char *p = malloc(strlen(log_path) + sizeof(CACHE_SUFFIX));

	if (p)
		strcat(strcpy(p, log_path), CACHE_SUFFIX);
	return p;
}

/* Write a fresh file holding only @sb. */
static int create_file(const char *path, const struct super *sb)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	int ret = 0;

	if (fd < 0)
		return -errno;
	if (pwrite(fd, sb, sizeof(*sb), 0) != (ssize_t)sizeof(*sb) || fsync(fd))
		ret = -errno;
	close(fd);
	return ret;
}

int vbd_create(const char *log_path, uint64_t size, uint32_t chunk_size,
	       const char *base_path)
{
	struct super sb = { .magic = VBD_MAGIC, .version = VBD_VERSION,
			    .chunk_size = chunk_size, .size = size };
	int ret;

	if (!IS_POWER_OF_2(chunk_size) || !IS_ALIGNED(size, chunk_size))
		return -EINVAL;
	if (base_path) {
		ret = base_ops_for(base_path)->probe(base_path, sb.base, sizeof(sb.base),
						     &sb.base_size, sb.base_tag,
						     sizeof(sb.base_tag));
		if (ret)
			return ret;
	}
	sb.crc = super_crc(&sb);
	ret = create_file(log_path, &sb);
	if (ret || !base_path)
		return ret;

	/* the cache is a second log with the same geometry */
	char *cp = cache_path(log_path);

	if (!cp) {
		ret = -ENOMEM;
	} else {
		ret = create_file(cp, &sb);
		free(cp);
	}
	if (ret)
		unlink(log_path);
	return ret;
}

/* Open the base named in the super block and check it is unchanged. */
/*
 * Bind the base named in the super block. A base that cannot be reached
 * leaves the volume open offline: cached chunks still read, a miss returns
 * the error seen here. A base that changed is refused with -ESTALE, since
 * the cache may then be wrong.
 */
static int open_base(struct vbd *v, const struct super *sb)
{
	char stored[sizeof(sb->base)], tag[BASE_TAG_LEN];
	uint64_t size;
	int ret;

	if (!sb->base[0])
		return 0;
	v->base_ops = base_ops_for(sb->base);
	v->base_size = sb->base_size;
	ret = v->base_ops->probe(sb->base, stored, sizeof(stored), &size, tag, sizeof(tag));
	if (ret) {
		v->base_err = ret;
		return 0;
	}
	if (size != sb->base_size || strcmp(tag, sb->base_tag))
		return -ESTALE;
	v->base = v->base_ops->open(sb->base);
	if (!v->base)
		v->base_err = -errno;
	return 0;
}

struct vbd *vbd_open_at(const char *log_path, uint64_t snapshot)
{
	struct vbd *v = calloc(1, sizeof(*v));
	struct super sb, csb;
	int err;

	if (!v)
		return NULL;
	v->at = snapshot;
	log_init(&v->data);
	log_init(&v->cache);
	pthread_mutex_init(&v->fetch_lock, NULL);
	pthread_cond_init(&v->fetch_done, NULL);

	if ((err = log_open(&v->data, log_path, &sb)))
		goto fail;
	if (!super_valid(&sb)) {
		err = -EINVAL;
		goto fail;
	}
	v->size = sb.size;
	v->chunk_size = sb.chunk_size;
	v->data.chunk_size = v->cache.chunk_size = sb.chunk_size;
	v->data.nchunks = v->cache.nchunks = sb.size / sb.chunk_size;
	if ((err = open_base(v, &sb)))
		goto fail;
	/* a checkpoint is the newest map; a snapshot view filters, so it replays it all */
	if (writable(v))
		ckpt_load(&v->data);
	if ((err = log_replay(&v->data, v->at)))
		goto fail;
	if (!writable(v) && v->at >= v->data.epoch) {
		err = -ENOENT;	/* snapshots so far are 0 .. epoch - 1 */
		goto fail;
	}

	if (v->base_ops) {
		char *cp = cache_path(log_path);

		if (!cp) {
			err = -ENOMEM;
			goto fail;
		}
		err = log_open(&v->cache, cp, &csb);
		free(cp);
		if (err)
			goto fail;
		if (!super_valid(&csb) || csb.size != sb.size ||
		    csb.chunk_size != sb.chunk_size) {
			err = -EINVAL;
			goto fail;
		}
		if ((err = log_replay(&v->cache, ALL_EPOCHS)))
			goto fail;
	}
	v->ready = true;
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
	if (v->ready && writable(v))
		ckpt_save(&v->data);
	log_close(&v->data);
	log_close(&v->cache);
	if (v->base)
		v->base_ops->close(v->base);
	pthread_mutex_destroy(&v->fetch_lock);
	pthread_cond_destroy(&v->fetch_done);
	free(v);
}

uint64_t vbd_size(const struct vbd *v)		{ return v->size; }
uint32_t vbd_chunk_size(const struct vbd *v)	{ return v->chunk_size; }
uint64_t vbd_fetched(const struct vbd *v)	{ return v->cache.nrec; }
int vbd_base_err(const struct vbd *v)		{ return v->base_err; }

/*
 * Bring @chunk of the base into the cache and copy @len bytes of it, from
 * @within, into @buf. Only one thread fetches a given chunk: it marks the
 * chunk INFLIGHT in the cache map first, and later arrivals wait for the
 * mark to turn into a record (then read it) or vanish (then fetch).
 */
static int fetch_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		       uint32_t within, uint32_t len)
{
	uint8_t *tmp;
	void *e;
	int ret = 0;

	if (!v->base)
		return v->base_err;	/* offline: nothing to fetch from */

	pthread_mutex_lock(&v->fetch_lock);
	while ((e = xa_load(&v->cache.map, chunk)) == INFLIGHT)
		pthread_cond_wait(&v->fetch_done, &v->fetch_lock);
	if (!e)
		ret = xa_err(xa_store(&v->cache.map, chunk, INFLIGHT, GFP_KERNEL));
	pthread_mutex_unlock(&v->fetch_lock);
	if (e)
		return log_read(&v->cache, xa_to_value(e), buf, within, len);
	if (ret)
		return ret;

	tmp = malloc(v->chunk_size);
	ret = tmp ? v->base_ops->read(v->base, chunk * v->chunk_size, tmp, v->chunk_size)
		  : -ENOMEM;
	if (!ret)
		ret = log_append(&v->cache, chunk, tmp);	/* replaces the mark */
	if (!ret)
		memcpy(buf, tmp + within, len);
	free(tmp);

	pthread_mutex_lock(&v->fetch_lock);
	if (ret)
		xa_erase(&v->cache.map, chunk);	/* let the next reader try */
	pthread_cond_broadcast(&v->fetch_done);
	pthread_mutex_unlock(&v->fetch_lock);
	return ret;
}

/* Read @len bytes of chunk @chunk starting @within bytes into it. */
static int read_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		      uint32_t within, uint32_t len)
{
	void *e = xa_load(&v->data.map, chunk);

	if (e)
		return log_read(&v->data, xa_to_value(e), buf, within, len);
	if (!v->base_ops || chunk * v->chunk_size >= v->base_size) {
		memset(buf, 0, len);	/* no base, or past its end: nothing to fetch */
		return 0;
	}
	e = xa_load(&v->cache.map, chunk);
	if (e && xa_is_value(e))
		return log_read(&v->cache, xa_to_value(e), buf, within, len);
	return fetch_chunk(v, chunk, buf, within, len);	/* miss, or INFLIGHT */
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
	if (writable(v))
		ckpt_save(&v->data);
	/* the cache can always be refetched; only the data log needs to last */
	return fdatasync(v->data.fd) ? -errno : 0;
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
