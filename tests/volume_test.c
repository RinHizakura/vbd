// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Volume engine tests. Each test_* gets a fresh volume over a random base
 * image and checks it byte for byte against an in-memory copy.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
	vbd_close(v);		/* NULL is fine */
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

static void test_stale_base(void)
{
	struct vbd *v = setup();

	vbd_close(v);
	assert(utimensat(AT_FDCWD, base_path, NULL, 0) == 0);
	assert(vbd_open(log_path) == NULL && errno == ESTALE);
	teardown(NULL);
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
	RUN(test_stale_base);

	rcu_barrier();
	rcu_unregister_thread();
	puts("volume test: ok");
	return 0;
}
