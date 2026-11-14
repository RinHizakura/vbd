// SPDX-License-Identifier: GPL-2.0-or-later
/* The record log: open, replay, append, read. Format and rules in log.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "ckpt.h"
#include "crc32c.h"
#include "log.h"

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

int log_create(const char *path, const void *super)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	int ret = 0;

	if (fd < 0)
		return -errno;
	if (pwrite(fd, super, SUPER_SIZE, 0) != SUPER_SIZE || fsync(fd))
		ret = errno ? -errno : -EIO;
	close(fd);
	return ret;
}

void log_init(struct log *l)
{
	l->fd = -1;
	xa_init(&l->map);
	pthread_mutex_init(&l->append_lock, NULL);
}

/* A view's map is not the whole log's, and a log that failed to load has none. */
static bool takes_checkpoints(const struct log *l)
{
	return l->loaded && l->upto == ALL_EPOCHS;
}

static int log_replay(struct log *l);

/*
 * Hold the log still, to add to it or to save its map: our own threads
 * keep out by the mutex. A shared log also has other processes appending;
 * the file lock keeps them out, and the map is then brought up to date
 * with what they added. Always paired with log_release, whatever it returns.
 */
static int log_hold(struct log *l)
{
	pthread_mutex_lock(&l->append_lock);
	if (!l->shared)
		return 0;
	while (flock(l->fd, LOCK_EX))
		if (errno != EINTR)
			return -errno;
	return log_replay(l);
}

static void log_release(struct log *l)
{
	if (l->shared)
		flock(l->fd, LOCK_UN);
	pthread_mutex_unlock(&l->append_lock);
}

/* Append a checkpoint if one is due. Best effort. */
static void log_checkpoint(struct log *l)
{
	if (!takes_checkpoints(l))
		return;
	if (!log_hold(l))
		ckpt_save(l);
	log_release(l);
}

void log_close(struct log *l)
{
	log_checkpoint(l);
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

/*
 * Bring the map up to date with the file, from record l->nrec on: 0, just
 * past the checkpoint the map was taken from, or where a shared log ended
 * when we last looked.
 */
static int log_replay(struct log *l)
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
		if (h.magic == CKPT_MAGIC) {
			l->ckpt_nrec = l->nrec + 1;	/* someone's checkpoint ends here */
			continue;
		}
		if ((ret = full_pread(l->fd, data, l->chunk_size, off + HDR_SIZE)))
			goto out;
		if (h.magic != VBD_MAGIC ||
		    h.chunk >= l->nchunks ||
		    h.crc != crc32c(0, data, l->chunk_size))
			break;		/* torn tail */
		if (h.epoch > l->epoch)
			l->epoch = h.epoch;
		if (h.epoch > l->upto)
			continue;	/* written after the snapshot shown */
		if ((ret = log_map(l, h.chunk, l->nrec)))
			goto out;
	}
	/*
	 * Drop anything after the last good record so appends start clean. A
	 * view appends nothing, and what looks torn to it may be a record
	 * someone else is in the middle of writing: it leaves the file alone.
	 */
	if (l->upto == ALL_EPOCHS && (uint64_t)st.st_size != rec_off(l, l->nrec) &&
	    ftruncate(l->fd, rec_off(l, l->nrec)))
		ret = -errno;
out:
	free(data);
	return ret;
}

int log_lock(struct log *l)
{
	if (!flock(l->fd, LOCK_EX | LOCK_NB))
		return 0;
	return errno == EWOULDBLOCK ? -EBUSY : -errno;
}

void log_share(struct log *l)
{
	l->shared = true;
}

int log_refresh(struct log *l)
{
	int ret = log_hold(l);

	log_release(l);
	return ret;
}

int log_load(struct log *l, uint32_t chunk_size, uint64_t nchunks, uint64_t upto)
{
	bool shared = l->shared;
	int ret;

	l->chunk_size = chunk_size;
	l->nchunks = nchunks;
	l->upto = upto;
	/* held, but the catching up is the load itself: from a checkpoint if there is one */
	l->shared = false;
	if (shared)
		while (flock(l->fd, LOCK_EX) && errno == EINTR)
			;
	ret = ckpt_load(l) ? 0 : log_replay(l);
	if (shared)
		flock(l->fd, LOCK_UN);
	l->shared = shared;
	l->loaded = !ret;
	return ret;
}

int log_sync(struct log *l)
{
	log_checkpoint(l);
	return fdatasync(l->fd) ? -errno : 0;
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
	int ret = log_hold(l);

	if (ret || (l->shared && xa_load(&l->map, chunk)))
		goto out;	/* failed, or another open of a shared log got it in first */
	h.epoch = l->epoch;
	ret = rec_write(l, &h, data);
	/* data is on its way to the file before anyone can find the record */
	if (!ret)
		ret = log_map(l, chunk, l->nrec);
	if (!ret)
		l->nrec++;
out:
	log_release(l);
	return ret;
}

int log_snapshot(struct log *l)
{
	int ret = log_hold(l);

	if (!ret)
		ret = ckpt_snapshot(l);
	log_release(l);
	return ret;
}

int log_read(struct log *l, uint64_t rec, uint8_t *buf, uint32_t within, uint32_t len)
{
	return full_pread(l->fd, buf, len, rec_off(l, rec) + HDR_SIZE + within);
}
