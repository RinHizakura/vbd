# Architecture

What is built today, drawn as blocks. Dotted boxes are optional: everything
on the base side exists only when the volume was created with a base image.

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
 |  engine                                               src/volume.c   |
 |                                                                      |
 |  always                         . . only if created with a base . .  |
 |  +---------------------------+  .                                 .  |
 |  |  data log                 |  .  +---------------------------+  .  |
 |  |  XArray: chunk -> rec no. |  .  |  base cache               |  .  |
 |  |  file: disk.log           |  .  |  XArray: chunk -> rec no. |  .  |
 |  +---------------------------+  .  |  file: disk.log.cache     |  .  |
 |                                 .  +-------------+-------------+  .  |
 |  a read looks in the data log,  .           miss | fetch_chunk    .  |
 |  then the base cache; with no   .                v                .  |
 |  base an unwritten chunk reads  .  +---------------------------+  .  |
 |  as zeros                       .  |  base_ops src/base/base.h |  .  |
 |                                 .  |  probe open read close    |  .  |
 |                                 .  +-----+---------------+-----+  .  |
 |                                 .        |    one of     |        .  |
 |                                 .  +-----+------+  +-----+------+ .  |
 |                                 .  | base/file.c|  | base/http.c| .  |
 |                                 .  | pread      |  | libcurl    | .  |
 |                                 .  +-----+------+  +-----+------+ .  |
 |                                 . . . . .|. . . . . . . .|. . . . .  |
 +------------------------------------------|---------------|-----------+
                                            v               v
                                      +-----------+   +-----------+
                                      |  base.img |   |  https:// |
                                      |  local    |   | Range GET |
                                      +-----------+   +-----------+

 libraries: libxarray (deps/), liburcu, POSIX file I/O; libcurl is used by
 the http backend only.
```

Everything below `include/vbd.h` is the engine. Everything above it is a
consumer: the CLI, the tests and the ublk server. The engine has no
knowledge of what sits above it; the server is one queue thread that turns
block requests into vbd_read and vbd_write calls.

The base is reached only from `fetch_chunk`, and only through `struct
base_ops` (`src/base/base.h`). The backend is picked from the name given to
`create`; the engine never knows which one it got, and never needs to.

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
 |  . base_ops, base  . . . . + . . . . . .>+-----+-----+-----+-----+-----+--
 |  . base_size             . |             | c0  | c1  | c2  | c3  | c4  | ...
 |  .                       . |             +-----+-----+-----+-----+-----+--
 |  . cache (struct log)    . |             0     C     2C    3C    4C      C = chunk_size
 |  .   same shape as data  . + . . . . . .> disk.log.cache, same layout as disk.log
 |  . . . . . . . . . . . . . |
 +----------------------------+
    dotted: NULL and unused when the volume was created without a base
```

- The map holds the **newest** record for each written chunk. Chunk 5 was
  written twice; rec 0 is now unreachable through the map but still in the
  file (P3 time travel reads it back).
- Record number and file offset are interchangeable:
  `rec_off(n) = 4096 + n * rec_size`. The map stores only the small integer
  `n` as an XArray value entry; no per-chunk allocation.
- Chunks never written (1..4) have no entry. Without a base they read as
  zeros. With a base, reads of them go to the base cache, and on a miss
  there, to the base image. The cache is a second `struct log` over
  `disk.log.cache` with exactly this layout.

## The ublk front end

`vbd serve` is one process with two threads, talking to the kernel's ublk
driver through three device nodes. Every arrow is an io_uring `URING_CMD`:
io_uring carries messages here, not data; the data moves by `copy_to_user`
and `copy_from_user` into buffers the server owns.

```
 vbd serve (one process, root)              kernel: ublk_drv
 +-----------------------------+            +---------------------------+
 |  main thread                |  URING_CMD |  /dev/ublk-control        |
 |  ctl_ring  -----------------+----------->|  one per system           |
 |  ADD_DEV, SET_PARAMS,       |            |  create, configure, start,|
 |  START_DEV ... sigwait ...  |            |  stop, delete device N    |
 |  STOP_DEV, DEL_DEV          |            +-------------+-------------+
 +-----------------------------+                          | creates
 |  queue thread               |  URING_CMD |  /dev/ublkcN  (char)      |
 |  io_ring  ------------------+----------->|  the server's side of N:  |
 |  FETCH, COMMIT_AND_FETCH    |            |  one command parked per   |
 |  iod[64]  <-- mmap ---------+------------+  tag, iod[tag] describes  |
 |  buf[64]  <-- copy to/from -+------------+  the request for that tag |
 |  vbd_read vbd_write vbd_flush            +-------------+-------------+
 +--------------+--------------+                          ^ block requests
                |                                         |
                v                           +-------------+-------------+
 +-----------------------------+            |  /dev/ublkbN  (block)     |
 |  libvbd: disk.log, cache,   |            |  the users' side of N:    |
 |  base                       |            |  mkfs, mount, dd, a VM    |
 +-----------------------------+            +---------------------------+
```

One request, by tag:

1. At start the queue thread parks a `FETCH_REQ` for each of the 64 tags on
   `/dev/ublkcN`. `START_DEV` returns once all are parked, and `/dev/ublkbN`
   appears.
2. Someone reads or writes `/dev/ublkbN`. The block layer forms a request;
   the driver takes a free tag, writes op, sector and length into
   `iod[tag]`, copies the data into `buf[tag]` for a write, and completes
   the parked command. The queue thread sees a CQE carrying the tag.
3. The thread reads `iod[tag]` and calls `vbd_read`, `vbd_write` or
   `vbd_flush` on `buf[tag]`.
4. It sends `COMMIT_AND_FETCH_REQ` with the tag and the result. The driver
   copies `buf[tag]` out for a read, ends the block request, and parks the
   command again for the next one.
5. On SIGINT or SIGTERM the main thread sends `STOP_DEV`: every parked
   command completes with an abort, the queue thread exits, the server
   closes `/dev/ublkcN`, and `DEL_DEV` removes both nodes.

## On-disk format

```
 super block (4096 B, written once by create, never rewritten)
 +-------+-------+---------+------------+------+-----------+-----------+---------------+
 | crc   | magic | version | chunk_size | size | base_size | base_tag  | base path/URL |
 | u32   | u32   | u32     | u32        | u64  | u64       | char[128] | char[3936]    |
 +-------+-------+---------+------------+------+-----------+-----------+---------------+
   ^       crc covers everything after itself       base_tag: the backend's validator at
                                                    create (file mtime, or ETag / Last-Modified)
   base_size 0 and an empty base path/URL mean the volume has no base.

 record (64 B header + chunk_size B data)
 +-------+-------+-------+-------+----------+------------------------------------+
 | magic | crc   | chunk | epoch | pad      | data: exactly chunk_size bytes     |
 | u32   | u32   | u64   | u64   | 40 B     |                                    |
 +-------+-------+-------+-------+----------+------------------------------------+
           ^ crc covers the data               epoch: snapshots taken before this write

 A snapshot appends a record with chunk = 2^64 - 1 and the new epoch; it
 persists the epoch and is never entered in the map.
```

A record always carries a whole chunk, even for a one-byte write. That costs
write amplification and buys a format with no index on disk.
