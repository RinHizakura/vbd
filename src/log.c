// SPDX-License-Identifier: GPL-2.0-or-later
/* The record log: open, replay, append, read. Format and rules in log.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "crc32c.h"
#include "log.h"

/*
 * A snapshot appends a record with this chunk number. It carries the new
 * epoch and no data, and is never stored in the map.
 *
 * ponytail: the marker costs a whole chunk of zeros, because every record has
 * the same size; a header-only marker would need variable-size records.
 */
#define SNAP_MARK	UINT64_MAX

uint64_t rec_off(const struct log *l, uint64_t rec)
{
	return SUPER_SIZE + rec * (HDR_SIZE + (uint64_t)l->chunk_size);
}

int full_pread(int fd, void *buf, size_t len, uint64_t off)
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

int log_map(struct log *l, uint64_t chunk, uint64_t rec)
{
	void *old = xa_store(&l->map, chunk, xa_mk_value(rec), GFP_KERNEL);

	if (xa_is_err(old))
		return xa_err(old);
	if (!old)
		l->mapped++;
	return 0;
}

void log_init(struct log *l)
{
	l->fd = -1;
	xa_init(&l->map);
	pthread_mutex_init(&l->append_lock, NULL);
}

void log_close(struct log *l)
{
	xa_destroy(&l->map);
	if (l->fd >= 0)
		close(l->fd);
	l->fd = -1;
}

int log_open(struct log *l, const char *path, void *super)
{
	l->fd = open(path, O_RDWR);
	if (l->fd < 0)
		return -errno;
	return full_pread(l->fd, super, SUPER_SIZE, 0);
}

int log_replay(struct log *l, uint64_t upto)
{
	struct stat st;
	uint8_t *data = malloc(l->chunk_size);
	int ret = 0;

	if (!data)
		return -ENOMEM;
	if (fstat(l->fd, &st)) {
		ret = -errno;
		goto out;
	}
	for (; rec_off(l, l->nrec + 1) <= (uint64_t)st.st_size; l->nrec++) {
		struct rec_hdr h;
		uint64_t off = rec_off(l, l->nrec);

		if ((ret = full_pread(l->fd, &h, sizeof(h), off)))
			goto out;
		/*
		 * A checkpoint record says nothing a replay needs, so it is
		 * passed over unread: damage to one must not cost the records
		 * that follow it.
		 */
		if (h.magic == CKPT_MAGIC)
			continue;
		if ((ret = full_pread(l->fd, data, l->chunk_size, off + HDR_SIZE)))
			goto out;
		if (h.magic != VBD_MAGIC ||
		    (h.chunk >= l->nchunks && h.chunk != SNAP_MARK) ||
		    h.crc != crc32c(0, data, l->chunk_size))
			break;		/* torn tail */
		if (h.epoch > l->epoch)
			l->epoch = h.epoch;
		if (h.chunk == SNAP_MARK || h.epoch > upto)
			continue;	/* marker, or written after the snapshot shown */
		if ((ret = log_map(l, h.chunk, l->nrec)))
			goto out;
	}
	/* drop anything after the last good record so appends start clean */
	if ((uint64_t)st.st_size != rec_off(l, l->nrec) && ftruncate(l->fd, rec_off(l, l->nrec)))
		ret = -errno;
out:
	free(data);
	return ret;
}

int rec_write(struct log *l, const void *hdr, const void *data)
{
	struct iovec iov[2] = {
		{ .iov_base = (void *)hdr, .iov_len = HDR_SIZE },
		{ .iov_base = (void *)data, .iov_len = l->chunk_size },
	};
	ssize_t n = pwritev(l->fd, iov, 2, rec_off(l, l->nrec));

	if (n == (ssize_t)(HDR_SIZE + l->chunk_size))
		return 0;
	return n < 0 ? -errno : -ENOSPC;	/* a short write is a full disk */
}

int log_append(struct log *l, uint64_t chunk, const uint8_t *data)
{
	struct rec_hdr h = { .magic = VBD_MAGIC, .chunk = chunk,
			     .crc = crc32c(0, data, l->chunk_size) };
	int ret;

	pthread_mutex_lock(&l->append_lock);
	h.epoch = l->epoch;
	ret = rec_write(l, &h, data);
	/* data is on its way to the file before anyone can find the record */
	if (!ret && chunk != SNAP_MARK)
		ret = log_map(l, chunk, l->nrec);
	if (!ret)
		l->nrec++;
	pthread_mutex_unlock(&l->append_lock);
	return ret;
}

int log_snapshot(struct log *l)
{
	uint8_t *zero = calloc(1, l->chunk_size);
	int ret;

	if (!zero)
		return -ENOMEM;
	/*
	 * Bump first, then persist the bump with a marker. A write that slips
	 * in between already carries the new epoch, so its position relative
	 * to the marker does not matter: replay takes the largest epoch seen.
	 */
	pthread_mutex_lock(&l->append_lock);
	l->epoch++;
	pthread_mutex_unlock(&l->append_lock);
	ret = log_append(l, SNAP_MARK, zero);
	free(zero);
	return ret;
}

int log_read(struct log *l, uint64_t rec, uint8_t *buf, uint32_t within, uint32_t len)
{
	return full_pread(l->fd, buf, len, rec_off(l, rec) + HDR_SIZE + within);
}
