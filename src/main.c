// SPDX-License-Identifier: GPL-2.0-or-later
/* vbd command line: create, inspect, read and write a volume. */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <urcu.h>

#include <vbd.h>

#include "ublk/ublk.h"

#define BUF_SIZE	(1 << 20)

struct cmd {
	const char *name, *args, *help;
	int min, max;		/* how many arguments follow <log> */
	bool at;		/* the optional last one is a snapshot to open the volume at */
	/* exactly one of the two: */
	int (*make)(const char *log, char **args, int n);	/* works on the path */
	int (*run)(struct vbd *v, const uint64_t *n, bool at);	/* on the open volume; the
								 * arguments are numbers */
};

static const struct cmd cmds[];

static int usage(void)
{
	fputs("usage: vbd <command> <log> ...\n", stderr);
	for (const struct cmd *c = cmds; c->name; c++)
		fprintf(stderr, "  %-8s <log> %-23s %s\n", c->name, c->args, c->help);
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

static int cmd_create(const char *log, char **args, int n)
{
	uint64_t size, chunk;
	int ret;

	if (parse_size(args[0], &size) || parse_size(args[1], &chunk) ||
	    chunk > UINT32_MAX)
		return usage();
	ret = vbd_create(log, size, chunk, n > 2 ? args[2] : NULL);
	if (ret) {
		fprintf(stderr, "vbd create: %s\n", strerror(-ret));
		return 1;
	}
	return 0;
}

static int cmd_info(struct vbd *v, const uint64_t *n, bool at)
{
	printf("size %" PRIu64 "\nchunk %" PRIu32 "\nepoch %" PRIu64 "\nfetched %" PRIu64 "\n",
	       vbd_size(v), vbd_chunk_size(v), vbd_epoch(v), vbd_fetched(v));
	if (vbd_base_err(v))
		printf("base offline: %s\n", strerror(-vbd_base_err(v)));
	return 0;
}

static int cmd_read(struct vbd *v, const uint64_t *n, bool at)
{
	uint64_t off = n[0], len = n[1];
	uint8_t *buf = malloc(BUF_SIZE);

	while (len) {
		size_t want = len < BUF_SIZE ? len : BUF_SIZE;
		ssize_t got = vbd_read(v, buf, want, off);

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

static int cmd_snapshot(struct vbd *v, const uint64_t *n, bool at)
{
	uint64_t frozen = vbd_epoch(v);
	int ret = vbd_snapshot(v);

	if (ret) {
		fprintf(stderr, "vbd snapshot: %s\n", strerror(-ret));
		return 1;
	}
	printf("%" PRIu64 "\n", frozen);
	return 0;
}

static int cmd_serve(struct vbd *v, const uint64_t *n, bool at)
{
	int ret = ublk_serve(v, at);	/* a snapshot is served read-only */

	if (ret) {
		fprintf(stderr, "vbd serve: %s\n", strerror(-ret));
		return 1;
	}
	return 0;
}

static int cmd_write(struct vbd *v, const uint64_t *n, bool at)
{
	uint8_t *buf = malloc(BUF_SIZE);
	uint64_t off = n[0];
	size_t got;
	int ret = 0;

	while ((got = fread(buf, 1, BUF_SIZE, stdin)) > 0) {
		ssize_t put = vbd_write(v, buf, got, off);

		if (put < 0) {
			fprintf(stderr, "vbd write: %s\n", strerror(-put));
			ret = 1;
			goto out;
		}
		off += put;
		if ((size_t)put < got) {
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

static const struct cmd cmds[] = {
	{ "create", "<size> <chunk> [base]", "new volume; sizes take K, M, G suffixes",
	  2, 3, .make = cmd_create },
	{ "info", "[snapshot]", "print size, chunk size, epoch, chunks fetched, base state",
	  0, 1, .at = true, .run = cmd_info },
	{ "read", "<off> <len> [snapshot]", "copy a range to stdout, as of a snapshot if given",
	  2, 3, .at = true, .run = cmd_read },
	{ "write", "<off>", "copy stdin to the volume and flush",
	  1, 1, .run = cmd_write },
	{ "snapshot", "", "freeze the current content; prints the snapshot number",
	  0, 0, .run = cmd_snapshot },
	{ "serve", "[snapshot]", "expose the volume as /dev/ublkbN until SIGINT or SIGTERM; needs root",
	  0, 1, .at = true, .run = cmd_serve },
	{ NULL }
};
#define MAX_ARGS	3	/* the largest max above */

int main(int argc, char **argv)
{
	const struct cmd *c = cmds;
	uint64_t n[MAX_ARGS] = { 0 };
	const char *log;
	struct vbd *v;
	int nargs = argc - 3, ret;
	bool at;

	if (argc < 3)
		return usage();
	while (c->name && strcmp(c->name, argv[1]))
		c++;
	if (!c->name || nargs < c->min || nargs > c->max)
		return usage();
	log = argv[2];
	if (c->make)
		return c->make(log, argv + 3, nargs);
	for (int i = 0; i < nargs; i++)
		if (parse_size(argv[3 + i], &n[i]))
			return usage();
	at = c->at && nargs == c->max;

	rcu_register_thread();
	v = at ? vbd_open_at(log, n[nargs - 1]) : vbd_open(log);
	if (!v) {
		fprintf(stderr, "vbd: %s: %s\n", log, strerror(errno));
		return 1;
	}
	ret = c->run(v, n, at);
	vbd_close(v);
	return ret;
}
