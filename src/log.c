// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The record log. Format and rules in log.h. In this order: records, the
 * map, checkpoints, the life of a log, and what is done with an open one.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "crc32c.h"
#include "log.h"

/*
 * Records.
 */

static uint64_t rec_off(const struct log *l, uint64_t rec)
{
	return SUPER_SIZE + rec * (HDR_SIZE + (uint64_t)l->chunk_size);
}

/* Read the header of record @rec, of whatever kind, into HDR_SIZE bytes at @hdr. */
static int rec_read_hdr(struct log *l, uint64_t rec, void *hdr)
{
	return full_pread(l->fd, hdr, HDR_SIZE, rec_off(l, rec));
}

/*
 * Read the data of record @rec, whose header is @hdr. -EBADMSG if it is
 * not what the header's crc says: a record that was never written whole.
 */
static int rec_read_data(struct log *l, uint64_t rec, const void *hdr, void *data)
{
	const struct hdr_start *h = hdr;
	int ret = full_pread(l->fd, data, l->chunk_size, rec_off(l, rec) + HDR_SIZE);

	if (!ret && h->crc != crc32c(0, data, l->chunk_size))
		ret = -EBADMSG;
	return ret;
}

/*
 * Write a header of any kind and @data as record l->nrec, after putting
 * the data's crc in the header. The caller holds l->append_lock.
 */
static int rec_write(struct log *l, void *hdr, const void *data)
{
	struct iovec iov[2] = {
		{ .iov_base = hdr, .iov_len = HDR_SIZE },
		{ .iov_base = (void *)data, .iov_len = l->chunk_size },
	};
	ssize_t n;

	((struct hdr_start *)hdr)->crc = crc32c(0, data, l->chunk_size);
	n = pwritev(l->fd, iov, 2, rec_off(l, l->nrec));

	if (n == (ssize_t)(HDR_SIZE + l->chunk_size))
		return 0;
	return n < 0 ? -errno : -ENOSPC;	/* a short write is a full disk */
}

/*
 * The map.
 */

/* Point @chunk at record @rec. 0 or -errno. */
static int log_map(struct log *l, uint64_t chunk, uint64_t rec)
{
	void *old = xa_store(&l->map, chunk, xa_mk_value(rec), GFP_KERNEL);

	if (xa_is_err(old))
		return xa_err(old);
	if (!old)
		l->mapped++;
	return 0;
}

/*
 * Checkpoints. A checkpoint is the whole map, sorted by chunk, cut into as
 * many consecutive records as it takes. The records are the size of any
 * other but have a header of their own kind, struct ckpt_hdr. A checkpoint
 * covers every record before its part 0.
 *
 * Two things write one: enough records piling up behind the last, and a
 * snapshot. A snapshot's checkpoint also steps the epoch, so it is both
 * the record of that step and the map the snapshot froze.
 */

/* Map entries that fit in the data of one record. */
static uint64_t per_record(const struct log *l)
{
	return l->chunk_size / (2 * sizeof(uint64_t));
}

/* Append one part, the entries gathered in @ent, and get ready for the next. */
static int put_part(struct log *l, struct ckpt_hdr *h, uint64_t *ent)
{
	int ret;

	ret = rec_write(l, h, ent);
	if (ret)
		return ret;
	l->nrec++;
	h->part++;
	h->count = 0;
	memset(ent, 0, l->chunk_size);
	return 0;
}

/*
 * Append the map of @l to its own file; with @snap, as a snapshot, which
 * also steps the epoch. The caller holds l->append_lock, which keeps the
 * map still, the parts together and the step tied to them.
 */
static int ckpt_write(struct log *l, bool snap)
{
	uint64_t per = per_record(l), n = 0, first = l->nrec, *ent;
	struct ckpt_hdr h = { .magic = CKPT_MAGIC, .epoch = l->epoch,
			      .next_epoch = l->epoch + snap,
			      .time = snap ? time(NULL) : 0 };
	unsigned long chunk;
	void *e;
	int ret = 0;

	xa_for_each(&l->map, chunk, e)
		n++;
	if ((n + per - 1) / per > UINT32_MAX)
		return -EOVERFLOW;
	if (!n && !snap)
		return 0;		/* nothing to save */
	h.parts = n ? (n + per - 1) / per : 1;
	ent = calloc(1, l->chunk_size);
	if (!ent)
		return -ENOMEM;
	/* a checkpoint must not describe records a power cut can still take */
	if (fdatasync(l->fd))
		ret = -errno;
	xa_for_each(&l->map, chunk, e) {
		if (ret)
			break;
		ent[2 * h.count] = chunk;
		ent[2 * h.count + 1] = xa_to_value(e);
		n--;
		if (++h.count == per || !n)	/* the record is full, or that was the last entry */
			ret = put_part(l, &h, ent);
	}
	if (!ret && !h.part)
		ret = put_part(l, &h, ent);	/* a snapshot of nothing is still a snapshot */
	free(ent);
	if (ret) {
		/* half a checkpoint is no checkpoint: take it back */
		l->nrec = first;
		if (ftruncate(l->fd, rec_off(l, first)))
			ret = -errno;
		return ret;
	}
	l->ckpt_nrec = l->nrec;
	l->ckpt_parts = h.parts;
	l->epoch = h.next_epoch;
	return 0;
}

/* Fill the map from the checkpoint in records [first, first + parts). */
static bool ckpt_read(struct log *l, uint64_t first, uint32_t parts)
{
	uint64_t per = per_record(l);
	uint64_t *ent = malloc(l->chunk_size);
	bool ok = ent != NULL;

	for (uint32_t p = 0; ok && p < parts; p++) {
		struct ckpt_hdr h;

		ok = !rec_read_hdr(l, first + p, &h) && h.magic == CKPT_MAGIC &&
		     h.part == p && h.parts == parts && h.count <= per &&
		     !rec_read_data(l, first + p, &h, ent);
		for (uint64_t i = 0; ok && i < h.count; i++)
			ok = ent[2 * i] < l->nchunks && ent[2 * i + 1] < first &&
			     !log_map(l, ent[2 * i], ent[2 * i + 1]);
	}
	free(ent);
	if (!ok) {
		xa_destroy(&l->map);	/* empty again */
		l->mapped = 0;
	}
	return ok;
}

/*
 * Start @l from the last complete checkpoint that holds nothing newer than
 * l->upto, if there is one, before the replay. The records a checkpoint
 * covers are not read again, so their checksums are not re-verified: that
 * is the time it saves.
 *
 * Returns true when no replay is needed at all: @l is a view of a snapshot
 * and the checkpoint found is the one that snapshot wrote.
 *
 * The checkpoint is found by walking the record headers back from the end
 * of the file; with none, the map stays empty for a replay from the first
 * record. Epochs never decrease along a log, so the first one that fits
 * l->upto is the newest that does.
 *
 * ponytail: the walk reads one header per record written since the
 * checkpoint, which for a view of an old snapshot is everything newer.
 * Epochs being sorted, a binary search would find it in a few reads.
 */
static bool ckpt_load(struct log *l)
{
	uint64_t rec, newest = 0;	/* the largest epoch any header after the checkpoint names */
	struct ckpt_hdr h;
	struct stat st;

	if (fstat(l->fd, &st) || st.st_size < SUPER_SIZE)
		return false;
	for (rec = (st.st_size - SUPER_SIZE) / (HDR_SIZE + l->chunk_size); rec-- > 0; ) {
		uint64_t epoch;

		if (rec_read_hdr(l, rec, &h))
			return false;
		if (h.magic == VBD_MAGIC)
			epoch = ((struct rec_hdr *)&h)->epoch;
		else if (h.magic == CKPT_MAGIC)
			epoch = h.next_epoch;
		else
			continue;
		if (epoch > newest)
			newest = epoch;
		/* only a last part can lead to a checkpoint that is all there */
		if (h.magic != CKPT_MAGIC || h.part + 1 != h.parts || h.part > rec ||
		    h.epoch > l->upto)
			continue;
		if (!ckpt_read(l, rec - h.part, h.parts))
			continue;
		l->nrec = l->ckpt_nrec = rec + 1;
		l->ckpt_parts = h.parts;
		l->epoch = h.next_epoch;
		if (h.epoch != l->upto || h.next_epoch == h.epoch)
			return false;	/* the records after it may still matter */
		/*
		 * The checkpoint snapshot l->upto wrote: the map is that
		 * snapshot, and everything after it is newer. What remains to
		 * know is how far the log has got since.
		 */
		l->epoch = newest;
		return true;
	}
	return false;
}

/*
 * Due when enough has piled up to replay, and enough to make the
 * checkpoint a small addition: the log never shrinks, so every checkpoint
 * stays in it for good.
 */
static void ckpt_save(struct log *l)
{
	uint64_t behind = l->nrec - l->ckpt_nrec;

	if (behind * (HDR_SIZE + l->chunk_size) >= CKPT_BYTES &&
	    behind >= 4 * (uint64_t)l->ckpt_parts)
		ckpt_write(l, false);
}

/*
 * The life of a log.
 */

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

int log_open(struct log *l, const char *path, void *super)
{
	l->fd = open(path, O_RDWR);
	if (l->fd < 0)
		return -errno;
	return full_pread(l->fd, super, SUPER_SIZE, 0);
}

int log_lock(struct log *l)
{
	if (!flock(l->fd, LOCK_EX | LOCK_NB))
		return 0;
	return errno == EWOULDBLOCK ? -EBUSY : -errno;
}

/*
 * Bring the map up to date with the file, from record l->nrec on: 0, just
 * past the checkpoint the map was taken from, or where the file ended when
 * we last looked.
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

		if ((ret = rec_read_hdr(l, l->nrec, &h)))
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
		if (h.magic != VBD_MAGIC || h.chunk >= l->nchunks)
			break;		/* torn tail */
		ret = rec_read_data(l, l->nrec, &h, data);
		if (ret == -EBADMSG) {
			ret = 0;
			break;		/* torn tail */
		}
		if (ret)
			goto out;
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

int log_snapshot_before(struct log *l, uint32_t chunk_size, int64_t when,
			uint64_t *snapshot)
{
	struct ckpt_hdr h;
	struct stat st;
	uint64_t rec;
	int ret;

	l->chunk_size = chunk_size;	/* where the records lie */
	if (fstat(l->fd, &st))
		return -errno;
	if (st.st_size < SUPER_SIZE)
		return -ENOENT;
	/* back from the end: the first snapshot old enough is the newest one that is */
	for (rec = (st.st_size - SUPER_SIZE) / (HDR_SIZE + chunk_size); rec-- > 0; ) {
		if ((ret = rec_read_hdr(l, rec, &h)))
			return ret;
		if (h.magic == CKPT_MAGIC && h.next_epoch == h.epoch + 1 &&
		    h.part + 1 == h.parts && (int64_t)h.time <= when) {
			*snapshot = h.epoch;
			return 0;
		}
	}
	return -ENOENT;
}

int log_load(struct log *l, uint32_t chunk_size, uint64_t nchunks, uint64_t upto)
{
	int ret;

	l->chunk_size = chunk_size;
	l->nchunks = nchunks;
	l->upto = upto;
	ret = ckpt_load(l) ? 0 : log_replay(l);
	l->loaded = !ret;
	return ret;
}

int log_catch_up(struct log *l)
{
	int ret;

	if (!l->loaded)
		return -EINVAL;
	pthread_mutex_lock(&l->append_lock);
	ret = log_replay(l);
	if (ret)
		l->loaded = false;	/* the map is no longer the file */
	pthread_mutex_unlock(&l->append_lock);
	return ret;
}

/* A view's map is not the whole log's, and a log that failed to load has none. */
static bool takes_checkpoints(const struct log *l)
{
	return l->loaded && l->upto == ALL_EPOCHS;
}

/* Append a checkpoint if one is due. Best effort. */
static void log_checkpoint(struct log *l)
{
	if (!takes_checkpoints(l))
		return;
	pthread_mutex_lock(&l->append_lock);
	ckpt_save(l);
	pthread_mutex_unlock(&l->append_lock);
}

int log_sync(struct log *l)
{
	log_checkpoint(l);
	return fdatasync(l->fd) ? -errno : 0;
}

void log_close(struct log *l)
{
	log_checkpoint(l);
	xa_destroy(&l->map);
	if (l->fd >= 0)
		close(l->fd);
	l->fd = -1;
}

/*
 * An open log.
 */

int log_append(struct log *l, uint64_t chunk, const uint8_t *data)
{
	struct rec_hdr h = { .magic = VBD_MAGIC, .chunk = chunk };
	int ret;

	pthread_mutex_lock(&l->append_lock);
	h.epoch = l->epoch;
	ret = rec_write(l, &h, data);
	/* data is on its way to the file before anyone can find the record */
	if (!ret)
		ret = log_map(l, chunk, l->nrec);
	if (!ret)
		l->nrec++;
	pthread_mutex_unlock(&l->append_lock);
	return ret;
}

int log_snapshot(struct log *l)
{
	int ret;

	pthread_mutex_lock(&l->append_lock);
	ret = ckpt_write(l, true);	/* the checkpoint is the snapshot */
	pthread_mutex_unlock(&l->append_lock);
	return ret;
}

bool log_has(const struct log *l, uint64_t chunk)
{
	return xa_load((struct xarray *)&l->map, chunk) != NULL;
}

uint64_t log_first(const struct log *l, uint64_t from, uint64_t end)
{
	unsigned long at = from;

	if (from >= end)
		return end;
	return xa_find((struct xarray *)&l->map, &at, end - 1, XA_PRESENT) ? at : end;
}

int log_read(struct log *l, uint64_t chunk, uint8_t *buf, uint32_t within, uint32_t len)
{
	void *e = xa_load(&l->map, chunk);

	if (!e)
		return -ENOENT;
	return full_pread(l->fd, buf, len, rec_off(l, xa_to_value(e)) + HDR_SIZE + within);
}

uint64_t log_chunks(const struct log *l)	{ return l->mapped; }
uint64_t log_epoch(const struct log *l)		{ return l->epoch; }
int log_fd(const struct log *l)			{ return l->fd; }
