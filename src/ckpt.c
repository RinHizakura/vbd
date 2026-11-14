// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A checkpoint is the whole map, sorted by chunk, cut into as many
 * consecutive records as it takes. The records are the size of any other
 * but have a header of their own kind. A checkpoint covers every record
 * before its part 0.
 *
 * Two things write one: enough records piling up behind the last, and a
 * snapshot. A snapshot's checkpoint also steps the epoch, so it is both
 * the record of that step and the map the snapshot froze.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ckpt.h"
#include "crc32c.h"

/*
 * Checkpoint once this many bytes of log would have to be replayed. Replay
 * reads and checksums every byte: about 540 MiB/s from the page cache on
 * the machine this was measured on, whatever the chunk size, so this bounds
 * an open after a crash to roughly a tenth of a second there.
 */
#define CKPT_BYTES	(64u << 20)

struct ckpt_hdr {
	uint32_t magic;		/* CKPT_MAGIC */
	uint32_t crc;		/* crc32c of the data that follows */
	uint64_t epoch;		/* no record it covers has a larger one */
	uint32_t part;		/* this record is part @part ... */
	uint32_t parts;		/* ... of @parts */
	uint64_t count;		/* map entries in the data: (chunk, record) as two uint64_t */
	/*
	 * What records after it are stamped with: @epoch, or @epoch + 1 when
	 * the checkpoint is a snapshot, the one numbered @epoch.
	 */
	uint64_t next_epoch;
	uint8_t  pad[HDR_SIZE - 40];
};
_Static_assert(sizeof(struct rec_hdr) == HDR_SIZE && sizeof(struct ckpt_hdr) == HDR_SIZE,
	       "record headers");

/* Map entries that fit in the data of one record. */
static uint64_t per_record(const struct log *l)
{
	return l->chunk_size / (2 * sizeof(uint64_t));
}

/* Append one part, the entries gathered in @ent, and get ready for the next. */
static int put_part(struct log *l, struct ckpt_hdr *h, uint64_t *ent)
{
	int ret;

	h->crc = crc32c(0, ent, l->chunk_size);
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
			      .next_epoch = l->epoch + snap };
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
		uint64_t off = rec_off(l, first + p);
		struct ckpt_hdr h;

		ok = !full_pread(l->fd, &h, sizeof(h), off) &&
		     !full_pread(l->fd, ent, l->chunk_size, off + HDR_SIZE) &&
		     h.magic == CKPT_MAGIC &&
		     h.part == p && h.parts == parts && h.count <= per &&
		     h.crc == crc32c(0, ent, l->chunk_size);
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
 * The checkpoint is found by walking the record headers back from the end
 * of the file; with none, the map stays empty for a replay from the first
 * record. Epochs never decrease along a log, so the first one that fits
 * l->upto is the newest that does.
 *
 * ponytail: the walk reads one header per record written since the
 * checkpoint, which for a view of an old snapshot is everything newer.
 * Epochs being sorted, a binary search would find it in a few reads.
 */
bool ckpt_load(struct log *l)
{
	uint64_t rec, newest = 0;	/* the largest epoch any header after the checkpoint names */
	struct ckpt_hdr h;
	struct stat st;

	if (fstat(l->fd, &st) || st.st_size < SUPER_SIZE)
		return false;
	for (rec = (st.st_size - SUPER_SIZE) / (HDR_SIZE + l->chunk_size); rec-- > 0; ) {
		uint64_t epoch;

		if (full_pread(l->fd, &h, sizeof(h), rec_off(l, rec)))
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
void ckpt_save(struct log *l)
{
	uint64_t behind = l->nrec - l->ckpt_nrec;

	if (behind * (HDR_SIZE + l->chunk_size) >= CKPT_BYTES &&
	    behind >= 4 * (uint64_t)l->ckpt_parts)
		ckpt_write(l, false);
}

int ckpt_snapshot(struct log *l)
{
	return ckpt_write(l, true);
}
