/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The manifest of a base: the sha256 of each of its chunks, the last one
 * padded with zeros, SHA256_LEN bytes each and nothing else. vbd_manifest
 * (vbd.h) makes one from an image.
 *
 * A volume created with one keeps a copy next to its log, <log>.manifest,
 * and the sha256 of that copy in its super block, so that the copy can be
 * trusted later.
 */
#ifndef VBD_MANIFEST_H
#define VBD_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#include "sha256.h"

struct manifest;

/*
 * Read the manifest in the file at @path, of a base of @base_size bytes in
 * chunks of @chunk_size. Returns NULL and sets errno; EINVAL if it is not a
 * manifest of such a base.
 */
struct manifest *manifest_read(const char *path, uint64_t base_size, uint32_t chunk_size);

/*
 * The same, of the copy kept with the volume at @log_path; EINVAL too if
 * its sha256 is not @hash.
 */
struct manifest *manifest_open(const char *log_path, uint64_t base_size, uint32_t chunk_size,
			       const uint8_t hash[SHA256_LEN]);
void manifest_close(struct manifest *m);	/* NULL is fine */

/* The sha256 of the manifest itself. */
void manifest_hash(const struct manifest *m, uint8_t hash[SHA256_LEN]);

/* Keep a copy with the new volume at @log_path. 0 or -errno, and then no file is left. */
int manifest_save(const struct manifest *m, const char *log_path);

/*
 * Is @data, a chunk's worth, what the manifest says @chunk of the base
 * holds? True with no manifest, @m NULL.
 */
bool manifest_check(const struct manifest *m, uint64_t chunk, const uint8_t *data);

#endif /* VBD_MANIFEST_H */
