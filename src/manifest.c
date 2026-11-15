// SPDX-License-Identifier: GPL-2.0-or-later
/* The manifest of a base. Format and rules in manifest.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vbd.h"
#include "common.h"
#include "manifest.h"

#define MANIFEST_SUFFIX	".manifest"

struct manifest {
	uint32_t chunk_size;
	size_t len;		/* bytes of sums */
	uint8_t sums[];		/* SHA256_LEN for each chunk of the base */
};

int vbd_manifest(const char *image, uint32_t chunk_size, int fd)
{
	uint8_t *buf, sum[SHA256_LEN];
	size_t got;
	FILE *f;
	int ret = 0;

	if (!chunk_size)
		return -EINVAL;
	f = fopen(image, "r");
	if (!f)
		return -errno;
	buf = malloc(chunk_size);
	if (!buf)
		ret = -ENOMEM;
	while (!ret && (got = fread(buf, 1, chunk_size, f)) > 0) {
		memset(buf + got, 0, chunk_size - got);	/* a short last chunk */
		sha256(buf, chunk_size, sum);
		for (size_t done = 0; !ret && done < sizeof(sum); ) {
			ssize_t n = write(fd, sum + done, sizeof(sum) - done);

			if (n > 0)
				done += n;
			else if (errno != EINTR)
				ret = n ? -errno : -EIO;
		}
	}
	if (!ret && ferror(f))
		ret = -EIO;
	free(buf);
	fclose(f);
	return ret;
}

struct manifest *manifest_read(const char *path, uint64_t base_size, uint32_t chunk_size)
{
	size_t len = (base_size + chunk_size - 1) / chunk_size * SHA256_LEN;
	struct manifest *m = malloc(sizeof(*m) + len);
	int ret;

	if (!m)
		return NULL;
	m->chunk_size = chunk_size;
	m->len = len;
	ret = read_exact(path, m->sums, len);
	if (ret) {
		free(m);
		errno = -ret;
		return NULL;
	}
	return m;
}

struct manifest *manifest_open(const char *log_path, uint64_t base_size, uint32_t chunk_size,
			       const uint8_t hash[SHA256_LEN])
{
	char *path = side_path(log_path, MANIFEST_SUFFIX);
	struct manifest *m = NULL;
	uint8_t mine[SHA256_LEN];
	int err = ENOMEM;

	if (path && !(m = manifest_read(path, base_size, chunk_size)))
		err = errno;
	free(path);
	if (m) {
		manifest_hash(m, mine);
		if (!memcmp(mine, hash, SHA256_LEN))
			return m;
		err = EINVAL;	/* not the one the volume was created with */
		free(m);
	}
	errno = err;
	return NULL;
}

void manifest_close(struct manifest *m)
{
	free(m);
}

void manifest_hash(const struct manifest *m, uint8_t hash[SHA256_LEN])
{
	sha256(m->sums, m->len, hash);
}

int manifest_save(const struct manifest *m, const char *log_path)
{
	char *path = side_path(log_path, MANIFEST_SUFFIX);
	int fd, ret = 0;

	if (!path)
		return -ENOMEM;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		ret = -errno;
	} else {
		if (pwrite(fd, m->sums, m->len, 0) != (ssize_t)m->len || fsync(fd))
			ret = errno ? -errno : -EIO;
		close(fd);
		if (ret)
			unlink(path);
	}
	free(path);
	return ret;
}

bool manifest_check(const struct manifest *m, uint64_t chunk, const uint8_t *data)
{
	uint8_t hash[SHA256_LEN];

	if (!m)
		return true;
	sha256(data, m->chunk_size, hash);
	return !memcmp(hash, m->sums + chunk * SHA256_LEN, SHA256_LEN);
}
