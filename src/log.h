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
 */
#ifndef VBD_LOG_H
#define VBD_LOG_H

#include <pthread.h>
#include <stdint.h>

#include <xarray.h>

#define VBD_MAGIC	0x31646276u	/* "vbd1": super block, data record, snapshot marker */
#define CKPT_MAGIC	0x6b636276u	/* "vbck": checkpoint record */
#define HDR_SIZE	64
#define SUPER_SIZE	4096

struct rec_hdr {
	uint32_t magic;
	uint32_t crc;		/* crc32c of the data that follows */
	uint64_t chunk;
	uint64_t epoch;		/* snapshots taken before this record was written */
	uint8_t  pad[HDR_SIZE - 24];
};

/* log_replay's @upto when nothing is to be left out */
#define ALL_EPOCHS	UINT64_MAX

struct log {
	int fd;
	uint32_t chunk_size;	/* bytes of data in a record; the owner sets these two ... */
	uint64_t nchunks;	/* ... chunks are numbered 0 .. nchunks - 1 */
	uint64_t nrec;		/* records in the file; next append goes here */
	uint64_t ckpt_nrec;	/* nrec right after the last checkpoint */
	uint32_t ckpt_parts;	/* records that checkpoint took */
	struct xarray map;	/* chunk -> xa_mk_value(record number) */
	pthread_mutex_t append_lock;
	uint64_t epoch;		/* what new records are stamped with */
};

void log_init(struct log *l);
/* Open @path and copy its first SUPER_SIZE bytes to @super. 0 or -errno. */
int log_open(struct log *l, const char *path, void *super);
void log_close(struct log *l);

/*
 * Bring the map up to date with the file, from record l->nrec on (0, or
 * just past a checkpoint). Later records override earlier ones; records
 * with an epoch above @upto are left out. The first record that does not
 * verify marks a torn tail, and the file is truncated there.
 */
int log_replay(struct log *l, uint64_t upto);

/* Append one whole chunk and publish it in the map. */
int log_append(struct log *l, uint64_t chunk, const uint8_t *data);

/*
 * Advance the epoch and append a marker record that carries the new one,
 * so the step survives a reopen with no further writes.
 */
int log_snapshot(struct log *l);

/* Read @len bytes of record @rec starting @within bytes into its data. */
int log_read(struct log *l, uint64_t rec, uint8_t *buf, uint32_t within, uint32_t len);

/* For code that writes records of its own kind (ckpt.c). */
uint64_t rec_off(const struct log *l, uint64_t rec);
/* Write a header of any kind and @data as record l->nrec; append_lock held. */
int rec_write(struct log *l, const void *hdr, const void *data);
/* pread all of it; past the end of the file reads as zeros. 0 or -errno. */
int full_pread(int fd, void *buf, size_t len, uint64_t off);

#endif /* VBD_LOG_H */
