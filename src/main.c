// SPDX-License-Identifier: GPL-2.0-or-later
/* vbd command line: create, inspect, read and write a volume. */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <urcu.h>

#include <vbd.h>

#define BUF_SIZE	(1 << 20)

static const char *usage_text =
	"usage: vbd <command> <log> ...\n"
	"  create <log> <size> <chunk> [base]  new volume; sizes take K, M, G suffixes\n"
	"  info   <log>                        print size, chunk size, chunks fetched, base state\n"
	"  read   <log> <off> <len>            copy a range to stdout\n"
	"  write  <log> <off>                  copy stdin to the volume and flush\n";

static int usage(void)
{
	fputs(usage_text, stderr);
	return 2;
}

/* Decimal with optional K, M or G suffix. */
static int parse_size(const char *s, uint64_t *out)
{
	char *end;
	uint64_t n = strtoull(s, &end, 0);

	if (end == s)
		return -1;
	switch (*end) {
	case 'G': n <<= 10; /* fall through */
	case 'M': n <<= 10; /* fall through */
	case 'K': n <<= 10; end++; break;
	}
	if (*end)
		return -1;
	*out = n;
	return 0;
}

static int cmd_create(const char *log, const char *size_s, const char *chunk_s,
		      const char *base)
{
	uint64_t size, chunk;
	int ret;

	if (parse_size(size_s, &size) || parse_size(chunk_s, &chunk) ||
	    chunk > UINT32_MAX)
		return usage();
	ret = vbd_create(log, size, chunk, base);
	if (ret) {
		fprintf(stderr, "vbd create: %s\n", strerror(-ret));
		return 1;
	}
	return 0;
}

static int cmd_info(struct vbd *v)
{
	printf("size %" PRIu64 "\nchunk %" PRIu32 "\nfetched %" PRIu64 "\n",
	       vbd_size(v), vbd_chunk_size(v), vbd_fetched(v));
	if (vbd_base_err(v))
		printf("base offline: %s\n", strerror(-vbd_base_err(v)));
	return 0;
}

static int cmd_read(struct vbd *v, uint64_t off, uint64_t len)
{
	uint8_t *buf = malloc(BUF_SIZE);

	while (len) {
		size_t n = len < BUF_SIZE ? len : BUF_SIZE;
		ssize_t got = vbd_read(v, buf, n, off);

		if (got < 0) {
			fprintf(stderr, "vbd read: %s\n", strerror(-got));
			free(buf);
			return 1;
		}
		if (!got)
			break;
		if (fwrite(buf, 1, got, stdout) != (size_t)got) {
			perror("vbd read: stdout");
			free(buf);
			return 1;
		}
		off += got; len -= got;
	}
	free(buf);
	return 0;
}

static int cmd_write(struct vbd *v, uint64_t off)
{
	uint8_t *buf = malloc(BUF_SIZE);
	size_t n;
	int ret = 0;

	while ((n = fread(buf, 1, BUF_SIZE, stdin)) > 0) {
		ssize_t put = vbd_write(v, buf, n, off);

		if (put < 0) {
			fprintf(stderr, "vbd write: %s\n", strerror(-put));
			ret = 1;
			goto out;
		}
		off += put;
		if ((size_t)put < n) {
			fputs("vbd write: input runs past the end of the volume\n", stderr);
			ret = 1;
			goto out;
		}
	}
	if (ferror(stdin)) {
		perror("vbd write: stdin");
		ret = 1;
	}
out:
	free(buf);
	if (vbd_flush(v))
		ret = 1;
	return ret;
}

int main(int argc, char **argv)
{
	const char *cmd, *log;
	struct vbd *v;
	uint64_t off, len;
	int ret;

	if (argc < 3)
		return usage();
	cmd = argv[1]; log = argv[2];
	argc -= 2; argv += 2;

	if (!strcmp(cmd, "create")) {
		if (argc != 3 && argc != 4)
			return usage();
		return cmd_create(log, argv[1], argv[2], argc == 4 ? argv[3] : NULL);
	}

	if (!strcmp(cmd, "info")) {
		if (argc != 1)
			return usage();
	} else if (!strcmp(cmd, "read")) {
		if (argc != 3 || parse_size(argv[1], &off) || parse_size(argv[2], &len))
			return usage();
	} else if (!strcmp(cmd, "write")) {
		if (argc != 2 || parse_size(argv[1], &off))
			return usage();
	} else {
		return usage();
	}

	rcu_register_thread();
	v = vbd_open(log);
	if (!v) {
		fprintf(stderr, "vbd: %s: %s\n", log, strerror(errno));
		return 1;
	}
	if (!strcmp(cmd, "info"))
		ret = cmd_info(v);
	else if (!strcmp(cmd, "read"))
		ret = cmd_read(v, off, len);
	else
		ret = cmd_write(v, off);
	vbd_close(v);
	return ret;
}
