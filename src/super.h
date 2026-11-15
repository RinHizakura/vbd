/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The super block: what a volume is, written once in front of its log when
 * it is created and never changed. Its size and chunk size, and what it
 * falls back to where it was never written.
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
	uint8_t  sums_hash[SHA256_LEN];	/* sha256 of the base's manifest; all zero: it has none */
	char     base_tag[BASE_TAG_LEN];	/* base validator at create (base_ops.probe) */
	char     base[SUPER_SIZE - 32 - SHA256_LEN - BASE_TAG_LEN];	/* path or URL; empty: zeros */
};
_Static_assert(sizeof(struct super) == SUPER_SIZE, "super block");

/*
 * Start the super block of a new volume: @size bytes in chunks of
 * @chunk_size, no base. -EINVAL for a geometry a volume cannot have. The
 * caller fills in the base, if any, and then seals it.
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
/*
 * A manifest of the base is the sha256 of each of its chunks, the last one
 * padded with zeros, SHA256_LEN bytes each. This is how long that is for
 * the base and chunk size in @sb.
 */
size_t super_sums_len(const struct super *sb);
/*
 * Give the new volume the manifest in the file at @path: read it and note
 * its own sha256, so that the copy kept with the volume can be trusted
 * later. Call after super_set_base. Hands the manifest back in *@sums for
 * the caller to store and free. -EINVAL if it is not a manifest of this
 * base at this chunk size.
 */
int super_set_sums(struct super *sb, const char *path, uint8_t **sums);
/* Was the volume created with a manifest? */
bool super_has_sums(const struct super *sb);
/* Is @sums, super_sums_len(@sb) bytes, the manifest the volume was created with? */
bool super_check_sums(const struct super *sb, const uint8_t *sums);
/* Checksum @sb; it must not change afterwards. */
void super_seal(struct super *sb);
/* Sealed, undamaged, and everything super_init insists on? */
bool super_valid(const struct super *sb);

#endif /* VBD_SUPER_H */
