/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * An append-only file of fixed-size records and the map that indexes it
 * (host byte order for now):
 *
 *   [super 4 KiB][record 0][record 1]...
 *   record = [header 64 B][chunk_size bytes of data]
 *
 * Records have a fixed size, so record number <-> file offset is arithmetic
 * and the XArray only has to store the record number as a value entry. A
 * data record carries one whole chunk; the map points each chunk at the
 * newest record written for it.
 *
 * Every header is HDR_SIZE bytes and starts with magic and crc; the magic
 * says which kind the rest is. Two kinds: struct rec_hdr here, and the
 * checkpoint's in ckpt.c.
 *
 * The super block in front belongs to whoever owns the log: log_open hands
 * it over as bytes and the owner says what geometry it describes.
 *
 * Now and then the log appends its own map to itself as a checkpoint
 * (ckpt.c), so that the next load can start from there instead of from
 * record 0. Callers do not see this: it happens inside load, sync and close.
 */
#ifndef VBD_LOG_H
#define VBD_LOG_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <xarray.h>

#define VBD_MAGIC	0x31646276u	/* "vbd1": super block, data record */
#define CKPT_MAGIC	0x6b636276u	/* "vbck": checkpoint record */
#define HDR_SIZE	64
#define SUPER_SIZE	4096
/* A record has to hold at least one map entry: checkpoints are made of them. */
#define MIN_CHUNK_SIZE	16

struct rec_hdr {
	uint32_t magic;
	uint32_t crc;		/* crc32c of the data that follows */
	uint64_t chunk;
	uint64_t epoch;		/* snapshots taken before this record was written */
	uint8_t  pad[HDR_SIZE - 24];
};

/* log_load's @upto for the whole log, as it is now */
#define ALL_EPOCHS	UINT64_MAX

/* What is in here is for log.c and ckpt.c; everyone else uses the functions. */
struct log {
	int fd;
	uint32_t chunk_size;	/* bytes of data in a record */
	uint64_t nchunks;	/* chunks are numbered 0 .. nchunks - 1 */
	uint64_t upto;		/* ALL_EPOCHS, or the last epoch this view of the log shows */
	bool loaded;		/* log_load finished: the map describes the file */
	uint64_t nrec;		/* records in the file; next append goes here */
	uint64_t ckpt_nrec;	/* nrec right after the last checkpoint */
	uint32_t ckpt_parts;	/* records that checkpoint took */
	struct xarray map;	/* chunk -> xa_mk_value(record number), nothing else */
	uint64_t mapped;	/* chunks the map holds */
	pthread_mutex_t append_lock;
	uint64_t epoch;		/* what new records are stamped with */
};

/*
 * Life of a log: init, open, lock if it is to be written, load, then
 * append / read / snapshot / sync as needed, close. close is safe after
 * init at any point.
 */
/* Create @path holding only the SUPER_SIZE bytes at @super; fails if it exists. */
int log_create(const char *path, const void *super);

void log_init(struct log *l);
/* Open @path and copy its first SUPER_SIZE bytes to @super. 0 or -errno. */
int log_open(struct log *l, const char *path, void *super);

/*
 * Claim the open log for writing, before loading it. -EBUSY if another open
 * of the same file, in this process or any other, holds the claim; it lasts
 * until close. A view needs none and can be loaded beside the writer.
 */
int log_lock(struct log *l);

/*
 * Build the map from the file, given the geometry the super block
 * describes. Later records override earlier ones. The first record that
 * does not verify marks a torn tail, and the file is truncated there.
 *
 * With @upto below ALL_EPOCHS the result is a view: records with a larger
 * epoch are left out of the map, and the log must not be appended to. A
 * view never changes the file, torn tail included, so it can be loaded
 * while someone else is appending.
 */
int log_load(struct log *l, uint32_t chunk_size, uint64_t nchunks, uint64_t upto);

/*
 * Read in the records that another open of the same file appended since
 * this one loaded or last caught up. The caller keeps every other open from
 * appending meanwhile, here and for as long as it goes on to append itself:
 * a log does not look at the file again on its own. 0 or -errno; after an
 * error, or before the log was loaded, the log takes no checkpoints.
 */
int log_catch_up(struct log *l);

/* Make every record appended so far durable. */
int log_sync(struct log *l);
void log_close(struct log *l);

/* Append one whole chunk and publish it in the map. */
int log_append(struct log *l, uint64_t chunk, const uint8_t *data);

/*
 * Take a snapshot: step the epoch, in a way that survives a reopen with no
 * further writes, and keep the map as it is now for views of the snapshot.
 */
int log_snapshot(struct log *l);

/* Does the log hold @chunk? Once it does, it always will. */
bool log_has(const struct log *l, uint64_t chunk);

/*
 * Read @len bytes of @chunk, as it was last appended, starting @within
 * bytes into it. -ENOENT if the log does not hold the chunk.
 */
int log_read(struct log *l, uint64_t chunk, uint8_t *buf, uint32_t within, uint32_t len);

/* How many different chunks the log holds. */
uint64_t log_chunks(const struct log *l);
/* The epoch new records are stamped with: the number of snapshots taken. */
uint64_t log_epoch(const struct log *l);
/* The open file, for a caller that locks it (see log_catch_up). */
int log_fd(const struct log *l);

/* For code that writes records of its own kind (ckpt.c). */
/* Point @chunk at record @rec in the map. 0 or -errno. */
int log_map(struct log *l, uint64_t chunk, uint64_t rec);
uint64_t rec_off(const struct log *l, uint64_t rec);
/* Write a header of any kind and @data as record l->nrec; append_lock held. */
int rec_write(struct log *l, const void *hdr, const void *data);
/* pread all of it; past the end of the file reads as zeros. 0 or -errno. */
int full_pread(int fd, void *buf, size_t len, uint64_t off);

#endif /* VBD_LOG_H */
