/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The super block: what a volume is, written once in front of its log when
 * it is created and never changed. Its size and chunk size, and what it
 * falls back to where it was never written: a base image, the volume it
 * was forked from, or nothing.
 */
#ifndef VBD_SUPER_H
#define VBD_SUPER_H

#include <stdbool.h>
#include <stdint.h>

#include "log.h"
#include "sha256.h"
#include "base/base.h"

struct super {
	uint32_t crc;		/* crc32c of everything after this field */
	uint32_t magic;
	uint32_t version;
	uint32_t chunk_size;
	uint64_t size;
	uint64_t base_size;	/* size of the base at create */
	uint64_t parent_at;	/* a fork: the parent's snapshot it continues from; else NO_PARENT */
	uint8_t  manifest_hash[SHA256_LEN];	/* manifest_hash of the base's manifest; all zero: it has none */
	char     base_tag[BASE_TAG_LEN];	/* base validator at create (base_ops.probe) */
	/*
	 * The base image, as a path or URL, or for a fork the path of the
	 * parent's log. Empty: neither. Use the functions below to tell.
	 */
	char     base[SUPER_SIZE - 40 - SHA256_LEN - BASE_TAG_LEN];
};
#define NO_PARENT	UINT64_MAX
_Static_assert(sizeof(struct super) == SUPER_SIZE, "super block");

/*
 * Start the super block of a new volume: @size bytes in chunks of
 * @chunk_size, no base. -EINVAL for a geometry a volume cannot have. The
 * caller fills in the base or the parent, if any, and then seals it.
 */
int super_init(struct super *sb, uint64_t size, uint32_t chunk_size);
/*
 * Give the new volume the base image @base: look at it and note what will
 * identify it later, its name in a form that stays valid, its size and the
 * backend's validator. 0 or -errno; -ENOENT and the like if it is not there.
 */
int super_set_base(struct super *sb, const char *base);
/*
 * Look at the base again: 0 if it is still what super_set_base saw,
 * -ESTALE if its size or validator changed, another -errno if it cannot be
 * reached. @sb must have a base.
 */
int super_check_base(const struct super *sb);
/* Does the volume have a base image? */
bool super_has_base(const struct super *sb);

/*
 * Make the new volume a fork: it continues from the volume whose log is at
 * @parent_log, as that was at @snapshot. Instead of super_set_base; a fork
 * has no base of its own. The path is noted in absolute form, so the
 * parent must stay where it is. 0 or -errno.
 */
int super_set_parent(struct super *sb, const char *parent_log, uint64_t snapshot);
/* The log of the volume this one was forked from, or NULL if it is not a fork. */
const char *super_parent(const struct super *sb);

/* Was the volume created with a manifest? */
bool super_has_manifest(const struct super *sb);
/* Checksum @sb; it must not change afterwards. */
void super_seal(struct super *sb);
/* Sealed, undamaged, and everything super_init insists on? */
bool super_valid(const struct super *sb);

#endif /* VBD_SUPER_H */
