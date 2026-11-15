/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base cache: what a volume's chunks hold before the volume writes
 * them. That is the content of the base image, fetched chunk by chunk on
 * first use into a log next to the volume's (<log>.cache) and served from
 * there afterwards; zeros past the end of the base, and zeros everywhere
 * for a volume with no base.
 *
 * The base must not change. A base whose size or validator differs from
 * what the volume recorded is refused. If the volume was created with a
 * manifest of the base, every chunk fetched is also checked against it: one
 * that does not match is not stored, and the read fails with -EIO. One that cannot be reached at all
 * leaves the cache open offline: chunks already fetched read normally, the
 * others fail with the error that reaching the base gave.
 *
 * Any number of threads may read at once; a chunk that several miss on
 * together is fetched once.
 *
 * The cache file may be open in several processes at once, a volume's
 * writer and views of it: each stores what it fetches and finds what the
 * others stored, and the file holds every chunk once. It is a shared log
 * (log_load_shared in log.h): they take turns at the file, only while looking
 * at it or adding to it, never while fetching.
 */
#ifndef VBD_CACHE_H
#define VBD_CACHE_H

#include <stdint.h>

struct cache;
struct super;

/*
 * Create the cache file for the new volume at @log_path, whose sealed
 * super block is @sb. Only for a volume with a base. @manifest is the
 * base's manifest if the volume has one (super_set_manifest), else NULL; it
 * is stored next to the cache file.
 */
int cache_create(const char *log_path, const struct super *sb, const uint8_t *manifest);

/*
 * Open the cache of the volume at @log_path, whose super block is @sb.
 * With no base in @sb, no file is involved.
 * Returns NULL and sets errno; ESTALE for a base that changed.
 */
struct cache *cache_open(const char *log_path, const struct super *sb);
void cache_close(struct cache *c);	/* NULL is fine */

/* Read @len bytes of @chunk starting @within bytes into it. 0 or -errno. */
int cache_read(struct cache *c, uint64_t chunk, uint8_t *buf,
	       uint32_t within, uint32_t len);

/*
 * From now on, when a read has to fetch a chunk, go on in the background
 * and fetch up to @chunks of the ones that follow it, stopping short at one
 * the cache already has. Off until called: it fetches what nobody has asked
 * for yet. Does nothing without a reachable base. 0 or -errno.
 */
int cache_prefetch(struct cache *c, uint32_t chunks);

/* Distinct base chunks fetched so far. */
uint64_t cache_fetched(const struct cache *c);
/* 0 when the base is reachable (or there is none), else the negative errno. */
int cache_err(const struct cache *c);

#endif /* VBD_CACHE_H */
