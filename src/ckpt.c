// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A checkpoint is the whole map, sorted by chunk, cut into as many
 * consecutive records as it takes. The records are the size of any other
 * but have a header of their own kind. A checkpoint covers every record
 * before its part 0.
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
	uint64_t epoch;		/* the log's epoch when it was written */
	uint32_t part;		/* this record is part @part ... */
	uint32_t parts;		/* ... of @parts */
	uint64_t count;		/* map entries in the data: (chunk, record) as two uint64_t */
	uint8_t  pad[HDR_SIZE - 32];
};
_Static_assert(sizeof(struct rec_hdr) == HDR_SIZE && sizeof(struct ckpt_hdr) == HDR_SIZE,
	       "record headers");

/* Map entries that fit in the data of one record. */
static uint64_t per_record(const struct log *l)
{
	return l->chunk_size / (2 * sizeof(uint64_t));
}

/*
 * Append the map of @l to its own file. The caller holds l->append_lock,
 * which keeps the map still and the parts together.
 */
static int ckpt_write(struct log *l)
{
	uint64_t per = per_record(l), n = 0, first = l->nrec, *ent;
	struct ckpt_hdr h = { .magic = CKPT_MAGIC, .epoch = l->epoch };
	unsigned long chunk;
	void *e;
	int ret = 0;

	xa_for_each(&l->map, chunk, e)
		n++;
	if (!per || !n || (n + per - 1) / per > UINT32_MAX)
		return 0;	/* nothing to save, or records too small to hold it */
	h.parts = (n + per - 1) / per;
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
		if (++h.count < per && n)
			continue;
		/* the record is full, or that was the last entry */
		h.crc = crc32c(0, ent, l->chunk_size);
		ret = rec_write(l, &h, ent);
		l->nrec++;
		h.part++;
		h.count = 0;
		memset(ent, 0, l->chunk_size);
	}
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
 * The last complete checkpoint is found by walking the record headers back
 * from the end of the file; with none, the map stays empty for a replay
 * from the first record.
 */
void ckpt_load(struct log *l)
{
	struct ckpt_hdr h;
	struct stat st;
	uint64_t rec;

	if (fstat(l->fd, &st) || st.st_size < SUPER_SIZE)
		return;
	for (rec = (st.st_size - SUPER_SIZE) / (HDR_SIZE + l->chunk_size); rec-- > 0; ) {
		if (full_pread(l->fd, &h, sizeof(h), rec_off(l, rec)))
			return;
		/* only a last part can lead to a checkpoint that is all there */
		if (h.magic != CKPT_MAGIC || h.part + 1 != h.parts || h.part > rec)
			continue;
		if (ckpt_read(l, rec - h.part, h.parts)) {
			l->nrec = l->ckpt_nrec = rec + 1;
			l->ckpt_parts = h.parts;
			l->epoch = h.epoch;
			return;
		}
	}
}

/*
 * Due when enough has piled up to replay, and enough to make the
 * checkpoint a small addition: the log never shrinks, so every checkpoint
 * stays in it for good.
 */
void ckpt_save(struct log *l)
{
	uint64_t behind;

	pthread_mutex_lock(&l->append_lock);
	behind = l->nrec - l->ckpt_nrec;
	if (behind * (HDR_SIZE + l->chunk_size) >= CKPT_BYTES &&
	    behind >= 4 * (uint64_t)l->ckpt_parts)
		ckpt_write(l);
	pthread_mutex_unlock(&l->append_lock);
}
