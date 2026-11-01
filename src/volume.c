// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Log layout (all little-endian, host order for now):
 *
 *   [super 4 KiB][record 0][record 1]...
 *   record = [header 64 B][chunk_size bytes of data]
 *
 * The super block names the base image and remembers its size and mtime, so
 * a log is never paired with the wrong base by mistake.
 *
 * Records have a fixed size, so record number <-> file offset is arithmetic
 * and the XArray only has to store the record number as a value entry.
 * On open the log is replayed front to back; the first record whose header
 * or checksum does not verify marks a torn tail and the file is truncated
 * there.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
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

struct vbd {
	int log_fd;
	int base_fd;		/* -1: all-zero base */
	uint64_t size;
	uint32_t chunk_size;
	uint64_t rec_size;	/* HDR_SIZE + chunk_size */
	uint64_t nrec;		/* records in the log; next append goes here */
	struct xarray map;	/* chunk -> xa_mk_value(record number) */
	pthread_mutex_t append_lock;
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

int vbd_create(const char *log_path, uint64_t size, uint32_t chunk_size,
	       const char *base_path)
{
	struct super sb = { .magic = VBD_MAGIC, .version = VBD_VERSION,
			    .chunk_size = chunk_size, .size = size };
	int fd, ret = 0;

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
	fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return -errno;
	if (pwrite(fd, &sb, sizeof(sb), 0) != (ssize_t)sizeof(sb) || fsync(fd))
		ret = -errno;
	close(fd);
	return ret;
}

/* Rebuild the map from the log. Later records override earlier ones. */
static int replay(struct vbd *v)
{
	struct stat st;
	uint8_t *data = malloc(v->chunk_size);
	int ret = 0;

	if (!data)
		return -ENOMEM;
	if (fstat(v->log_fd, &st)) {
		ret = -errno;
		goto out;
	}
	for (v->nrec = 0; rec_off(v, v->nrec + 1) <= (uint64_t)st.st_size; v->nrec++) {
		struct rec_hdr h;
		uint64_t off = rec_off(v, v->nrec);

		if ((ret = full_pread(v->log_fd, &h, sizeof(h), off)) ||
		    (ret = full_pread(v->log_fd, data, v->chunk_size, off + HDR_SIZE)))
			goto out;
		if (h.magic != VBD_MAGIC || h.chunk >= v->size / v->chunk_size ||
		    h.crc != crc32c(0, data, v->chunk_size))
			break;		/* torn tail */
		ret = xa_err(xa_store(&v->map, h.chunk, xa_mk_value(v->nrec), GFP_KERNEL));
		if (ret)
			goto out;
	}
	/* drop anything after the last good record so appends start clean */
	if ((uint64_t)st.st_size != rec_off(v, v->nrec) && ftruncate(v->log_fd, rec_off(v, v->nrec)))
		ret = -errno;
out:
	free(data);
	return ret;
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
	return 0;
}

struct vbd *vbd_open(const char *log_path)
{
	struct vbd *v = calloc(1, sizeof(*v));
	struct super sb;
	int err;

	if (!v)
		return NULL;
	v->base_fd = -1;
	v->log_fd = open(log_path, O_RDWR);
	if (v->log_fd < 0)
		goto fail;
	if ((err = full_pread(v->log_fd, &sb, sizeof(sb), 0))) {
		errno = -err;
		goto fail;
	}
	if (!super_valid(&sb)) {
		errno = EINVAL;
		goto fail;
	}
	v->size = sb.size;
	v->chunk_size = sb.chunk_size;
	v->rec_size = HDR_SIZE + sb.chunk_size;
	if ((err = open_base(v, &sb))) {
		errno = -err;
		goto fail;
	}
	xa_init(&v->map);
	pthread_mutex_init(&v->append_lock, NULL);
	if ((err = replay(v))) {
		errno = -err;
		goto fail;
	}
	return v;
fail:
	err = errno;
	vbd_close(v);
	errno = err;
	return NULL;
}

void vbd_close(struct vbd *v)
{
	if (!v)
		return;
	xa_destroy(&v->map);
	if (v->log_fd >= 0)
		close(v->log_fd);
	if (v->base_fd >= 0)
		close(v->base_fd);
	free(v);
}

uint64_t vbd_size(const struct vbd *v)		{ return v->size; }
uint32_t vbd_chunk_size(const struct vbd *v)	{ return v->chunk_size; }

/* Read @len bytes of chunk @chunk starting @within bytes into it. */
static int read_chunk(struct vbd *v, uint64_t chunk, uint8_t *buf,
		      uint32_t within, uint32_t len)
{
	void *e = xa_load(&v->map, chunk);

	if (e)
		return full_pread(v->log_fd, buf, len,
				  rec_off(v, xa_to_value(e)) + HDR_SIZE + within);
	if (v->base_fd < 0) {
		memset(buf, 0, len);
		return 0;
	}
	return full_pread(v->base_fd, buf, len, chunk * v->chunk_size + within);
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

/* Append one whole chunk and publish it in the map. */
static int append_chunk(struct vbd *v, uint64_t chunk, const uint8_t *data)
{
	struct rec_hdr h = { .magic = VBD_MAGIC, .chunk = chunk,
			     .crc = crc32c(0, data, v->chunk_size) };
	struct iovec iov[2] = {
		{ .iov_base = &h, .iov_len = sizeof(h) },
		{ .iov_base = (void *)data, .iov_len = v->chunk_size },
	};
	uint64_t rec;
	int ret = 0;

	pthread_mutex_lock(&v->append_lock);
	rec = v->nrec;
	if (pwritev(v->log_fd, iov, 2, rec_off(v, rec)) != (ssize_t)v->rec_size) {
		ret = -errno;
		goto out;
	}
	/* data is on its way to the file before anyone can find the record */
	ret = xa_err(xa_store(&v->map, chunk, xa_mk_value(rec), GFP_KERNEL));
	if (!ret)
		v->nrec++;
out:
	pthread_mutex_unlock(&v->append_lock);
	return ret;
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
			ret = append_chunk(v, chunk, p);
		} else {
			/* partial chunk: read-modify-write */
			if (!tmp && !(tmp = malloc(v->chunk_size))) {
				ret = -ENOMEM;
				break;
			}
			ret = read_chunk(v, chunk, tmp, 0, v->chunk_size);
			if (!ret) {
				memcpy(tmp + within, p, n);
				ret = append_chunk(v, chunk, tmp);
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
	return fdatasync(v->log_fd) ? -errno : 0;
}
