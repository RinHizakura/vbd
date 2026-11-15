// SPDX-License-Identifier: GPL-2.0-or-later
/* The base cache: a log of fetched base chunks in front of a base backend. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <urcu.h>
#include <xarray.h>

#include "common.h"
#include "cache.h"
#include "log.h"
#include "sha256.h"
#include "super.h"
#include "base/base.h"

#define CACHE_SUFFIX	".cache"
#define MANIFEST_SUFFIX	".manifest"		/* the base's manifest, if the volume has one */

struct cache {
	uint32_t chunk_size;
	const struct base_ops *ops;	/* NULL: no base, everything reads as zeros */
	void *base;		/* NULL while ops is set: base unreachable, offline */
	int err;		/* why the base is unreachable, 0 when it is open */
	uint64_t base_size;	/* bytes the base actually has; the rest reads as zeros */
	uint8_t *manifest;		/* sha256 of each base chunk, or NULL: fetches are not checked */
	struct log log;		/* base chunks fetched so far; shared with the volume's other opens */
	struct xarray fetching;	/* chunks being fetched from the base right now */
	pthread_mutex_t fetch_lock;	/* guards fetching and the pf_ fields */
	pthread_cond_t fetch_done;	/* a fetch finished, fetching may have changed */
	uint32_t pf_window;	/* chunks to fetch ahead of a miss; 0: no prefetcher */
	uint64_t pf_next;	/* where the prefetcher starts next, or PF_IDLE */
	bool pf_stop;
	pthread_t pf_thread;
	pthread_cond_t pf_wake;	/* pf_next or pf_stop changed */
};

/* cache.pf_next when the prefetcher has nothing to do */
#define PF_IDLE		UINT64_MAX

/* What cache.fetching holds for a chunk; only its presence matters. */
#define FETCHING	xa_mk_value(1)

/* <log_path><suffix>; the caller frees it. */
static char *side_path(const char *log_path, const char *suffix)
{
	char *p = malloc(strlen(log_path) + strlen(suffix) + 1);

	if (p)
		strcat(strcpy(p, log_path), suffix);
	return p;
}

int cache_create(const char *log_path, const struct super *sb, const uint8_t *manifest)
{
	char *path = side_path(log_path, CACHE_SUFFIX);
	char *manifest_path = side_path(log_path, MANIFEST_SUFFIX);
	ssize_t len = super_manifest_len(sb);
	int fd, ret = -ENOMEM;

	if (!path || !manifest_path || (ret = log_create(path, sb)) || !manifest)
		goto out;
	fd = open(manifest_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		ret = -errno;
	} else {
		if (pwrite(fd, manifest, len, 0) != len || fsync(fd))
			ret = errno ? -errno : -EIO;
		close(fd);
		if (ret)
			unlink(manifest_path);
	}
	if (ret)
		unlink(path);	/* no half-made cache left behind */
out:
	free(path);
	free(manifest_path);
	return ret;
}

/* Load the manifest kept with the volume; it must be the one the super block pins. */
static int load_manifest(struct cache *c, const char *log_path, const struct super *sb)
{
	char *path = side_path(log_path, MANIFEST_SUFFIX);
	int ret = -ENOMEM;

	c->manifest = malloc(super_manifest_len(sb) + 1);
	if (path && c->manifest)
		ret = read_exact(path, c->manifest, super_manifest_len(sb));
	if (!ret && !super_check_manifest(sb, c->manifest))
		ret = -EINVAL;
	free(path);
	return ret;
}

/*
 * Bind the base. One that cannot be reached leaves the cache offline:
 * fetched chunks still read, a miss returns the error seen here. One that
 * changed is refused with -ESTALE, since what was fetched may then be wrong.
 */
static int bind_base(struct cache *c, const struct super *sb)
{
	int ret = super_check_base(sb);

	c->ops = base_ops_for(sb->base);
	c->base_size = sb->base_size;
	if (ret == -ESTALE)
		return ret;
	if (ret) {
		c->err = ret;
		return 0;
	}
	c->base = c->ops->open(sb->base);
	if (!c->base)
		c->err = -errno;
	return 0;
}

struct cache *cache_open(const char *log_path, const struct super *sb)
{
	struct cache *c = calloc(1, sizeof(*c));
	struct super *mine = NULL;
	char *path = NULL;
	int err = 0;

	if (!c)
		return NULL;
	c->chunk_size = sb->chunk_size;
	log_init(&c->log);
	xa_init(&c->fetching);
	pthread_mutex_init(&c->fetch_lock, NULL);
	pthread_cond_init(&c->fetch_done, NULL);
	pthread_cond_init(&c->pf_wake, NULL);
	if (!super_has_base(sb))
		return c;

	if ((err = bind_base(c, sb)))
		goto fail;
	if (super_has_manifest(sb) && (err = load_manifest(c, log_path, sb)))
		goto fail;
	path = side_path(log_path, CACHE_SUFFIX);
	mine = malloc(sizeof(*mine));
	if (!path || !mine) {
		err = -ENOMEM;
		goto fail;
	}
	if ((err = log_open(&c->log, path, mine)))
		goto fail;
	if (memcmp(mine, sb, sizeof(*sb))) {
		err = -EINVAL;	/* not this volume's cache */
		goto fail;
	}
	err = log_load_shared(&c->log, sb->chunk_size, sb->size / sb->chunk_size);
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
	if (c->pf_window) {
		pthread_mutex_lock(&c->fetch_lock);
		c->pf_stop = true;
		pthread_cond_signal(&c->pf_wake);
		pthread_mutex_unlock(&c->fetch_lock);
		pthread_join(c->pf_thread, NULL);
	}
	log_close(&c->log);
	xa_destroy(&c->fetching);
	if (c->base)
		c->ops->close(c->base);
	free(c->manifest);
	pthread_mutex_destroy(&c->fetch_lock);
	pthread_cond_destroy(&c->fetch_done);
	pthread_cond_destroy(&c->pf_wake);
	free(c);
}

uint64_t cache_fetched(const struct cache *c)	{ return log_chunks(&c->log); }
int cache_err(const struct cache *c)		{ return c->err; }

/* Is @data what the manifest says @chunk of the base holds? True with no manifest. */
static bool verified(const struct cache *c, uint64_t chunk, const uint8_t *data)
{
	uint8_t hash[SHA256_LEN];

	if (!c->manifest)
		return true;
	sha256(data, c->chunk_size, hash);
	return !memcmp(hash, c->manifest + chunk * SHA256_LEN, SHA256_LEN);
}

/* Put a fetched chunk in the cache file, unless another open of it has by now. */
static int store(struct cache *c, uint64_t chunk, const uint8_t *data)
{
	int ret = log_append_if(&c->log, chunk, data, LOG_ABSENT);

	return ret == -EAGAIN ? 0 : ret;
}

/*
 * Bring @chunk of the base into the cache and copy @len bytes of it, from
 * @within, into @buf. Only one thread fetches a given chunk: it enters the
 * chunk in c->fetching first, and later arrivals wait for it to leave, then
 * find the chunk in the log (and read it) or not (and fetch).
 *
 * Another process may have fetched the chunk since this one last looked at
 * the file, so it looks again before going to the base. Two processes that
 * miss at the same moment both fetch; the file keeps the first to arrive.
 */
static int fetch(struct cache *c, uint64_t chunk, uint8_t *buf,
		 uint32_t within, uint32_t len)
{
	uint8_t *tmp = NULL;
	bool have;
	int ret = 0;

	pthread_mutex_lock(&c->fetch_lock);
	while (xa_load(&c->fetching, chunk))
		pthread_cond_wait(&c->fetch_done, &c->fetch_lock);
	have = log_has(&c->log, chunk);
	if (!have)
		ret = xa_err(xa_store(&c->fetching, chunk, FETCHING, GFP_KERNEL));
	pthread_mutex_unlock(&c->fetch_lock);
	if (have)
		return log_read(&c->log, chunk, buf, within, len);
	if (ret)
		return ret;

	ret = log_refresh(&c->log);
	if (!ret)
		have = log_has(&c->log, chunk);
	if (ret || have)
		;			/* failed, or someone else fetched it meanwhile */
	else if (!c->base)
		ret = c->err;		/* offline: nothing to fetch from */
	else if (!(tmp = malloc(c->chunk_size)))
		ret = -ENOMEM;
	else
		ret = c->ops->read(c->base, chunk * c->chunk_size, tmp, c->chunk_size);
	if (!ret && tmp && !verified(c, chunk, tmp))
		ret = -EIO;		/* not what the manifest promised: keep it out */
	if (!ret && tmp)
		ret = store(c, chunk, tmp);
	if (!ret && tmp)
		memcpy(buf, tmp + within, len);
	free(tmp);

	pthread_mutex_lock(&c->fetch_lock);
	xa_erase(&c->fetching, chunk);	/* in the log now, or the next reader tries */
	pthread_cond_broadcast(&c->fetch_done);
	pthread_mutex_unlock(&c->fetch_lock);
	if (!ret && have)
		ret = log_read(&c->log, chunk, buf, within, len);
	return ret;
}

/*
 * A reader that had to fetch chunk n will likely want n + 1 next. The
 * prefetcher fetches the hole that starts there: up to pf_window chunks,
 * fewer if the cache already holds one of them or the base ends. A newer
 * miss, somewhere else, takes over from a run still in progress.
 */
static void *prefetcher(void *arg)
{
	struct cache *c = arg;
	uint64_t nbase = (c->base_size + c->chunk_size - 1) / c->chunk_size;
	uint8_t byte;

	rcu_register_thread();
	pthread_mutex_lock(&c->fetch_lock);
	while (!c->pf_stop) {
		uint64_t at = c->pf_next, end = at + c->pf_window;

		if (at == PF_IDLE) {
			pthread_cond_wait(&c->pf_wake, &c->fetch_lock);
			continue;
		}
		c->pf_next = PF_IDLE;
		if (end > nbase)
			end = nbase;
		end = log_first(&c->log, at, end);
		for (; at < end && !c->pf_stop && c->pf_next == PF_IDLE; at++) {
			pthread_mutex_unlock(&c->fetch_lock);
			fetch(c, at, &byte, 0, 1);	/* an error is the reader's to meet */
			pthread_mutex_lock(&c->fetch_lock);
		}
	}
	pthread_mutex_unlock(&c->fetch_lock);
	rcu_unregister_thread();
	return NULL;
}

int cache_prefetch(struct cache *c, uint32_t chunks)
{
	sigset_t all, old;
	int ret;

	if (!c->base || !chunks || c->pf_window)
		return 0;
	c->pf_next = PF_IDLE;
	c->pf_window = chunks;
	/* signals are for the application's threads, not this one */
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	ret = -pthread_create(&c->pf_thread, NULL, prefetcher, c);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	if (ret)
		c->pf_window = 0;
	return ret;
}

int cache_read(struct cache *c, uint64_t chunk, uint8_t *buf,
	       uint32_t within, uint32_t len)
{
	if (!c->ops || chunk * c->chunk_size >= c->base_size) {
		memset(buf, 0, len);	/* no base, or past its end: nothing to fetch */
		return 0;
	}
	if (log_has(&c->log, chunk))
		return log_read(&c->log, chunk, buf, within, len);
	if (c->pf_window) {
		pthread_mutex_lock(&c->fetch_lock);
		c->pf_next = chunk + 1;
		pthread_cond_signal(&c->pf_wake);
		pthread_mutex_unlock(&c->fetch_lock);
	}
	return fetch(c, chunk, buf, within, len);
}
