/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Checkpoints: now and then a log's map is appended to the log itself, so
 * that a load can take the map from there and replay only what follows.
 * The log stays one file, and append stays the only way it is written.
 *
 * This is the inside of the log: only log.c calls it, with l->append_lock
 * held around ckpt_save and ckpt_snapshot.
 */
#ifndef VBD_CKPT_H
#define VBD_CKPT_H

#include <stdbool.h>

#include "log.h"

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

/*
 * Start @l from the last complete checkpoint that holds nothing newer than
 * l->upto, if there is one, before the replay. The records a checkpoint
 * covers are not read again, so their checksums are not re-verified: that
 * is the time it saves.
 *
 * Returns true when no replay is needed at all: @l is a view of a snapshot
 * and the checkpoint found is the one that snapshot wrote.
 */
bool ckpt_load(struct log *l);

/*
 * Append a checkpoint to @l if enough has piled up since the last one.
 * Best effort; replay covers for a missing one.
 */
void ckpt_save(struct log *l);

/*
 * Take a snapshot: append a checkpoint of the map as it is and step the
 * epoch, as one. The checkpoint is what makes the step last, and what a
 * view of the snapshot is later loaded from.
 */
int ckpt_snapshot(struct log *l);

#endif /* VBD_CKPT_H */
