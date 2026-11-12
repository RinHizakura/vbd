// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ublk front end: serve a volume as /dev/ublkbN.
 *
 * The kernel's ublk driver turns block requests into descriptors in a
 * buffer it shares with us by mmap, and signals each one as the completion
 * of an io_uring URING_CMD on /dev/ublkcN. We answer with vbd_read or
 * vbd_write on a per-tag bounce buffer and commit the result with another
 * URING_CMD, which also fetches the next request for that tag. Control
 * (add, params, start, stop, delete) goes the same way to /dev/ublk-control.
 *
 * ponytail: one queue, one thread, bounce buffers. Add queues when one core
 * cannot keep up, zero copy when the memcpy shows up in a profile.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <liburing.h>
#include <urcu.h>

#include "ublk_cmd.h"
#include "ublk.h"

#define DEPTH		64			/* requests in flight */
#define MAX_IO		(256u << 10)		/* bytes per request */
#define CTRL_DEV	"/dev/ublk-control"

struct srv {
	struct vbd *v;
	bool readonly;
	int ctl, ch;				/* ublk-control, ublkcN */
	int dev_id;				/* N, or -1 before ADD_DEV */
	bool rings;				/* both rings initialised */
	struct io_uring ctl_ring, io_ring;
	struct ublksrv_io_desc *iod;		/* DEPTH descriptors, mapped from the driver */
	size_t iod_bytes;
	uint8_t *buf;				/* DEPTH bounce buffers of MAX_IO */
	int hs[2];				/* handshake pipe, queue thread -> main */
};

static void prep_cmd(struct io_uring *ring, int fd, unsigned op,
		     const void *cmd, size_t len, uint64_t user_data)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	io_uring_prep_rw(IORING_OP_URING_CMD, sqe, fd, NULL, 0, 0);
	sqe->cmd_op = op;
	sqe->user_data = user_data;
	memcpy(sqe->cmd, cmd, len);
}

/* One control command, synchronously; returns the driver's result. */
static int ctrl(struct srv *s, unsigned op, struct ublksrv_ctrl_cmd *c)
{
	struct io_uring_cqe *cqe;
	int ret;

	c->dev_id = s->dev_id;
	c->queue_id = -1;
	prep_cmd(&s->ctl_ring, s->ctl, op, c, sizeof(*c), 0);
	ret = io_uring_submit(&s->ctl_ring);
	if (ret < 0)
		return ret;
	ret = io_uring_wait_cqe(&s->ctl_ring, &cqe);
	if (ret)
		return ret;
	ret = cqe->res;
	io_uring_cqe_seen(&s->ctl_ring, cqe);
	return ret;
}

static int add_dev(struct srv *s)
{
	struct ublksrv_ctrl_dev_info info = {
		.nr_hw_queues = 1, .queue_depth = DEPTH,
		.max_io_buf_bytes = MAX_IO, .dev_id = -1,
	};
	struct ublksrv_ctrl_cmd c = { .addr = (uintptr_t)&info, .len = sizeof(info) };
	int ret = ctrl(s, UBLK_U_CMD_ADD_DEV, &c);

	if (ret < 0)
		return ret;
	s->dev_id = info.dev_id;		/* filled in by the driver */
	return 0;
}

static int set_params(struct srv *s)
{
	unsigned chunk_shift = __builtin_ctz(vbd_chunk_size(s->v));
	struct ublk_params p = {
		.len = sizeof(p), .types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			/* flushes reach us as UBLK_IO_OP_FLUSH */
			.attrs = UBLK_ATTR_VOLATILE_CACHE |
				 (s->readonly ? UBLK_ATTR_READ_ONLY : 0),
			.logical_bs_shift = 9,
			.physical_bs_shift = chunk_shift < 12 ? chunk_shift : 12,
			.io_min_shift = chunk_shift < 12 ? chunk_shift : 12,
			.io_opt_shift = chunk_shift,	/* a chunk is the ideal I/O */
			.max_sectors = MAX_IO >> 9,
			.dev_sectors = vbd_size(s->v) >> 9,
		},
	};
	struct ublksrv_ctrl_cmd c = { .addr = (uintptr_t)&p, .len = sizeof(p) };

	return ctrl(s, UBLK_U_CMD_SET_PARAMS, &c);
}

static int open_char_dev(struct srv *s)
{
	char path[32];

	snprintf(path, sizeof(path), "/dev/ublkc%d", s->dev_id);
	for (int i = 0; i < 100; i++) {		/* devtmpfs may need a moment */
		s->ch = open(path, O_RDWR);
		if (s->ch >= 0)
			return 0;
		if (errno != ENOENT)
			return -errno;
		usleep(10000);
	}
	return -ENOENT;
}

static int setup(struct srv *s)
{
	size_t page = sysconf(_SC_PAGESIZE);
	int ret;

	s->ctl = open(CTRL_DEV, O_RDWR);
	if (s->ctl < 0)
		return -errno;
	if ((ret = io_uring_queue_init(4, &s->ctl_ring, IORING_SETUP_SQE128)))
		return ret;
	if ((ret = io_uring_queue_init(DEPTH, &s->io_ring, IORING_SETUP_SQE128))) {
		io_uring_queue_exit(&s->ctl_ring);
		return ret;
	}
	s->rings = true;
	if ((ret = add_dev(s)) < 0 || (ret = set_params(s)) < 0 ||
	    (ret = open_char_dev(s)))
		return ret;

	s->iod_bytes = (DEPTH * sizeof(*s->iod) + page - 1) / page * page;
	s->iod = mmap(NULL, s->iod_bytes, PROT_READ, MAP_SHARED, s->ch,
		      UBLKSRV_CMD_BUF_OFFSET);	/* queue 0 */
	if (s->iod == MAP_FAILED) {
		s->iod = NULL;
		return -errno;
	}
	s->buf = malloc((size_t)DEPTH * MAX_IO);
	if (!s->buf || pipe(s->hs))
		return -errno;
	return 0;
}

/* Undo setup in reverse. STOP_DEV first: it aborts the pending fetches, which
 * ends the queue thread; DEL_DEV last: it waits for ublkcN to be closed. */
static void teardown(struct srv *s, pthread_t *queue)
{
	struct ublksrv_ctrl_cmd c = {};

	if (s->dev_id >= 0)
		ctrl(s, UBLK_U_CMD_STOP_DEV, &c);
	if (queue)
		pthread_join(*queue, NULL);
	if (s->iod)
		munmap(s->iod, s->iod_bytes);
	if (s->ch >= 0)
		close(s->ch);
	if (s->dev_id >= 0)
		ctrl(s, UBLK_U_CMD_DEL_DEV, &c);
	free(s->buf);
	if (s->hs[0] >= 0) {
		close(s->hs[0]);
		close(s->hs[1]);
	}
	if (s->rings) {
		io_uring_queue_exit(&s->io_ring);
		io_uring_queue_exit(&s->ctl_ring);
	}
	if (s->ctl >= 0)
		close(s->ctl);
}

static void queue_cmd(struct srv *s, unsigned tag, unsigned op, int result)
{
	struct ublksrv_io_cmd c = {
		.tag = tag, .result = result,
		.addr = (uintptr_t)(s->buf + (size_t)tag * MAX_IO),
	};

	prep_cmd(&s->io_ring, s->ch, op, &c, sizeof(c), tag);
}

/* Serve the request the driver left for @tag; returns bytes or -errno. */
static int do_io(struct srv *s, unsigned tag)
{
	const struct ublksrv_io_desc *d = &s->iod[tag];
	uint8_t *buf = s->buf + (size_t)tag * MAX_IO;
	uint64_t off = d->start_sector << 9;
	size_t len = (size_t)d->nr_sectors << 9;

	switch (ublksrv_get_op(d)) {
	case UBLK_IO_OP_READ:	return vbd_read(s->v, buf, len, off);
	case UBLK_IO_OP_WRITE:	return vbd_write(s->v, buf, len, off);
	case UBLK_IO_OP_FLUSH:	return vbd_flush(s->v);
	default:		return -EOPNOTSUPP;
	}
}

/*
 * The queue's daemon: the driver binds each tag to the thread that fetched
 * it, so every FETCH and COMMIT for the queue is issued from here. Ends when
 * every tag has been aborted, which STOP_DEV causes.
 */
static void *queue_thread(void *arg)
{
	struct srv *s = arg;
	unsigned live = DEPTH;
	int ret;

	rcu_register_thread();
	for (unsigned tag = 0; tag < DEPTH; tag++)
		queue_cmd(s, tag, UBLK_U_IO_FETCH_REQ, 0);
	ret = io_uring_submit(&s->io_ring);
	ret = ret == DEPTH ? 0 : ret < 0 ? ret : -EIO;
	if (write(s->hs[1], &ret, sizeof(ret)) != sizeof(ret) || ret)
		goto out;

	while (live) {
		struct io_uring_cqe *cqe;
		unsigned head, n = 0;

		ret = io_uring_submit_and_wait(&s->io_ring, 1);
		if (ret == -EINTR)
			continue;
		if (ret < 0)
			break;
		io_uring_for_each_cqe(&s->io_ring, head, cqe) {
			unsigned tag = cqe->user_data;

			n++;
			if (cqe->res != UBLK_IO_RES_OK) {	/* tag retired */
				if (cqe->res != UBLK_IO_RES_ABORT)
					fprintf(stderr, "vbd serve: tag %u: %s\n",
						tag, strerror(-cqe->res));
				live--;
				continue;
			}
			queue_cmd(s, tag, UBLK_U_IO_COMMIT_AND_FETCH_REQ, do_io(s, tag));
		}
		io_uring_cq_advance(&s->io_ring, n);
	}
out:
	rcu_unregister_thread();
	return NULL;
}

int ublk_serve(struct vbd *v, bool readonly)
{
	struct srv s = { .v = v, .readonly = readonly, .ctl = -1, .ch = -1,
			 .dev_id = -1, .hs = { -1, -1 } };
	struct ublksrv_ctrl_cmd start = { .data[0] = getpid() };
	pthread_t queue;
	sigset_t set;
	int ret, sig;

	if (vbd_chunk_size(v) < 512)
		return -EINVAL;		/* sectors must not split a chunk */
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, NULL);	/* for sigwait below; threads inherit */

	if ((ret = setup(&s)) ||
	    (ret = -pthread_create(&queue, NULL, queue_thread, &s))) {
		teardown(&s, NULL);
		return ret;
	}
	if (read(s.hs[0], &ret, sizeof(ret)) != sizeof(ret))
		ret = -EIO;
	if (!ret)	/* the driver waits here until every tag has been fetched */
		ret = ctrl(&s, UBLK_U_CMD_START_DEV, &start);
	if (ret < 0) {
		teardown(&s, &queue);
		return ret;
	}

	printf("/dev/ublkb%d\n", s.dev_id);
	fflush(stdout);
	sigwait(&set, &sig);
	teardown(&s, &queue);
	return 0;
}
