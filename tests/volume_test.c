// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Volume engine tests. Each test_* gets a fresh volume over a random base
 * image and checks it byte for byte against an in-memory copy.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <urcu.h>

#include "vbd.h"

#define SIZE		(1u << 20)	/* 1 MiB volume */
#define CHUNK		4096u
#define SUPER_SIZE	4096		/* must match volume.c */

static uint8_t ref[SIZE], buf[SIZE];
static char base_path[] = "/tmp/vbd-base-XXXXXX";
static char log_path[64];

static void fill_random(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = rand();
}

static void check_all(struct vbd *v)
{
	assert(vbd_read(v, buf, SIZE, 0) == SIZE);
	assert(memcmp(buf, ref, SIZE) == 0);
}

/* Fresh base (random first half, shorter than the volume) and empty log. */
static struct vbd *setup(void)
{
	struct vbd *v;
	int fd = mkstemp(base_path);

	assert(fd >= 0);
	memset(ref, 0, SIZE);
	fill_random(ref, SIZE / 2);
	assert(write(fd, ref, SIZE / 2) == SIZE / 2);
	close(fd);
	snprintf(log_path, sizeof(log_path), "%s.log", base_path);
	assert(vbd_create(log_path, SIZE, CHUNK, base_path) == 0);
	v = vbd_open(log_path);
	assert(v);
	return v;
}

static void teardown(struct vbd *v)
{
	char cache_path[sizeof(log_path) + 8];

	vbd_close(v);		/* NULL is fine */
	snprintf(cache_path, sizeof(cache_path), "%s.cache", log_path);
	unlink(cache_path);
	unlink(log_path);
	unlink(base_path);
	strcpy(base_path, "/tmp/vbd-base-XXXXXX");
}

/* Close, open again, and make sure nothing was lost on the way. */
static struct vbd *reopen(struct vbd *v)
{
	assert(vbd_flush(v) == 0);
	vbd_close(v);
	v = vbd_open(log_path);
	assert(v);
	check_all(v);
	return v;
}

/* Flip one bit at @off in the log. */
static void flip_log_bit(uint64_t off)
{
	uint8_t byte;
	int fd = open(log_path, O_RDWR);

	assert(fd >= 0);
	assert(pread(fd, &byte, 1, off) == 1);
	byte ^= 1;
	assert(pwrite(fd, &byte, 1, off) == 1);
	close(fd);
}

static void test_create_errors(void)
{
	struct vbd *v = setup();

	assert(vbd_create(log_path, SIZE, CHUNK, base_path) == -EEXIST);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, 3000, NULL) == -EINVAL);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE + 1, CHUNK, NULL) == -EINVAL);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, CHUNK, "/nonexistent") == -ENOENT);
	teardown(v);
}

static void test_fresh_volume_reads_base(void)
{
	struct vbd *v = setup();

	assert(vbd_size(v) == SIZE && vbd_chunk_size(v) == CHUNK);
	check_all(v);	/* first half: base; second half: zeros */
	teardown(v);
}

static void test_end_of_volume(void)
{
	struct vbd *v = setup();

	assert(vbd_read(v, buf, 1, SIZE) == 0);
	assert(vbd_read(v, buf, 10, SIZE - 3) == 3);
	assert(vbd_write(v, buf, 1, SIZE) == -ENOSPC);
	assert(vbd_write(v, buf, 10, SIZE - 3) == 3);
	memcpy(ref + SIZE - 3, buf, 3);
	check_all(v);
	teardown(v);
}

static void test_random_io(void)
{
	struct vbd *v = setup();

	for (int i = 0; i < 3000; i++) {
		uint64_t off = rand() % SIZE;
		size_t len = 1 + rand() % (3 * CHUNK);

		if (off + len > SIZE)
			len = SIZE - off;
		if (rand() % 2) {
			fill_random(buf, len);
			assert(vbd_write(v, buf, len, off) == (ssize_t)len);
			memcpy(ref + off, buf, len);
		} else {
			assert(vbd_read(v, buf, len, off) == (ssize_t)len);
			assert(memcmp(buf, ref + off, len) == 0);
		}
		if (i % 1000 == 999)
			v = reopen(v);
	}
	teardown(v);
}

/* A half-written record at the tail is dropped, and appending resumes. */
static void test_torn_tail(void)
{
	struct vbd *v = setup();
	int fd;

	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, CHUNK) == CHUNK);
	memcpy(ref + CHUNK, buf, CHUNK);
	vbd_close(v);

	fd = open(log_path, O_WRONLY | O_APPEND);
	assert(fd >= 0);
	fill_random(buf, 100);
	assert(write(fd, buf, 100) == 100);
	close(fd);

	v = vbd_open(log_path);
	assert(v);
	check_all(v);
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);
	memcpy(ref, buf, CHUNK);
	v = reopen(v);
	teardown(v);
}

/* A damaged super block is refused and the log is left untouched. */
static void test_bad_super(void)
{
	struct vbd *v = setup();
	struct stat before, after;

	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);
	memcpy(ref, buf, CHUNK);
	vbd_close(v);

	assert(stat(log_path, &before) == 0);
	flip_log_bit(12);	/* chunk_size */
	assert(vbd_open(log_path) == NULL && errno == EINVAL);
	assert(stat(log_path, &after) == 0 && after.st_size == before.st_size);

	flip_log_bit(12);
	v = vbd_open(log_path);
	assert(v);
	check_all(v);
	teardown(v);
}

/* A corrupt record is a torn tail: it and everything after it is dropped. */
static void test_bad_record(void)
{
	struct vbd *v = setup();
	uint8_t base0[CHUNK];

	memcpy(base0, ref, CHUNK);
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);		/* record 0 */
	assert(vbd_write(v, buf, CHUNK, CHUNK) == CHUNK);	/* record 1 */
	vbd_close(v);

	flip_log_bit(SUPER_SIZE + 64 + 1);	/* record 0 data: crc mismatch */
	v = vbd_open(log_path);
	assert(v);
	/* both records are gone, so both chunks read the base again */
	memcpy(ref, base0, CHUNK);
	check_all(v);
	teardown(v);
}

/* Base chunks are fetched once, kept across reopen, never refetched. */
static void test_base_cache(void)
{
	struct vbd *v = setup();

	assert(vbd_fetched(v) == 0);
	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK);		/* chunk 0 */
	assert(vbd_read(v, buf, 10, 3 * CHUNK + 5) == 10);	/* chunk 3 */
	assert(vbd_fetched(v) == 2);
	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK);		/* hit */
	assert(vbd_fetched(v) == 2);

	/* check_all in reopen fetches the rest of the base; the zero half
	 * beyond its end is never fetched */
	v = reopen(v);
	assert(vbd_fetched(v) == SIZE / 2 / CHUNK);
	check_all(v);
	assert(vbd_fetched(v) == SIZE / 2 / CHUNK);

	/* a write to a cached chunk is served from the data log afterwards */
	fill_random(buf, 10);
	assert(vbd_write(v, buf, 10, 5) == 10);
	memcpy(ref + 5, buf, 10);
	check_all(v);
	assert(vbd_fetched(v) == SIZE / 2 / CHUNK);
	teardown(v);
}

static void test_stale_base(void)
{
	struct vbd *v = setup();

	vbd_close(v);
	assert(utimensat(AT_FDCWD, base_path, NULL, 0) == 0);
	assert(vbd_open(log_path) == NULL && errno == ESTALE);
	teardown(NULL);
}

/* A base that is gone leaves the volume open offline; only misses fail. */
static void test_offline_base(void)
{
	struct vbd *v = setup();
	char gone[sizeof(base_path) + 5];

	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK);		/* chunk 0 cached */
	vbd_close(v);
	snprintf(gone, sizeof(gone), "%s.gone", base_path);
	assert(rename(base_path, gone) == 0);

	v = vbd_open(log_path);
	assert(v && vbd_base_err(v) == -ENOENT);
	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK);		/* served from cache */
	assert(memcmp(buf, ref, CHUNK) == 0);
	assert(vbd_read(v, buf, CHUNK, CHUNK) == -ENOENT);	/* miss */
	assert(vbd_read(v, buf, CHUNK, SIZE / 2) == CHUNK);	/* past the base: zeros */
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 2 * CHUNK) == CHUNK);	/* whole chunk, no RMW */
	assert(vbd_fetched(v) == 1);
	vbd_close(v);

	assert(rename(gone, base_path) == 0);
	v = vbd_open(log_path);
	assert(v && vbd_base_err(v) == 0);
	assert(vbd_read(v, buf, CHUNK, CHUNK) == CHUNK);	/* back online */
	assert(vbd_fetched(v) == 2);
	teardown(v);
}

/* Many threads miss on the same chunks at once; each chunk is fetched once. */
#define NTHREADS	8

static void *reader(void *arg)
{
	struct vbd *v = arg;
	uint8_t *mine = malloc(CHUNK);
	size_t nchunk = SIZE / 2 / CHUNK;

	rcu_register_thread();
	for (size_t i = 0; i < nchunk; i++) {
		assert(vbd_read(v, mine, CHUNK, i * CHUNK) == CHUNK);
		assert(memcmp(mine, ref + i * CHUNK, CHUNK) == 0);
	}
	rcu_unregister_thread();
	free(mine);
	return NULL;
}

static void test_concurrent_fetch(void)
{
	struct vbd *v = setup();
	pthread_t t[NTHREADS];

	for (int i = 0; i < NTHREADS; i++)
		assert(pthread_create(&t[i], NULL, reader, v) == 0);
	for (int i = 0; i < NTHREADS; i++)
		assert(pthread_join(t[i], NULL) == 0);
	assert(vbd_fetched(v) == SIZE / 2 / CHUNK);
	check_all(v);
	teardown(v);
}

/* Header fields of record @n, read straight from the log file. */
static void rec_hdr(uint64_t n, uint64_t *chunk, uint64_t *epoch)
{
	int fd = open(log_path, O_RDONLY);
	off_t off = SUPER_SIZE + n * (64 + CHUNK);

	assert(fd >= 0);
	assert(pread(fd, chunk, 8, off + 8) == 8);	/* after magic, crc */
	assert(pread(fd, epoch, 8, off + 16) == 8);
	close(fd);
}

static void test_snapshot(void)
{
	struct vbd *v = setup();
	uint64_t chunk, epoch;

	assert(vbd_epoch(v) == 0);
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);		/* rec 0: chunk 0 @0 */
	assert(vbd_snapshot(v) == 0);				/* rec 1: marker @1 */
	assert(vbd_epoch(v) == 1);
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);		/* rec 2: chunk 0 @1 */
	memcpy(ref, buf, CHUNK);
	assert(vbd_snapshot(v) == 0);				/* rec 3: marker @2 */
	assert(vbd_snapshot(v) == 0);				/* rec 4: marker @3 */

	v = reopen(v);						/* epoch survives, markers stay out of the map */
	assert(vbd_epoch(v) == 3);
	check_all(v);
	rec_hdr(0, &chunk, &epoch); assert(chunk == 0 && epoch == 0);
	rec_hdr(1, &chunk, &epoch); assert(chunk == UINT64_MAX && epoch == 1);
	rec_hdr(2, &chunk, &epoch); assert(chunk == 0 && epoch == 1);
	rec_hdr(3, &chunk, &epoch); assert(chunk == UINT64_MAX && epoch == 2);
	rec_hdr(4, &chunk, &epoch); assert(chunk == UINT64_MAX && epoch == 3);
	teardown(v);
}

static void test_open_at(void)
{
	struct vbd *v = setup();
	uint8_t a[CHUNK], b[CHUNK], c[CHUNK], d[CHUNK];

	fill_random(a, CHUNK); fill_random(b, CHUNK);
	fill_random(c, CHUNK); fill_random(d, CHUNK);
	assert(vbd_write(v, a, CHUNK, 0) == CHUNK);
	assert(vbd_snapshot(v) == 0);			/* snapshot 0: chunk 0 = a */
	assert(vbd_write(v, b, CHUNK, 0) == CHUNK);
	assert(vbd_write(v, c, CHUNK, CHUNK) == CHUNK);
	assert(vbd_snapshot(v) == 0);			/* snapshot 1: a->b, chunk 1 = c */
	assert(vbd_write(v, d, CHUNK, 0) == CHUNK);	/* live: chunk 0 = d */
	vbd_close(v);

	v = vbd_open_at(log_path, 0);
	assert(v && vbd_epoch(v) == 2);
	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK && memcmp(buf, a, CHUNK) == 0);
	assert(vbd_read(v, buf, CHUNK, CHUNK) == CHUNK && memcmp(buf, ref + CHUNK, CHUNK) == 0);
	assert(vbd_write(v, a, CHUNK, 0) == -EROFS);
	assert(vbd_snapshot(v) == -EROFS);
	vbd_close(v);

	v = vbd_open_at(log_path, 1);
	assert(v);
	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK && memcmp(buf, b, CHUNK) == 0);
	assert(vbd_read(v, buf, CHUNK, CHUNK) == CHUNK && memcmp(buf, c, CHUNK) == 0);
	vbd_close(v);

	assert(vbd_open_at(log_path, 2) == NULL && errno == ENOENT);	/* not taken yet */

	v = vbd_open(log_path);					/* live view untouched */
	assert(v && vbd_epoch(v) == 2);
	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK && memcmp(buf, d, CHUNK) == 0);
	teardown(v);
}

#define RUN(t)	do { t(); puts("  " #t); } while (0)

int main(void)
{
	srand(1);
	rcu_register_thread();

	RUN(test_create_errors);
	RUN(test_fresh_volume_reads_base);
	RUN(test_end_of_volume);
	RUN(test_random_io);
	RUN(test_torn_tail);
	RUN(test_bad_super);
	RUN(test_bad_record);
	RUN(test_base_cache);
	RUN(test_stale_base);
	RUN(test_offline_base);
	RUN(test_concurrent_fetch);
	RUN(test_snapshot);
	RUN(test_open_at);

	rcu_barrier();
	rcu_unregister_thread();
	puts("volume test: ok");
	return 0;
}
