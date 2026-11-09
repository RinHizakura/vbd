// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Two files share one layout (host byte order for now):
 *
 *   [super 4 KiB][record 0][record 1]...
 *   record = [header 64 B][chunk_size bytes of data]
 *
 * disk.log        every chunk the volume was written with
 * disk.log.cache  every base chunk fetched so far (only with a base image)
 *
 * Records have a fixed size, so record number <-> file offset is arithmetic
 * and each XArray only has to store the record number as a value entry.
 * On open both files are replayed front to back; the first record whose
 * header or checksum does not verify marks a torn tail and the file is
 * truncated there.
 *
 * The super block names the base image and remembers its size and mtime, so
 * a log is never paired with the wrong base by mistake.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <xarray.h>

#include "common.h"
#include "vbd.h"
#include "crc32c.h"

#define VBD_MAGIC	0x31646276u	/* "vbd1" */
#define VBD_VERSION	1
#define HDR_SIZE	64
#define SUPER_SIZE	4096
#define CACHE_SUFFIX	".cache"

struct super {
	uint32_t crc;		/* crc32c of everything after this field */
	uint32_t magic;
	uint32_t version;
	uint32_t chunk_size;
	uint64_t size;
	uint64_t base_size;	/* st_size of the base at create */
	int64_t  base_mtime;	/* st_mtim of the base at create, in ns */
	char     base[SUPER_SIZE - 40];	/* absolute path; empty: all-zero base */
};

struct rec_hdr {
	uint32_t magic;
	uint32_t crc;		/* crc32c of the data that follows */
	uint64_t chunk;
	uint64_t epoch;		/* reserved for snapshots; always 0 for now */
	uint8_t  pad[HDR_SIZE - 24];
};

/* An append-only file of records plus the map that indexes it. */
struct log {
	int fd;
	uint64_t nrec;		/* records in the file; next append goes here */
	struct xarray map;	/* chunk -> xa_mk_value(record number) */
	pthread_mutex_t append_lock;
};

struct vbd {
	uint64_t size;
	uint32_t chunk_size;
	uint64_t rec_size;	/* HDR_SIZE + chunk_size */
	int base_fd;		/* -1: all-zero base */
	uint64_t base_size;	/* bytes the base actually has; the rest reads as zeros */
	struct log data;	/* what the volume was written with */
	struct log cache;	/* base chunks fetched so far; unused without a base */
};

static uint64_t rec_off(const struct vbd *v, uint64_t rec)
{
	return SUPER_SIZE + rec * v->rec_size;
}

static int full_pread(int fd, void *buf, size_t len, uint64_t off)
{
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

static int64_t mtime_ns(const struct stat *st)
{
	return st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
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
		char *abs = realpath(base_path, NULL);
		struct stat st;

		if (!abs)
			return -errno;
		if (strlen(abs) >= sizeof(sb.base)) {
			free(abs);
			return -ENAMETOOLONG;
		}
		strcpy(sb.base, abs);
		free(abs);
		if (stat(sb.base, &st))
			return -errno;
		sb.base_size = st.st_size;
		sb.base_mtime = mtime_ns(&st);
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

static void log_init(struct log *l)
{
	l->fd = -1;
	xa_init(&l->map);
	pthread_mutex_init(&l->append_lock, NULL);
}

static void log_close(struct log *l)
{
	xa_destroy(&l->map);
	if (l->fd >= 0)
		close(l->fd);
	l->fd = -1;
}

/* Open @path, hand back its super block if it is sound. */
static int log_open(struct log *l, const char *path, struct super *sb)
{
	int ret;

	l->fd = open(path, O_RDWR);
	if (l->fd < 0)
		return -errno;
	ret = full_pread(l->fd, sb, sizeof(*sb), 0);
	if (ret)
		return ret;
	return super_valid(sb) ? 0 : -EINVAL;
}

/* Rebuild the map from the file. Later records override earlier ones. */
static int log_replay(struct vbd *v, struct log *l)
{
	struct stat st;
	uint8_t *data = malloc(v->chunk_size);
	int ret = 0;

	if (!data)
		return -ENOMEM;
	if (fstat(l->fd, &st)) {
		ret = -errno;
		goto out;
	}
	for (l->nrec = 0; rec_off(v, l->nrec + 1) <= (uint64_t)st.st_size; l->nrec++) {
		struct rec_hdr h;
		uint64_t off = rec_off(v, l->nrec);

		if ((ret = full_pread(l->fd, &h, sizeof(h), off)) ||
		    (ret = full_pread(l->fd, data, v->chunk_size, off + HDR_SIZE)))
			goto out;
		if (h.magic != VBD_MAGIC || h.chunk >= v->size / v->chunk_size ||
		    h.crc != crc32c(0, data, v->chunk_size))
			break;		/* torn tail */
		ret = xa_err(xa_store(&l->map, h.chunk, xa_mk_value(l->nrec), GFP_KERNEL));
		if (ret)
			goto out;
	}
	/* drop anything after the last good record so appends start clean */
	if ((uint64_t)st.st_size != rec_off(v, l->nrec) && ftruncate(l->fd, rec_off(v, l->nrec)))
		ret = -errno;
out:
	free(data);
	return ret;
}

/* Append one whole chunk and publish it in the map. */
static int log_append(struct vbd *v, struct log *l, uint64_t chunk, const uint8_t *data)
{
	struct rec_hdr h = { .magic = VBD_MAGIC, .chunk = chunk,
			     .crc = crc32c(0, data, v->chunk_size) };
	struct iovec iov[2] = {
		{ .iov_base = &h, .iov_len = sizeof(h) },
		{ .iov_base = (void *)data, .iov_len = v->chunk_size },
	};
	uint64_t rec;
	int ret = 0;

	pthread_mutex_lock(&l->append_lock);
	rec = l->nrec;
	if (pwritev(l->fd, iov, 2, rec_off(v, rec)) != (ssize_t)v->rec_size) {
		ret = -errno;
		goto out;
	}
	/* data is on its way to the file before anyone can find the record */
	ret = xa_err(xa_store(&l->map, chunk, xa_mk_value(rec), GFP_KERNEL));
	if (!ret)
		l->nrec++;
out:
	pthread_mutex_unlock(&l->append_lock);
	return ret;
}

/* Read @len bytes of record @rec starting @within bytes into its data. */
static int log_read(struct vbd *v, struct log *l, uint64_t rec, uint8_t *buf,
		    uint32_t within, uint32_t len)
{
	return full_pread(l->fd, buf, len, rec_off(v, rec) + HDR_SIZE + within);
}

/* Open the base named in the super block and check it is unchanged. */
static int open_base(struct vbd *v, const struct super *sb)
{
	struct stat st;

	if (!sb->base[0])
		return 0;
	v->base_fd = open(sb->base, O_RDONLY);
	if (v->base_fd < 0 || fstat(v->base_fd, &st))
		return -errno;
	if ((uint64_t)st.st_size != sb->base_size || mtime_ns(&st) != sb->base_mtime)
		return -ESTALE;
	v->base_size = sb->base_size;
	return 0;
}

struct vbd *vbd_open(const char *log_path)
{
	struct vbd *v = calloc(1, sizeof(*v));
	struct super sb, csb;
	int err;

	if (!v)
		return NULL;
	v->base_fd = -1;
	log_init(&v->data);
	log_init(&v->cache);

	if ((err = log_open(&v->data, log_path, &sb)))
		goto fail;
	v->size = sb.size;
	v->chunk_size = sb.chunk_size;
	v->rec_size = HDR_SIZE + sb.chunk_size;
	if ((err = open_base(v, &sb)))
		goto fail;
	if ((err = log_replay(v, &v->data)))
		goto fail;

	if (v->base_fd >= 0) {
		char *cp = cache_path(log_path);

		if (!cp) {
			err = -ENOMEM;
			goto fail;
		}
		err = log_open(&v->cache, cp, &csb);
		free(cp);
		if (err)
			goto fail;
		if (csb.size != sb.size || csb.chunk_size != sb.chunk_size) {
			err = -EINVAL;
			goto fail;
		}
		if ((err = log_replay(v, &v->cache)))
			goto fail;
	}
	return v;
fail:
	vbd_close(v);
	errno = -err;
	return NULL;
}

void vbd_close(struct vbd *v)
{
	if (!v)
		return;
	log_close(&v->data);
	log_close(&v->cache);
	if (v->base_fd >= 0)
		close(v->base_fd);
	free(v);
}

uint64_t vbd_size(const struct vbd *v)		{ return v->size; }
uint32_t vbd_chunk_size(const struct vbd *v)	{ return v->chunk_size; }
uint64_t vbd_fetched(const struct vbd *v)	{ return v->cache.nrec; }

/*
 * Bring @chunk of the base into the cache and copy @len bytes of it, from
 * @within, into @buf.
 *
 * ponytail: two readers missing the same chunk both fetch it and append two
 * records; the second store wins and the first becomes garbage. An
 * in-flight mark on the cache map would let the second wait instead.
 */
static int fetch_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		       uint32_t within, uint32_t len)
{
	uint8_t *tmp = malloc(v->chunk_size);
	int ret;

	if (!tmp)
		return -ENOMEM;
	ret = full_pread(v->base_fd, tmp, v->chunk_size, chunk * v->chunk_size);
	if (!ret)
		ret = log_append(v, &v->cache, chunk, tmp);
	if (!ret)
		memcpy(buf, tmp + within, len);
	free(tmp);
	return ret;
}

/* Read @len bytes of chunk @chunk starting @within bytes into it. */
static int read_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		      uint32_t within, uint32_t len)
{
	void *e = xa_load(&v->data.map, chunk);

	if (e)
		return log_read(v, &v->data, xa_to_value(e), buf, within, len);
	if (v->base_fd < 0 || chunk * v->chunk_size >= v->base_size) {
		memset(buf, 0, len);	/* no base, or past its end: nothing to fetch */
		return 0;
	}
	e = xa_load(&v->cache.map, chunk);
	if (e)
		return log_read(v, &v->cache, xa_to_value(e), buf, within, len);
	return fetch_chunk(v, chunk, buf, within, len);
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
			ret = log_append(v, &v->data, chunk, p);
		} else {
			/* partial chunk: read-modify-write */
			if (!tmp && !(tmp = malloc(v->chunk_size))) {
				ret = -ENOMEM;
				break;
			}
			ret = read_chunk(v, chunk, tmp, 0, v->chunk_size);
			if (!ret) {
				memcpy(tmp + within, p, n);
				ret = log_append(v, &v->data, chunk, tmp);
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
	return fdatasync(v->data.fd) ? -errno : 0;
}
