// SPDX-License-Identifier: GPL-2.0-or-later
/* The base cache: a log of fetched base chunks in front of a base backend. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "log.h"
#include "base/base.h"

#define CACHE_SUFFIX	".cache"

struct cache {
	uint32_t chunk_size;
	const struct base_ops *ops;	/* NULL: no base, everything reads as zeros */
	void *base;		/* NULL while ops is set: base unreachable, offline */
	int err;		/* why the base is unreachable, 0 when it is open */
	uint64_t base_size;	/* bytes the base actually has; the rest reads as zeros */
	struct log log;		/* base chunks fetched so far */
	struct xarray fetching;	/* chunks being fetched from the base right now */
	pthread_mutex_t fetch_lock;	/* guards fetching */
	pthread_cond_t fetch_done;	/* a fetch finished, fetching may have changed */
};

/* What cache.fetching holds for a chunk; only its presence matters. */
#define FETCHING	xa_mk_value(1)

static char *cache_path(const char *log_path)
{
	char *p = malloc(strlen(log_path) + sizeof(CACHE_SUFFIX));

	if (p)
		strcat(strcpy(p, log_path), CACHE_SUFFIX);
	return p;
}

int cache_create(const char *log_path, const void *super)
{
	char *path = cache_path(log_path);
	int ret = path ? log_create(path, super) : -ENOMEM;

	free(path);
	return ret;
}

/*
 * Bind the base. One that cannot be reached leaves the cache offline:
 * fetched chunks still read, a miss returns the error seen here. One that
 * changed is refused with -ESTALE, since what was fetched may then be wrong.
 */
static int bind_base(struct cache *c, const char *base, uint64_t base_size,
		     const char *base_tag)
{
	/* the name came out of a super block, so its stored form fits in one */
	char tag[BASE_TAG_LEN], stored[SUPER_SIZE];
	uint64_t size;
	int ret;

	c->ops = base_ops_for(base);
	c->base_size = base_size;
	ret = c->ops->probe(base, stored, sizeof(stored), &size, tag, sizeof(tag));
	if (ret) {
		c->err = ret;
		return 0;
	}
	if (size != base_size || strcmp(tag, base_tag))
		return -ESTALE;
	c->base = c->ops->open(base);
	if (!c->base)
		c->err = -errno;
	return 0;
}

struct cache *cache_open(const char *log_path, const void *super,
			 const char *base, uint64_t base_size, const char *base_tag,
			 uint32_t chunk_size, uint64_t nchunks)
{
	struct cache *c = calloc(1, sizeof(*c));
	uint8_t *mine = NULL;
	char *path = NULL;
	int err = 0;

	if (!c)
		return NULL;
	c->chunk_size = chunk_size;
	log_init(&c->log);
	xa_init(&c->fetching);
	pthread_mutex_init(&c->fetch_lock, NULL);
	pthread_cond_init(&c->fetch_done, NULL);
	if (!base[0])
		return c;

	if ((err = bind_base(c, base, base_size, base_tag)))
		goto fail;
	path = cache_path(log_path);
	mine = malloc(SUPER_SIZE);
	if (!path || !mine) {
		err = -ENOMEM;
		goto fail;
	}
	if ((err = log_open(&c->log, path, mine)))
		goto fail;
	if (memcmp(mine, super, SUPER_SIZE)) {
		err = -EINVAL;	/* not this volume's cache */
		goto fail;
	}
	err = log_load(&c->log, chunk_size, nchunks, ALL_EPOCHS);
fail:
	free(path);
	free(mine);
	if (err) {
		cache_close(c);
		errno = -err;
		return NULL;
	}
	return c;
}

void cache_close(struct cache *c)
{
	if (!c)
		return;
	log_close(&c->log);
	xa_destroy(&c->fetching);
	if (c->base)
		c->ops->close(c->base);
	pthread_mutex_destroy(&c->fetch_lock);
	pthread_cond_destroy(&c->fetch_done);
	free(c);
}

uint64_t cache_fetched(const struct cache *c)	{ return c->log.mapped; }
int cache_err(const struct cache *c)		{ return c->err; }

/*
 * Bring @chunk of the base into the cache and copy @len bytes of it, from
 * @within, into @buf. Only one thread fetches a given chunk: it enters the
 * chunk in c->fetching first, and later arrivals wait for it to leave, then
 * find the chunk in the log (and read it) or not (and fetch).
 */
static int fetch(struct cache *c, uint64_t chunk, uint8_t *buf,
		 uint32_t within, uint32_t len)
{
	uint8_t *tmp;
	void *e;
	int ret = 0;

	if (!c->base)
		return c->err;	/* offline: nothing to fetch from */

	pthread_mutex_lock(&c->fetch_lock);
	while (xa_load(&c->fetching, chunk))
		pthread_cond_wait(&c->fetch_done, &c->fetch_lock);
	e = xa_load(&c->log.map, chunk);
	if (!e)
		ret = xa_err(xa_store(&c->fetching, chunk, FETCHING, GFP_KERNEL));
	pthread_mutex_unlock(&c->fetch_lock);
	if (e)
		return log_read(&c->log, xa_to_value(e), buf, within, len);
	if (ret)
		return ret;

	tmp = malloc(c->chunk_size);
	ret = tmp ? c->ops->read(c->base, chunk * c->chunk_size, tmp, c->chunk_size)
		  : -ENOMEM;
	if (!ret)
		ret = log_append(&c->log, chunk, tmp);
	if (!ret)
		memcpy(buf, tmp + within, len);
	free(tmp);

	pthread_mutex_lock(&c->fetch_lock);
	xa_erase(&c->fetching, chunk);	/* in the log now, or the next reader tries */
	pthread_cond_broadcast(&c->fetch_done);
	pthread_mutex_unlock(&c->fetch_lock);
	return ret;
}

int cache_read(struct cache *c, uint64_t chunk, uint8_t *buf,
	       uint32_t within, uint32_t len)
{
	void *e;

	if (!c->ops || chunk * c->chunk_size >= c->base_size) {
		memset(buf, 0, len);	/* no base, or past its end: nothing to fetch */
		return 0;
	}
	e = xa_load(&c->log.map, chunk);
	if (e)
		return log_read(&c->log, xa_to_value(e), buf, within, len);
	return fetch(c, chunk, buf, within, len);
}
