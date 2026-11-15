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
#include <time.h>
#include <unistd.h>
#include <urcu.h>

#include "vbd.h"
/* the on-disk format, for the tests that look at or damage the files */
#include "../src/log.h"
#include "../src/sha256.h"

#define SIZE		(1u << 20)	/* 1 MiB volume */
#define CHUNK		4096u
#define REC		(HDR_SIZE + CHUNK)	/* one record of the test volumes */

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
	assert(vbd_create(log_path, SIZE, CHUNK, base_path, NULL) == 0);
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

/* Flip one bit at @off in the file at @path. */
static void flip_bit(const char *path, uint64_t off)
{
	uint8_t byte;
	int fd = open(path, O_RDWR);

	assert(fd >= 0);
	assert(pread(fd, &byte, 1, off) == 1);
	byte ^= 1;
	assert(pwrite(fd, &byte, 1, off) == 1);
	close(fd);
}

static void flip_log_bit(uint64_t off)
{
	flip_bit(log_path, off);
}

static void test_create_errors(void)
{
	struct vbd *v = setup();

	assert(vbd_create(log_path, SIZE, CHUNK, base_path, NULL) == -EEXIST);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, 3000, NULL, NULL) == -EINVAL);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, 8, NULL, NULL) == -EINVAL);	/* under 16 */
	assert(vbd_create("/tmp/vbd-bad.log", 0, CHUNK, NULL, NULL) == -EINVAL);	/* no chunks at all */
	assert(vbd_create("/tmp/vbd-bad.log", SIZE + 1, CHUNK, NULL, NULL) == -EINVAL);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, CHUNK, "/nonexistent", NULL) == -ENOENT);
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
		if (rand() % 8 == 0) {
			len *= 4;	/* long enough to take in whole chunks */
			if (off + len > SIZE)
				len = SIZE - off;
			assert(vbd_zero(v, off, len) == 0);
			memset(ref + off, 0, len);
		} else if (rand() % 2) {
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

	flip_log_bit(SUPER_SIZE + HDR_SIZE + 1);	/* record 0 data: crc mismatch */
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

/*
 * Several threads each keep rewriting their own slice of the same few
 * chunks. A write to part of a chunk reads the chunk, changes its part and
 * appends the whole; if two of those interleave, the later append carries
 * the other's slice as it was before, and that update is lost.
 */
static void *slice_writer(void *arg)
{
	struct vbd *v = ((void **)arg)[0];
	unsigned id = (uintptr_t)((void **)arg)[1];
	uint8_t slice[CHUNK / NTHREADS], back[CHUNK / NTHREADS];

	rcu_register_thread();
	for (int round = 0; round < 200; round++) {
		/* a new value each round, so that a neighbour's stale copy of it shows */
		memset(slice, round == 199 ? id + 1 : round, sizeof(slice));
		for (unsigned c = 0; c < 4; c++) {
			uint64_t off = c * CHUNK + id * sizeof(slice);

			assert(vbd_write(v, slice, sizeof(slice), off) == sizeof(slice));
			assert(vbd_read(v, back, sizeof(back), off) == sizeof(back));
			assert(memcmp(slice, back, sizeof(slice)) == 0);
		}
	}
	rcu_unregister_thread();
	return NULL;
}

static void test_concurrent_partial_writes(void)
{
	struct vbd *v = setup();
	void *args[NTHREADS][2];
	pthread_t t[NTHREADS];

	for (uintptr_t i = 0; i < NTHREADS; i++) {
		args[i][0] = v;
		args[i][1] = (void *)i;
		assert(pthread_create(&t[i], NULL, slice_writer, args[i]) == 0);
	}
	for (int i = 0; i < NTHREADS; i++)
		assert(pthread_join(t[i], NULL) == 0);
	for (unsigned c = 0; c < 4; c++)
		for (unsigned i = 0; i < NTHREADS; i++)
			memset(ref + c * CHUNK + i * (CHUNK / NTHREADS), i + 1, CHUNK / NTHREADS);
	check_all(v);
	teardown(v);
}

/* The header of record @n, read straight from the file. */
static void read_hdr(uint64_t n, void *hdr)
{
	int fd = open(log_path, O_RDONLY);

	assert(fd >= 0);
	assert(pread(fd, hdr, HDR_SIZE, SUPER_SIZE + n * REC) == HDR_SIZE);
	close(fd);
}

static void expect_data(uint64_t n, uint64_t chunk, uint64_t epoch)
{
	struct rec_hdr h;

	read_hdr(n, &h);
	assert(h.magic == VBD_MAGIC && h.chunk == chunk && h.epoch == epoch);
}

/* Record @n is the checkpoint of snapshot @snap: it steps the epoch past it. */
static void expect_snapshot(uint64_t n, uint64_t snap)
{
	struct ckpt_hdr h;

	read_hdr(n, &h);
	assert(h.magic == CKPT_MAGIC && h.epoch == snap && h.next_epoch == snap + 1);
}

static void test_snapshot(void)
{
	struct vbd *v = setup();

	assert(vbd_epoch(v) == 0);
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);		/* rec 0: chunk 0 @0 */
	assert(vbd_snapshot(v) == 0);				/* rec 1: snapshot 0 */
	assert(vbd_epoch(v) == 1);
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);		/* rec 2: chunk 0 @1 */
	memcpy(ref, buf, CHUNK);
	assert(vbd_snapshot(v) == 0);				/* rec 3: snapshot 1 */
	assert(vbd_snapshot(v) == 0);				/* rec 4: snapshot 2 */

	v = reopen(v);						/* the epoch survives */
	assert(vbd_epoch(v) == 3);
	check_all(v);
	expect_data(0, 0, 0);
	expect_snapshot(1, 0);
	expect_data(2, 0, 1);
	expect_snapshot(3, 1);
	expect_snapshot(4, 2);
	teardown(v);
}

/*
 * A snapshot is a checkpoint: a view of it is loaded from there and reads
 * no record before it (damage to one goes unnoticed, where a replay would
 * have stopped at it and found no snapshot), nor any after. A writable open
 * starts from it too. Before anything is written it is one empty record.
 */
static void test_snapshot_checkpoint(void)
{
	struct vbd *v = setup();
	uint8_t a[CHUNK], b[CHUNK], c[CHUNK];
	struct stat st;

	assert(vbd_snapshot(v) == 0);			/* rec 0: snapshot 0, of nothing */
	assert(stat(log_path, &st) == 0 && st.st_size == SUPER_SIZE + REC);
	fill_random(a, CHUNK); fill_random(b, CHUNK); fill_random(c, CHUNK);
	assert(vbd_write(v, a, CHUNK, 0) == CHUNK);		/* rec 1 */
	assert(vbd_write(v, b, CHUNK, CHUNK) == CHUNK);		/* rec 2 */
	assert(vbd_snapshot(v) == 0);				/* rec 3: snapshot 1 */
	assert(vbd_write(v, c, CHUNK, 0) == CHUNK);		/* rec 4 */
	vbd_close(v);
	expect_snapshot(0, 0);
	expect_snapshot(3, 1);

	flip_log_bit(SUPER_SIZE + 2 * REC - 1);	/* rec 1, chunk 0 as of snapshot 1 */
	v = vbd_open_at(log_path, 1);
	assert(v && vbd_epoch(v) == 2);
	assert(vbd_read(v, buf, CHUNK, CHUNK) == CHUNK && memcmp(buf, b, CHUNK) == 0);
	vbd_close(v);
	v = vbd_open_at(log_path, 0);				/* the empty one: base content */
	assert(v && vbd_epoch(v) == 2);
	check_all(v);
	vbd_close(v);
	assert(vbd_open_at(log_path, 2) == NULL && errno == ENOENT);
	v = vbd_open(log_path);					/* from snapshot 1, then rec 4 */
	assert(v && vbd_epoch(v) == 2);
	assert(vbd_read(v, buf, CHUNK, 0) == CHUNK && memcmp(buf, c, CHUNK) == 0);
	assert(vbd_read(v, buf, CHUNK, CHUNK) == CHUNK && memcmp(buf, b, CHUNK) == 0);
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

/*
 * After enough records the map is appended to the log as a checkpoint and
 * reopen starts from it. The proof: damage to a record it covers goes
 * unnoticed, where a replay would have cut the log short at that record
 * (test_bad_record). What was appended after the checkpoint is replayed on
 * top of it, and a damaged checkpoint means a full replay.
 */
/*
 * Zeroing costs one record for all the whole chunks of a range and hides
 * the base without fetching it. Zero chunks survive a reopen, by replay
 * and by checkpoint, and belong to the snapshot they were zeroed before.
 */
static void test_zero(void)
{
	struct vbd *v = setup(), *view;
	static uint8_t before[SIZE];
	struct stat st;

	assert(vbd_zero(v, 0, SIZE) == 0);			/* rec 0: all 256 chunks */
	assert(stat(log_path, &st) == 0 && st.st_size == SUPER_SIZE + REC);
	memset(ref, 0, SIZE);
	check_all(v);
	assert(vbd_fetched(v) == 0);				/* the base was never needed */

	fill_random(buf, 3 * CHUNK);
	assert(vbd_write(v, buf, 3 * CHUNK, CHUNK) == 3 * CHUNK);	/* recs 1..3 */
	memcpy(ref + CHUNK, buf, 3 * CHUNK);
	memcpy(before, ref, SIZE);
	assert(vbd_snapshot(v) == 0);				/* snapshot 0: its checkpoint has zero entries */
	assert(vbd_zero(v, CHUNK + 100, CHUNK) == 0);		/* parts of two chunks, no whole one */
	memset(ref + CHUNK + 100, 0, CHUNK);
	assert(vbd_zero(v, 2 * CHUNK + 5, 3 * CHUNK) == 0);	/* a head, two whole chunks, a tail */
	memset(ref + 2 * CHUNK + 5, 0, 3 * CHUNK);
	check_all(v);
	assert(vbd_zero(v, SIZE - CHUNK, 2 * CHUNK) == -EINVAL);
	assert(vbd_zero(v, SIZE, 0) == 0);

	v = reopen(v);						/* from the checkpoint, then by replay */
	view = vbd_open_at(log_path, 0);
	assert(view && vbd_read(view, buf, SIZE, 0) == SIZE && memcmp(buf, before, SIZE) == 0);
	assert(vbd_zero(view, 0, CHUNK) == -EROFS);
	vbd_close(view);
	teardown(v);
}

/* Wait until @v has fetched @want chunks, give it a moment to overshoot, and check it did not. */
static void expect_fetched(struct vbd *v, uint64_t want)
{
	for (int i = 0; i < 300 && vbd_fetched(v) < want; i++)
		usleep(10000);
	usleep(50000);
	assert(vbd_fetched(v) == want);
}

/*
 * With prefetching on, a miss brings in the chunks after it on its own: as
 * many as asked for, fewer when the cache already holds one of them or the
 * base ends, and none after a read the cache could serve.
 */
static void test_prefetch(void)
{
	struct vbd *v = setup();
	uint64_t nbase = SIZE / 2 / CHUNK;

	assert(vbd_prefetch(v, 4) == 0);
	assert(vbd_read(v, buf, 1, 8 * CHUNK) == 1);		/* miss on chunk 8 */
	expect_fetched(v, 5);					/* 8, and 9..12 behind it */
	assert(vbd_read(v, buf, 4 * CHUNK, 9 * CHUNK) == 4 * CHUNK);	/* hits */
	assert(memcmp(buf, ref + 9 * CHUNK, 4 * CHUNK) == 0);
	expect_fetched(v, 5);					/* a hit starts nothing */

	assert(vbd_read(v, buf, 1, 6 * CHUNK) == 1);		/* miss on 6: the hole is 7 alone */
	expect_fetched(v, 7);
	assert(vbd_read(v, buf, 1, (nbase - 2) * CHUNK) == 1);	/* two before the end of the base */
	expect_fetched(v, 9);					/* that one and the last */
	check_all(v);
	teardown(v);
}

/* Write a manifest of the base to <base>.manifest, wrong for @bad_chunk (-1: for none). */
static void write_manifest(char *path, size_t path_len, int bad_chunk)
{
	uint8_t sum[SHA256_LEN];
	FILE *f;

	snprintf(path, path_len, "%s.manifest", base_path);
	f = fopen(path, "w");
	assert(f);
	for (size_t c = 0; c < SIZE / 2 / CHUNK; c++) {
		sha256(ref + c * CHUNK, CHUNK, sum);
		if ((int)c == bad_chunk)
			sum[0] ^= 1;
		assert(fwrite(sum, sizeof(sum), 1, f) == 1);
	}
	assert(fclose(f) == 0);
}

/*
 * With a manifest, a chunk fetched from the base that does not match it is
 * refused and stays out of the cache. The manifest kept with the volume is
 * itself pinned by the super block.
 */
static void test_manifest(void)
{
	static const uint8_t abc[SHA256_LEN] = {	/* sha256("abc"), FIPS 180-4 */
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
		0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
	struct vbd *v = setup();
	char manifest[sizeof(base_path) + 16], kept[sizeof(log_path) + 16];
	uint8_t sum[SHA256_LEN];

	sha256("abc", 3, sum);
	assert(memcmp(sum, abc, SHA256_LEN) == 0);

	vbd_close(v);				/* setup's volume has no manifest: make one that has */
	assert(system("rm -f /tmp/vbd-base-*.log /tmp/vbd-base-*.log.cache") == 0);
	write_manifest(manifest, sizeof(manifest), 3);	/* says something else about chunk 3 */
	assert(vbd_create(log_path, SIZE, 2 * CHUNK, base_path, manifest) == -EINVAL);	/* other chunk size */
	assert(vbd_create(log_path, SIZE, CHUNK, NULL, manifest) == -EINVAL);		/* of what base? */
	assert(vbd_create(log_path, SIZE, CHUNK, base_path, manifest) == 0);

	v = vbd_open(log_path);
	assert(v);
	assert(vbd_read(v, buf, 3 * CHUNK, 0) == 3 * CHUNK);
	assert(memcmp(buf, ref, 3 * CHUNK) == 0);
	assert(vbd_read(v, buf, CHUNK, 3 * CHUNK) == -EIO);
	assert(vbd_read(v, buf, CHUNK, 3 * CHUNK) == -EIO);	/* it was not cached */
	assert(vbd_fetched(v) == 3);
	assert(vbd_read(v, buf, CHUNK, 4 * CHUNK) == CHUNK);	/* the rest is unaffected */
	assert(memcmp(buf, ref + 4 * CHUNK, CHUNK) == 0);
	vbd_close(v);

	snprintf(kept, sizeof(kept), "%s.manifest", log_path);
	flip_bit(kept, 3 * SHA256_LEN);		/* "repair" chunk 3 in the kept copy */
	assert(vbd_open(log_path) == NULL && errno == EINVAL);
	teardown(NULL);
}

/* One writer at a time; views of a snapshot are welcome beside it. */
static void test_one_writer(void)
{
	struct vbd *v = setup(), *view;

	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);
	memcpy(ref, buf, CHUNK);
	assert(vbd_snapshot(v) == 0);
	assert(vbd_open(log_path) == NULL && errno == EBUSY);
	view = vbd_open_at(log_path, 0);
	assert(view);
	check_all(view);
	vbd_close(view);
	vbd_close(v);
	v = vbd_open(log_path);			/* free again once the writer is gone */
	assert(v);
	teardown(v);
}

/*
 * A writer and a view of the same volume share its base cache, as two
 * processes would: each stores what it fetches, finds what the other
 * stored, and the file holds every chunk once.
 */
static void test_shared_cache(void)
{
	struct vbd *v = setup(), *view;
	char cache_path[sizeof(log_path) + 8];
	uint64_t nbase = SIZE / 2 / CHUNK;
	struct stat st;

	snprintf(cache_path, sizeof(cache_path), "%s.cache", log_path);
	assert(vbd_snapshot(v) == 0);
	view = vbd_open_at(log_path, 0);
	assert(view);

	assert(vbd_read(v, buf, 10 * CHUNK, 0) == 10 * CHUNK);		/* chunks 0..9 */
	assert(memcmp(buf, ref, 10 * CHUNK) == 0);
	assert(vbd_read(view, buf, 10 * CHUNK, 5 * CHUNK) == 10 * CHUNK);	/* 5..14 */
	assert(memcmp(buf, ref + 5 * CHUNK, 10 * CHUNK) == 0);
	assert(vbd_fetched(view) == 15);	/* found 0..9 there, added 10..14 */
	assert(stat(cache_path, &st) == 0);
	assert(st.st_size == SUPER_SIZE + 15 * REC);

	check_all(v);				/* its own, the view's, and the rest */
	check_all(view);
	assert(vbd_fetched(v) == nbase && vbd_fetched(view) == nbase);
	assert(stat(cache_path, &st) == 0);
	assert(st.st_size == SUPER_SIZE + nbase * REC);

	vbd_close(view);
	v = reopen(v);
	assert(vbd_fetched(v) == nbase);
	teardown(v);
}

/*
 * The same, of the log alone: two opens of one file as a shared log. Each
 * sees what the other appended once it has had a turn at the file, a chunk
 * both add is in the file once, and the first to arrive stays.
 */
static void test_shared_log(void)
{
	char path[] = "/tmp/vbd-shared-XXXXXX";
	uint8_t super[SUPER_SIZE] = { 0 }, x[CHUNK], y[CHUNK];
	struct log a, b;
	struct stat st;

	close(mkstemp(path));
	unlink(path);
	assert(log_create(path, super) == 0);
	log_init(&a);
	log_init(&b);
	assert(log_open(&a, path, super) == 0 && log_open(&b, path, super) == 0);
	assert(log_load(&a, CHUNK, 16, LOG_SHARED, 0) == 0);
	assert(log_load(&b, CHUNK, 16, LOG_SHARED, 0) == 0);

	fill_random(x, CHUNK); fill_random(y, CHUNK);
	assert(log_append_if(&a, 3, x, LOG_ABSENT) == 0);
	assert(!log_has(&b, 3));			/* not looked yet */
	assert(log_refresh(&b) == 0 && log_has(&b, 3));
	assert(log_append_if(&b, 3, y, LOG_ABSENT) == -EAGAIN);	/* a was first */
	assert(log_append_if(&b, 4, y, LOG_ABSENT) == 0);
	assert(log_append_if(&a, 4, x, LOG_ABSENT) == -EAGAIN);	/* seen without a refresh: appending is a turn */
	assert(stat(path, &st) == 0 && st.st_size == SUPER_SIZE + 2 * REC);
	assert(log_read(&b, 3, buf, 0, CHUNK) == 0 && memcmp(buf, x, CHUNK) == 0);
	assert(log_read(&a, 4, buf, 0, CHUNK) == 0 && memcmp(buf, y, CHUNK) == 0);
	log_close(&a);
	log_close(&b);

	log_init(&a);					/* and a writer finds both */
	assert(log_open(&a, path, super) == 0);
	assert(log_load(&a, CHUNK, 16, LOG_WRITER, 0) == 0 && log_chunks(&a) == 2);
	log_init(&b);
	assert(log_open(&b, path, super) == 0);
	assert(log_load(&b, CHUNK, 16, LOG_WRITER, 0) == -EBUSY);
	log_close(&b);
	log_close(&a);
	unlink(path);
}

/*
 * A fork starts out as its parent was at the fork and goes its own way
 * from there, as does the parent. A fork of a fork reads down the chain,
 * and all of them fetch into the one cache at the bottom.
 */
static void test_fork(void)
{
	struct vbd *v = setup(), *a, *b;
	char a_path[sizeof(log_path) + 4], b_path[sizeof(log_path) + 4];
	static uint8_t at_fork[SIZE], ref_a[SIZE];
	uint64_t nbase = SIZE / 2 / CHUNK;
	struct stat st;

	snprintf(a_path, sizeof(a_path), "%s.a", log_path);
	snprintf(b_path, sizeof(b_path), "%s.b", log_path);
	fill_random(buf, 2 * CHUNK);
	assert(vbd_write(v, buf, 2 * CHUNK, 0) == 2 * CHUNK);
	memcpy(ref, buf, 2 * CHUNK);
	memcpy(at_fork, ref, SIZE);

	assert(vbd_fork(log_path, VBD_HEAD, a_path) == -EBUSY);	/* v would have to be snapshotted */
	vbd_close(v);
	assert(vbd_fork(log_path, VBD_HEAD, a_path) == 0);	/* takes snapshot 0 */
	assert(stat(a_path, &st) == 0 && st.st_size == SUPER_SIZE);
	assert(vbd_fork(log_path, 1, b_path) == -ENOENT);	/* no such snapshot */
	assert(vbd_fork(log_path, 0, a_path) == -EEXIST);

	v = vbd_open(log_path);					/* the parent moves on */
	assert(v && vbd_epoch(v) == 1);
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, CHUNK) == CHUNK);
	memcpy(ref + CHUNK, buf, CHUNK);

	a = vbd_open(a_path);					/* beside the parent's writer */
	assert(a && vbd_size(a) == SIZE && vbd_chunk_size(a) == CHUNK);
	assert(vbd_read(a, buf, SIZE, 0) == SIZE && memcmp(buf, at_fork, SIZE) == 0);
	assert(vbd_fetched(a) == nbase - 2);			/* the parent had written two itself */
	memcpy(ref_a, at_fork, SIZE);
	fill_random(buf, 100);
	assert(vbd_write(a, buf, 100, 3 * CHUNK + 7) == 100);	/* read-modify-write down the chain */
	memcpy(ref_a + 3 * CHUNK + 7, buf, 100);
	assert(vbd_read(a, buf, SIZE, 0) == SIZE && memcmp(buf, ref_a, SIZE) == 0);
	check_all(v);						/* the parent saw none of it */
	assert(vbd_fetched(v) == nbase - 2);			/* and found the fork's fetches in the cache */
	assert(vbd_fork(log_path, 0, b_path) == 0);		/* an explicit snapshot needs no writer */
	unlink(b_path);
	vbd_close(a);

	assert(vbd_fork(a_path, VBD_HEAD, b_path) == 0);	/* a fork of the fork */
	a = vbd_open(a_path);
	fill_random(buf, CHUNK);
	assert(a && vbd_write(a, buf, CHUNK, 5 * CHUNK) == CHUNK);	/* after b was forked off */
	b = vbd_open(b_path);
	assert(b && vbd_read(b, buf, SIZE, 0) == SIZE && memcmp(buf, ref_a, SIZE) == 0);
	assert(vbd_fetched(b) == nbase - 2);			/* nothing was fetched twice */
	vbd_close(b);
	vbd_close(a);
	teardown(v);
}

/*
 * A snapshot remembers when it was taken, and a volume can be opened as it
 * was at a time: at the newest snapshot taken by then. The content is the
 * earlier one, byte for byte, although everything was written again since.
 */
static void test_open_before(void)
{
	struct vbd *v = setup();
	static uint8_t earlier[SIZE];
	time_t t0, t1;

	assert(vbd_open_before(log_path, time(NULL)) == NULL && errno == ENOENT);
	fill_random(buf, 3 * CHUNK);
	assert(vbd_write(v, buf, 3 * CHUNK, 0) == 3 * CHUNK);
	memcpy(ref, buf, 3 * CHUNK);
	memcpy(earlier, ref, SIZE);
	assert(vbd_snapshot(v) == 0);			/* snapshot 0, within second t0 */
	t0 = time(NULL);
	fill_random(buf, SIZE);
	assert(vbd_write(v, buf, SIZE, 0) == SIZE);	/* write again, all of it */
	memcpy(ref, buf, SIZE);
	while ((t1 = time(NULL)) == t0)			/* times are whole seconds */
		usleep(10000);
	assert(vbd_snapshot(v) == 0);			/* snapshot 1, at t1 or later */
	vbd_close(v);

	assert(vbd_open_before(log_path, t0 - 10) == NULL && errno == ENOENT);
	v = vbd_open_before(log_path, t0);		/* snapshot 0: 1 is too new */
	assert(v && vbd_read(v, buf, SIZE, 0) == SIZE && memcmp(buf, earlier, SIZE) == 0);
	assert(vbd_write(v, buf, CHUNK, 0) == -EROFS);
	vbd_close(v);
	v = vbd_open_before(log_path, time(NULL) + 10);	/* snapshot 1 */
	assert(v);
	check_all(v);
	teardown(v);
}

/*
 * A view leaves the file as it finds it: what looks like a torn tail may be
 * a record another process is still writing. That holds when the view is
 * loaded from its snapshot's checkpoint, when it has to replay because the
 * checkpoint is damaged, and when the snapshot asked for does not exist.
 */
static void test_view_keeps_tail(void)
{
	struct vbd *v = setup();
	uint8_t a[CHUNK], b[CHUNK];
	struct stat before, after;
	int fd;

	fill_random(a, CHUNK); fill_random(b, CHUNK);
	assert(vbd_write(v, a, CHUNK, 0) == CHUNK);		/* rec 0 */
	assert(vbd_snapshot(v) == 0);				/* rec 1: snapshot 0 */
	assert(vbd_write(v, b, CHUNK, 0) == CHUNK);		/* rec 2 */
	vbd_close(v);
	fd = open(log_path, O_WRONLY | O_APPEND);		/* half a record, in progress */
	assert(fd >= 0 && write(fd, b, 100) == 100);
	close(fd);
	assert(stat(log_path, &before) == 0);

	v = vbd_open_at(log_path, 0);				/* from the checkpoint */
	assert(v && vbd_read(v, buf, CHUNK, 0) == CHUNK && memcmp(buf, a, CHUNK) == 0);
	vbd_close(v);
	assert(vbd_open_at(log_path, 7) == NULL && errno == ENOENT);	/* replays to the end */
	flip_log_bit(SUPER_SIZE + REC + HDR_SIZE + 1);	/* snapshot 0's checkpoint */
	v = vbd_open_at(log_path, 0);				/* by replay */
	assert(v && vbd_read(v, buf, CHUNK, 0) == CHUNK && memcmp(buf, a, CHUNK) == 0);
	vbd_close(v);
	assert(stat(log_path, &after) == 0 && after.st_size == before.st_size);

	v = vbd_open(log_path);					/* the writer does clean up */
	assert(v && vbd_read(v, buf, CHUNK, 0) == CHUNK && memcmp(buf, b, CHUNK) == 0);
	assert(stat(log_path, &after) == 0 && after.st_size == before.st_size - 100);
	teardown(v);
}

#define CKPT_NREC	((CKPT_BYTES + REC - 1) / REC)	/* records until one is due */
#define BIG		(3u << 20)	/* 768 chunks: a map of three records */

static void test_checkpoint(void)
{
	struct vbd *v = setup();
	static uint8_t big_ref[BIG], big_buf[BIG];
	struct stat st;

	for (unsigned i = 0; i < CKPT_NREC; i++) {	/* over and over the 256 chunks */
		uint64_t off = (i % (SIZE / CHUNK)) * CHUNK;

		fill_random(buf, CHUNK);
		assert(vbd_write(v, buf, CHUNK, off) == CHUNK);
		memcpy(ref + off, buf, CHUNK);
	}
	v = reopen(v);				/* close appended it: 256 entries fit one record */
	assert(stat(log_path, &st) == 0);
	assert(st.st_size == SUPER_SIZE + (CKPT_NREC + 1) * REC);

	fill_random(buf, CHUNK);		/* one more record, after the checkpoint */
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);
	memcpy(ref, buf, CHUNK);
	vbd_close(v);				/* too little new for another checkpoint */
	assert(stat(log_path, &st) == 0);
	assert(st.st_size == SUPER_SIZE + (CKPT_NREC + 2) * REC);

	flip_log_bit(SUPER_SIZE + HDR_SIZE + 1);	/* record 0: covered, long superseded */
	v = vbd_open(log_path);
	assert(v);
	check_all(v);				/* the map from the checkpoint, the last record by replay */
	vbd_close(v);

	flip_log_bit(SUPER_SIZE + HDR_SIZE + 1);	/* heal record 0, break the checkpoint */
	flip_log_bit(SUPER_SIZE + CKPT_NREC * REC + HDR_SIZE + 1);
	v = vbd_open(log_path);
	assert(v);
	check_all(v);				/* by replay from record 0 */
	teardown(v);

	/* a map too big for one record is cut into parts: 768 entries, 256 in each */
	snprintf(log_path, sizeof(log_path), "/tmp/vbd-base-big.log");
	assert(vbd_create(log_path, BIG, CHUNK, NULL, NULL) == 0);
	v = vbd_open(log_path);
	assert(v);
	memset(big_ref, 0, BIG);
	for (unsigned i = 0; i < CKPT_NREC; i++) {
		uint64_t off = (i % (BIG / CHUNK)) * CHUNK;

		fill_random(buf, CHUNK);
		assert(vbd_write(v, buf, CHUNK, off) == CHUNK);
		memcpy(big_ref + off, buf, CHUNK);
	}
	vbd_close(v);
	assert(stat(log_path, &st) == 0);
	assert(st.st_size == SUPER_SIZE + (CKPT_NREC + 3) * REC);
	flip_log_bit(SUPER_SIZE + HDR_SIZE + 1);	/* record 0 again */
	v = vbd_open(log_path);
	assert(v && vbd_read(v, big_buf, BIG, 0) == BIG && memcmp(big_buf, big_ref, BIG) == 0);
	vbd_close(v);
	unlink(log_path);
}

/*
 * The base cache is a log like the data log, so it checkpoints the same
 * way, when the volume is closed. A base big enough to be due for one is
 * read through; after a reopen the cache is known from the checkpoint, not
 * from its records: damage to the first of them would otherwise empty it.
 */
#define BIG_CHUNKS	17000u		/* 66 MiB: past CKPT_BYTES of cache records */
#define BIG_PARTS	((BIG_CHUNKS + CHUNK / 16 - 1) / (CHUNK / 16))

/* Chunk @i of the big base: its own number, over and over. */
static void big_chunk(uint8_t *p, uint32_t i)
{
	for (size_t k = 0; k < CHUNK; k += sizeof(i))
		memcpy(p + k, &i, sizeof(i));
}

static void test_cache_checkpoint(void)
{
	char big_base[] = "/tmp/vbd-base-cache.img", cache_path[sizeof(log_path) + 8];
	uint8_t want[CHUNK];
	struct stat st;
	struct vbd *v;
	FILE *f = fopen(big_base, "w");

	assert(f);
	for (uint32_t i = 0; i < BIG_CHUNKS; i++) {
		big_chunk(want, i);
		assert(fwrite(want, CHUNK, 1, f) == 1);
	}
	assert(fclose(f) == 0);
	snprintf(log_path, sizeof(log_path), "%s.log", big_base);
	snprintf(cache_path, sizeof(cache_path), "%s.cache", log_path);
	assert(vbd_create(log_path, (uint64_t)BIG_CHUNKS * CHUNK, CHUNK, big_base, NULL) == 0);

	v = vbd_open(log_path);
	assert(v);
	for (uint32_t i = 0; i < BIG_CHUNKS; i++) {
		big_chunk(want, i);
		assert(vbd_read(v, buf, CHUNK, (uint64_t)i * CHUNK) == CHUNK);
		assert(memcmp(buf, want, CHUNK) == 0);
	}
	assert(vbd_fetched(v) == BIG_CHUNKS);
	vbd_close(v);				/* appends the checkpoint */
	assert(stat(cache_path, &st) == 0);
	assert(st.st_size == SUPER_SIZE + (uint64_t)(BIG_CHUNKS + BIG_PARTS) * REC);

	flip_bit(cache_path, SUPER_SIZE + HDR_SIZE + 1);	/* cache record 0: chunk 0 */
	v = vbd_open(log_path);
	assert(v && vbd_fetched(v) == BIG_CHUNKS);	/* checkpoint records are not chunks */
	big_chunk(want, BIG_CHUNKS - 1);
	assert(vbd_read(v, buf, CHUNK, (uint64_t)(BIG_CHUNKS - 1) * CHUNK) == CHUNK);
	assert(memcmp(buf, want, CHUNK) == 0);
	assert(vbd_fetched(v) == BIG_CHUNKS);		/* served from the cache */
	vbd_close(v);
	assert(stat(cache_path, &st) == 0);		/* nothing new: no second checkpoint */
	assert(st.st_size == SUPER_SIZE + (uint64_t)(BIG_CHUNKS + BIG_PARTS) * REC);

	unlink(cache_path);
	unlink(log_path);
	unlink(big_base);
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
	RUN(test_concurrent_partial_writes);
	RUN(test_snapshot);
	RUN(test_open_at);
	RUN(test_snapshot_checkpoint);
	RUN(test_open_before);
	RUN(test_fork);
	RUN(test_zero);
	RUN(test_prefetch);
	RUN(test_manifest);
	RUN(test_one_writer);
	RUN(test_shared_cache);
	RUN(test_shared_log);
	RUN(test_view_keeps_tail);
	RUN(test_checkpoint);
	RUN(test_cache_checkpoint);

	rcu_barrier();
	rcu_unregister_thread();
	puts("volume test: ok");
	return 0;
}
