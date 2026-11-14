/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base cache: what a volume's chunks hold before the volume writes
 * them. That is the content of the base image, fetched chunk by chunk on
 * first use into a log next to the volume's (<log>.cache) and served from
 * there afterwards; zeros past the end of the base, and zeros everywhere
 * for a volume with no base.
 *
 * The base must not change. A base whose size or validator differs from
 * what the volume recorded is refused. One that cannot be reached at all
 * leaves the cache open offline: chunks already fetched read normally, the
 * others fail with the error that reaching the base gave.
 *
 * Any number of threads may read at once; a chunk that several miss on
 * together is fetched once.
 *
 * The cache file may be open in several processes at once, a volume's
 * writer and views of it: each stores what it fetches and finds what the
 * others stored, and the file holds every chunk once.
 */
#ifndef VBD_CACHE_H
#define VBD_CACHE_H

#include <stdint.h>

struct cache;

/* Create the cache file for the volume at @log_path, behind a copy of its super block. */
int cache_create(const char *log_path, const void *super);

/*
 * Open the cache of the volume at @log_path. @base, @base_size and
 * @base_tag are what the volume's super block says about its base; an empty
 * @base means there is none, and then no file is involved. @super is that
 * super block, which the cache file must start with too.
 * Returns NULL and sets errno; ESTALE for a base that changed.
 */
struct cache *cache_open(const char *log_path, const void *super,
			 const char *base, uint64_t base_size, const char *base_tag,
			 uint32_t chunk_size, uint64_t nchunks);
void cache_close(struct cache *c);	/* NULL is fine */

/* Read @len bytes of @chunk starting @within bytes into it. 0 or -errno. */
int cache_read(struct cache *c, uint64_t chunk, uint8_t *buf,
	       uint32_t within, uint32_t len);

/* Distinct base chunks fetched so far. */
uint64_t cache_fetched(const struct cache *c);
/* 0 when the base is reachable (or there is none), else the negative errno. */
int cache_err(const struct cache *c);

#endif /* VBD_CACHE_H */
