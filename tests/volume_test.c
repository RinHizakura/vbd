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
/* the on-disk format, for the tests that look at or damage the files */
#include "../src/ckpt.h"
#include "../src/log.h"

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

	assert(vbd_create(log_path, SIZE, CHUNK, base_path) == -EEXIST);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, 3000, NULL) == -EINVAL);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, 8, NULL) == -EINVAL);	/* under 16 */
	assert(vbd_create("/tmp/vbd-bad.log", 0, CHUNK, NULL) == -EINVAL);	/* no chunks at all */
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
	assert(vbd_create(log_path, BIG, CHUNK, NULL) == 0);
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
	assert(vbd_create(log_path, (uint64_t)BIG_CHUNKS * CHUNK, CHUNK, big_base) == 0);

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
	RUN(test_snapshot);
	RUN(test_open_at);
	RUN(test_snapshot_checkpoint);
	RUN(test_one_writer);
	RUN(test_shared_cache);
	RUN(test_view_keeps_tail);
	RUN(test_checkpoint);
	RUN(test_cache_checkpoint);

	rcu_barrier();
	rcu_unregister_thread();
	puts("volume test: ok");
	return 0;
}
