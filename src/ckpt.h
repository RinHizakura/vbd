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
