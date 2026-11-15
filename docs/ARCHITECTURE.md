# Architecture

What is built today, drawn as blocks.

## Layers

```
 +----------------------------------------------------------------------+
 |  front ends                                                          |
 |                                                                      |
 |  +--------------+   +-----------------+   +-----------------------+  |
 |  |  vbd CLI     |   |  tests          |   |  vbd serve            |  |
 |  |  src/main.c  |   |  tests/*        |   |  src/ublk -> ublkbN   |  |
 |  +------+-------+   +--------+--------+   +------------+----------+  |
 +---------|---------------------|-----------------------|--------------+
           v                     v                       v
 +----------------------------------------------------------------------+
 |  libvbd                                               include/vbd.h  |
 |  vbd_create  vbd_open  vbd_close  vbd_read  vbd_write  vbd_flush     |
 +----------------------------------------------------------------------+
 |  engine                                                              |
 |                                                                      |
 |  +----------------------------------------------------------------+  |
 |  |  src/volume.c                                                  |  |
 |  |  a volume: cuts a byte range into chunks; a chunk is read from |  |
 |  |  the data log or, if it was never written, from the base cache |  |
 |  +-------+-------------------------+-----------+-----------+------+  |
 |          |                         |           |           |         |
 |          |                         |           v           |         |
 |          |                         |  +----------------+   |         |
 |          |                         |  | src/manifest.c |   |         |
 |          |                         |  | the manifest   |   |         |
 |          |                         |  | of a base      |   |         |
 |          |                         |  +----------------+   |         |
 |          |                         |           ^           |         |
 |          v                         v           |           v         |
 |  +----------------+       +------------------+ |    +-------------+  |
 |  |  src/log.c     |       |  src/cache.c     +-+    | src/super.c |  |
 |  |  a record log: | <---- |  the base cache: | ---> | the super   |  |
 |  |  records, map, |       |  fetch a chunk   |      | block: size |  |
 |  |  replay,       |       |  once and keep   |      | of volume,  |  |
 |  |  checkpoints,  |       |  it, work        |      | identity of |  |
 |  |  snapshots     |       |  offline         |      | its base    |  |
 |  +---+--------+---+       +--------+---------+      +------+------+  |
 |      |        :                    |                       |         |
 |      |        :                    v                       v         |
 |      |        :           +------------------------------------+     |
 |      |        :           |  src/base/      struct base_ops    |     |
 |      |        :           |  probe  open  read  close          |     |
 |      |        :           |  base.c picks  file.c  or  http.c  |     |
 |      |        :           +---------+----------------+---------+     |
 +------|--------:---------------------|----------------|---------------+
        v        v                     v                v
  +----------+ +----------------+   +-----------+  +-----------+
  |disk.log  | |disk.log.cache  |   | base.img  |  | https://  |
  |data log  | |fetched chunks  |   |   local   |  | Range GET |
  +----------+ +----------------+   +-----------+  +-----------+
               \__ only for a volume created with a base __/

 libraries: libxarray (deps/), liburcu, POSIX file I/O; libcurl is used by
 the http backend only.
```

Everything below `include/vbd.h` is the engine; everything above it is a
consumer. Inside the engine an arrow points from the file that calls to the
file that is called, and nothing calls upwards.

## A volume in memory and on disk

```
 struct vbd  (one per open volume)          disk.log
 +----------------------------+             +--------------------------+ 0
 |  size, chunk_size          |             |  super block, 4 KiB      |
 |  rec_size = 64 + chunk     |             +--------------------------+ 4096
 |                            |             |  rec 0 | hdr | chunk 5   |
 |  data (struct log)         |             +--------------------------+ 4096 + R
 |    fd           -----------+------------>|  rec 1 | hdr | chunk 0   |
 |    nrec = 3                |             +--------------------------+ 4096 + 2R
 |    append_lock (mutex)     |             |  rec 2 | hdr | chunk 5   |
 |    map (XArray)            |             +--------------------------+ 4096 + 3R
 |      chunk 0 -> rec 1      |                ^ next append            R = rec_size
 |      chunk 5 -> rec 2      |
 |      (chunk 1..4: empty)   |
 |                            |
 |  . . . . . . . . . . . . . |             base.img or URL  (immutable)
 |  . cache (struct cache)  . + . . . . . .>+-----+-----+-----+-----+-----+--
 |  .   base_ops, base      . |             | c0  | c1  | c2  | c3  | c4  | ...
 |  .   base_size           . |             +-----+-----+-----+-----+-----+--
 |  .   log (struct log)    . |             0     C     2C    3C    4C      C = chunk_size
 |  .   same shape as data  . + . . . . . .> disk.log.cache, same layout as disk.log
 |  . . . . . . . . . . . . . |
 +----------------------------+
    dotted: no base and no file when the volume was created without a base
```

- The map holds the **newest** record for each written chunk. Chunk 5 was
  written twice; rec 0 is now unreachable through the map but still in the
  file (P3 time travel reads it back).
- Record number and file offset are interchangeable:
  `rec_off(n) = 4096 + n * rec_size`. The map stores only the small integer
  `n` as an XArray value entry; no per-chunk allocation.
- A chunk that was zeroed or discarded has an entry too, with a reserved
  value in place of a record number: it reads as zeros without touching a
  file, and it hides the base.
- Chunks never written (1..4) have no entry. Without a base they read as
  zeros. With a base, reads of them go to the base cache, and on a miss
  there, to the base image. The cache holds a second `struct log` over
  `disk.log.cache` with exactly this layout.

## The ublk front end

`vbd serve` is one process with a main thread and a queue thread for every
CPU, talking to the kernel's ublk driver through three device nodes. Every arrow is an io_uring `URING_CMD`:
io_uring carries messages here, not data. The data moves when a queue
thread calls `pread` or `pwrite` on `/dev/ublkcN` at an offset that names
its queue and the tag (`UBLK_F_USER_COPY`), which copies between the pages
of the request and the thread's one buffer.

```
 vbd serve (one process, root)              kernel: ublk_drv
 +-----------------------------+            +---------------------------+
 |  main thread                |  URING_CMD |  /dev/ublk-control        |
 |  ctl_ring  -----------------+----------->|  one per system           |
 |  ADD_DEV, SET_PARAMS,       |            |  create, configure, start,|
 |  START_DEV ... sigwait ...  |            |  stop, delete device N    |
 |  STOP_DEV, DEL_DEV          |            +-------------+-------------+
 +-----------------------------+                          | creates
 |  queue thread, one per CPU  |  URING_CMD |  /dev/ublkcN  (char)      |
 |  ring  ---------------------+----------->|  the server's side of N:  |
 |  FETCH, COMMIT_AND_FETCH    |            |  one command parked per   |
 |  iod[64]  <-- mmap ---------+------------+  tag, iod[tag] describes  |
 |  buf  <-- pread / pwrite ---+------------+  the request for that tag |
 |  vbd_read vbd_write vbd_flush            +-------------+-------------+
 +--------------+--------------+                          ^ block requests
                |                                         |
                v                           +-------------+-------------+
 +-----------------------------+            |  /dev/ublkbN  (block)     |
 |  libvbd: disk.log, cache,   |            |  the users' side of N:    |
 |  base                       |            |  mkfs, mount, dd, a VM    |
 +-----------------------------+            +---------------------------+
```

The device has one queue per CPU. Each queue is a thread here with a ring,
a descriptor array and a buffer of its own, so nothing is shared between
queues but the volume, and a request issued on one CPU does not wait
behind one issued on another. One 256 KiB buffer is enough for a queue: its
thread serves one request at a time, and copies the data itself.

One request, by tag:

1. At start each queue thread parks a `FETCH_REQ` for each of its 64 tags
   on `/dev/ublkcN`. `START_DEV` returns once all of them are parked, and
   `/dev/ublkbN` appears.
2. Someone reads or writes `/dev/ublkbN`. The block layer forms a request
   on the queue of the CPU it runs on; the driver takes a free tag of that
   queue, writes op, sector and length into its `iod[tag]`, and completes
   the parked command. That queue's thread sees a CQE carrying the tag.
3. The thread reads `iod[tag]`. For a write it `pread`s the request's data
   into its buffer and calls `vbd_write`; for a read it calls `vbd_read`
   and `pwrite`s the result into the request; a flush is `vbd_flush`, a
   discard or a write of zeros `vbd_zero`, with no data to move.
4. It sends `COMMIT_AND_FETCH_REQ` with the tag and the result. The driver
   ends the block request and parks the command again for the next one.
5. On SIGINT or SIGTERM the main thread sends `STOP_DEV`: every parked
   command completes with an abort, the queue threads exit, the server
   closes `/dev/ublkcN`, and `DEL_DEV` removes both nodes.

## On-disk format

```
 super block (4096 B, written once by create, never rewritten)
 +-----+-------+---------+------------+------+-----------+-----------+---------------+-----------+----------+
 | crc | magic | version | chunk_size | size | base_size | parent_at | manifest_hash | base_tag  | base     |
 | u32 | u32   | u32     | u32        | u64  | u64       | u64       | u8[32]        | char[128] | char[..] |
 +-----+-------+---------+------------+------+-----------+-----------+---------------+-----------+----------+
   ^ crc covers everything after itself
   base: the base image as a path or URL, 3896 bytes; empty: the volume has no base.
   base_tag: the backend's validator at create (file mtime, or ETag / Last-Modified).
   manifest_hash: sha256 of the base's manifest, kept as disk.log.manifest:
   one sha256 per base chunk. All zero when the volume was created without one.
   parent_at: 2^64 - 1 unless the volume is a fork. For a fork it is the
   parent's snapshot the fork continues from, and base holds the path of the
   parent's log instead; a fork has no base, tag or manifest of its own.

 record (64 B header + chunk_size B data)
 +-------+-------+-------+-------+----------+------+--------------------------------+
 | magic | crc   | chunk | epoch | zero_cnt | pad  | data: exactly chunk_size bytes |
 | "vbd1"| u32   | u64   | u64   | u64      | 32 B |                                |
 +-------+-------+-------+-------+----------+------+--------------------------------+
           ^ crc covers the data               epoch: snapshots taken before this write

 zero_cnt 0: the data is the content of chunk. zero_cnt n: the n chunks
 from chunk on are zero, and the data is not used; that is how a discard
 of any length is one record.

 checkpoint record (the same size, a header of its own kind)
 +-------+-------+-------+------+-------+-------+------------+------+------+----------------+
 | magic | crc   | epoch | part | parts | count | next_epoch | time | pad  | data: count    |
 | "vbck"| u32   | u64   | u32  | u32   | u64   | u64        | u64  | 16 B | map entries    |
 +-------+-------+-------+------+-------+-------+------------+------+------+----------------+
           epoch: no record the checkpoint covers has a larger one
           next_epoch: what records after it are stamped with
           time: when a snapshot was taken, seconds since 1970; 0 otherwise
           a map entry is (chunk, record) as two u64

 Every header is 64 bytes and starts with magic and crc; the magic says how
 to read the rest. A checkpoint is `parts` consecutive checkpoint records
 holding the whole map in chunk order, `count` entries in each. It covers
 every record before its part 0.

 A snapshot is a checkpoint with next_epoch = epoch + 1: it is snapshot
 number `epoch`, the map it holds is the content that snapshot froze, it is
 what makes the step to the next epoch last, and its time is how the
 snapshot is found by date. Any other checkpoint
 has next_epoch = epoch.
```

A record always carries a whole chunk, even for a one-byte write. That costs
write amplification and buys a format with no index the log depends on.

The checkpoint is an index, but a disposable one, and it lives in the log
like everything else: a volume is one file and appending is the only way
it is written.

```
 disk.log
 +-------+-------+-------+ ... +-------+----------+----------+-------+-------+
 | super | rec 0 | rec 1 |     | rec k | ckpt 0/2 | ckpt 1/2 | rec   | rec   |
 +-------+-------+-------+ ... +-------+----------+----------+-------+-------+
          \_____ covered: not read at open ____/  \_ loaded _/ \_ replayed _/
```

Open walks the 64-byte headers back from the end of the file until it
meets the last part of a checkpoint whose parts all verify, loads the map
from it and replays the records after it. With no checkpoint, or only torn
ones, it replays from record 0 as before.

A view of snapshot N looks for the newest checkpoint with epoch <= N
instead. Normally that is the one snapshot N wrote, and then nothing is
replayed at all: the map is the snapshot, every record before it is in the
map and every record after it is newer. The headers passed on the way
back tell how far the log has got since.

```
 view of snapshot 1
 +-------+-------+-------+----------------+-------+----------------+-------+
 | super | rec   | rec   | ckpt  epoch 0  | rec   | ckpt  epoch 1  | rec   |
 |       | @0    | @0    |       next  1  | @1    |       next  2  | @2    |
 +-------+-------+-------+----------------+-------+----------------+-------+
          \________ not read ____________________/ \__ loaded ____/  ^ header only
```

It is written at flush and at close, after an `fdatasync` so that it never
describes records a power cut can still take, and only when 64 MiB of
records have piled up behind the last one (about a tenth of a second of
replay, see below) and at least four times as many records as that
one took: the log never shrinks, so checkpoints must stay a small part of
it. The base cache is never synced, since it can be refetched, so it
writes its checkpoints at close only: after a crash the next open replays
the cache from its last one.

Replay reads and checksums every byte of every record, so its cost goes by
bytes, not records. Measured on the development machine, opening a log
with no checkpoint, from the page cache:

```
 chunk    records   log size    open
 4 KiB    4000      15 MiB      0.03 s
 64 KiB   4000      250 MiB     0.46 s
 1 MiB    1000      1000 MiB    1.83 s      about 540 MiB/s throughout
```
