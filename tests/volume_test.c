// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Random reads and writes against a volume, checked byte for byte against
 * an in-memory copy. The volume is closed and reopened along the way, and
 * once with a torn record at the tail of the log.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <urcu.h>

#include "vbd.h"

#define SIZE		(1u << 20)	/* 1 MiB volume */
#define CHUNK		4096u
#define OPS		3000

static uint8_t ref[SIZE], buf[SIZE];
static char base_path[] = "/tmp/vbd-base-XXXXXX";
static char log_path[64];

static void check_all(struct vbd *v)
{
	assert(vbd_read(v, buf, SIZE, 0) == SIZE);
	assert(memcmp(buf, ref, SIZE) == 0);
}

static void fill_random(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = rand();
}

int main(void)
{
	struct vbd *v;
	int fd;

	srand(1);
	rcu_register_thread();

	/* base image: random content, and shorter than the volume */
	fd = mkstemp(base_path);
	assert(fd >= 0);
	fill_random(ref, SIZE / 2);
	assert(write(fd, ref, SIZE / 2) == SIZE / 2);
	close(fd);
	snprintf(log_path, sizeof(log_path), "%s.log", base_path);
	unlink(log_path);

	assert(vbd_create(log_path, SIZE, CHUNK, base_path) == 0);
	assert(vbd_create(log_path, SIZE, CHUNK, base_path) == -EEXIST);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, 3000, NULL) == -EINVAL);
	assert(vbd_create("/tmp/vbd-bad.log", SIZE, CHUNK, "/nonexistent") == -ENOENT);
	v = vbd_open(log_path);
	assert(v && vbd_size(v) == SIZE && vbd_chunk_size(v) == CHUNK);
	check_all(v);

	/* end of volume: short read, short write, no space */
	assert(vbd_read(v, buf, 1, SIZE) == 0);
	assert(vbd_read(v, buf, 10, SIZE - 3) == 3);
	assert(vbd_write(v, buf, 1, SIZE) == -ENOSPC);
	assert(vbd_write(v, buf, 10, SIZE - 3) == 3);
	memcpy(ref + SIZE - 3, buf, 3);

	for (int i = 0; i < OPS; i++) {
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
		if (i % 1000 == 999) {
			assert(vbd_flush(v) == 0);
			vbd_close(v);
			v = vbd_open(log_path);
			assert(v);
			check_all(v);
		}
	}

	/* torn tail: a half-written record must be ignored, not corrupt the volume */
	vbd_close(v);
	fd = open(log_path, O_WRONLY | O_APPEND);
	assert(fd >= 0);
	fill_random(buf, 100);
	assert(write(fd, buf, 100) == 100);
	close(fd);
	v = vbd_open(log_path);
	assert(v);
	check_all(v);
	/* and appending after recovery still works */
	fill_random(buf, CHUNK);
	assert(vbd_write(v, buf, CHUNK, 0) == CHUNK);
	memcpy(ref, buf, CHUNK);
	vbd_close(v);
	v = vbd_open(log_path);
	assert(v);
	check_all(v);
	vbd_close(v);

	/* a base that changed since create is refused */
	assert(utimensat(AT_FDCWD, base_path, NULL, 0) == 0);
	assert(vbd_open(log_path) == NULL && errno == ESTALE);

	unlink(log_path);
	unlink(base_path);
	rcu_barrier();
	rcu_unregister_thread();
	puts("volume test: ok");
	return 0;
}
