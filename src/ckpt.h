/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Checkpoints: now and then a log's map is appended to the log itself, so
 * that open can take the map from there and replay only what follows. The
 * log stays one file, and append stays the only way it is written.
 */
#ifndef VBD_CKPT_H
#define VBD_CKPT_H

#include "log.h"

/*
 * Start @l from its last complete checkpoint, if it has one; call before
 * log_replay. The records a checkpoint covers are not read again, so their
 * checksums are not re-verified: that is the time it saves.
 */
void ckpt_load(struct log *l);

/*
 * Append a checkpoint to @l if enough has piled up since the last one.
 * Best effort; replay covers for a missing one.
 */
void ckpt_save(struct log *l);

#endif /* VBD_CKPT_H */
