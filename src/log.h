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
 * data record carries one whole chunk, or says that a run of chunks is
 * zero; the map points each chunk at the newest record written for it, or
 * marks it as zero.
 *
 * Every header is HDR_SIZE bytes and starts with magic and crc; the magic
 * says which kind the rest is. Two kinds: struct rec_hdr and struct
 * ckpt_hdr.
 *
 * The super block in front belongs to whoever owns the log: log_open hands
 * it over as bytes and the owner says what geometry it describes.
 *
 * Now and then the log appends its own map to itself as a checkpoint, so
 * that the next load can start from there instead of from record 0. The
 * log stays one file, and append stays the only way it is written. Callers
 * do not see this: it happens inside load, sync, snapshot and close.
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

/* How every record header starts, whatever its kind. */
struct hdr_start {
	uint32_t magic;		/* which kind */
	uint32_t crc;		/* crc32c of the data that follows */
};

struct rec_hdr {
	uint32_t magic;		/* VBD_MAGIC */
	uint32_t crc;
	uint64_t chunk;
	uint64_t epoch;		/* snapshots taken before this record was written */
	/*
	 * 0: the data is the content of @chunk. n: the n chunks from @chunk on
	 * are zero, and the data is not used.
	 */
	uint64_t zero_cnt;
	uint8_t  pad[HDR_SIZE - 32];
};

/*
 * Header of a checkpoint record. A checkpoint is the log's map, appended to
 * the log as one or more consecutive records of this kind.
 */
struct ckpt_hdr {
	uint32_t magic;		/* CKPT_MAGIC */
	uint32_t crc;		/* crc32c of the data that follows */
	uint64_t epoch;		/* no record it covers has a larger one */
	uint32_t part;		/* this record is part @part ... */
	uint32_t parts;		/* ... of @parts */
	uint64_t count;		/* map entries in the data: (chunk, record) as two uint64_t */
	/*
	 * What records after it are stamped with: @epoch, or @epoch + 1 when
	 * the checkpoint is a snapshot, the one numbered @epoch.
	 */
	uint64_t next_epoch;
	uint64_t time;		/* a snapshot: when it was taken, in seconds since 1970; else 0 */
	uint8_t  pad[HDR_SIZE - 48];
};
_Static_assert(sizeof(struct rec_hdr) == HDR_SIZE && sizeof(struct ckpt_hdr) == HDR_SIZE,
	       "record headers");

/*
 * Checkpoint once this many bytes of log would have to be replayed. Replay
 * reads and checksums every byte: about 540 MiB/s from the page cache on
 * the machine this was measured on, whatever the chunk size, so this bounds
 * an open after a crash to roughly a tenth of a second there.
 */
#define CKPT_BYTES	(64u << 20)

/* What log_at says of a log that is not a view: the whole log, as it is now. */
#define ALL_EPOCHS	UINT64_MAX

/* What a log is loaded as. */
enum log_as {
	/*
	 * To be appended to by this open alone. -EBUSY if another open of the
	 * same file, in this process or any other, is the writer already; it
	 * stays the writer until it closes.
	 */
	LOG_WRITER,
	/*
	 * To be appended to by several opens, in any number of processes, that
	 * take turns at the file. Each has its turn inside load, an append,
	 * log_refresh and close, and reads in there what the others appended;
	 * between turns it knows the file as it was at its last one.
	 */
	LOG_SHARED,
	/*
	 * A view of snapshot @arg: records with a larger epoch are left out of
	 * the map, and the log must not be appended to. -ENOENT if there is no
	 * such snapshot. A view never changes the file, torn tail included, so
	 * it can be loaded while someone else is appending.
	 */
	LOG_AT,
	/*
	 * The same, of the newest snapshot taken at or before @arg, seconds
	 * since 1970. -ENOENT if there is none.
	 */
	LOG_BEFORE,
};

/* What is in here is for log.c; everyone else uses the functions. */
struct log {
	int fd;
	uint32_t chunk_size;	/* bytes of data in a record */
	uint64_t nchunks;	/* chunks are numbered 0 .. nchunks - 1 */
	uint64_t upto;		/* ALL_EPOCHS, or the last epoch this view of the log shows */
	bool shared;		/* LOG_SHARED: flock the file for every turn at it */
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
 * Life of a log: init, open, load, then append / read / snapshot / sync as
 * needed, close. close is safe after init at any point.
 */
/* Create @path holding only the SUPER_SIZE bytes at @super; fails if it exists. */
int log_create(const char *path, const void *super);

void log_init(struct log *l);
/* Open @path and copy its first SUPER_SIZE bytes to @super. 0 or -errno. */
int log_open(struct log *l, const char *path, void *super);

/*
 * Build the map from the file, given the geometry the super block
 * describes, as what @as says; @arg is what LOG_AT and LOG_BEFORE ask for.
 * Later records override earlier ones. The first record that does not
 * verify marks a torn tail, and the file is truncated there, except by a
 * view.
 */
int log_load(struct log *l, uint32_t chunk_size, uint64_t nchunks,
	     enum log_as as, uint64_t arg);

/* The snapshot a view shows, or ALL_EPOCHS. */
uint64_t log_at(const struct log *l);

/*
 * A shared log: read in what the other opens of the file appended since
 * this one's last turn. 0 or -errno; after an error the log can no longer
 * be appended to and takes no checkpoints.
 */
int log_refresh(struct log *l);

/* Make every record appended so far durable. */
int log_sync(struct log *l);
void log_close(struct log *l);

/* Append one whole chunk and publish it in the map. */
int log_append(struct log *l, uint64_t chunk, const uint8_t *data);

/*
 * Append the statement that the @count chunks from @chunk on are zero: one
 * record, however many chunks. The log holds them afterwards, and they read
 * as zeros.
 */
int log_append_zero(struct log *l, uint64_t chunk, uint64_t count);

/* What log_version says of a chunk the log does not hold. */
#define LOG_ABSENT	UINT64_MAX

/*
 * Which version of @chunk the log holds now: a number that is different
 * after every append of that chunk, or LOG_ABSENT.
 */
uint64_t log_version(const struct log *l, uint64_t chunk);

/*
 * Append @data for @chunk only if the log still holds @version of it.
 * -EAGAIN, and nothing written, if the chunk was appended in the meantime.
 *
 * For changing part of a chunk without a lock: note the version, read the
 * chunk, change it, append; on -EAGAIN someone else got in first, so do it
 * again on top of theirs.
 *
 * With LOG_ABSENT the chunk is appended only if the log does not hold it:
 * the first to arrive stays. On a shared log that counts what the other
 * opens appended, so the file holds the chunk once.
 */
int log_append_if(struct log *l, uint64_t chunk, const uint8_t *data, uint64_t version);

/*
 * Take a snapshot: step the epoch, in a way that survives a reopen with no
 * further writes, and keep the map as it is now for views of the snapshot.
 */
int log_snapshot(struct log *l);

/* Does the log hold @chunk? Once it does, it always will. */
bool log_has(const struct log *l, uint64_t chunk);

/* The first chunk the log holds in [@from, @end), or @end if it holds none of them. */
uint64_t log_first(const struct log *l, uint64_t from, uint64_t end);

/*
 * Read @len bytes of @chunk, as it was last appended, starting @within
 * bytes into it. -ENOENT if the log does not hold the chunk.
 */
int log_read(struct log *l, uint64_t chunk, uint8_t *buf, uint32_t within, uint32_t len);

/* How many different chunks the log holds. */
uint64_t log_chunks(const struct log *l);
/* The epoch new records are stamped with: the number of snapshots taken. */
uint64_t log_epoch(const struct log *l);

#endif /* VBD_LOG_H */
