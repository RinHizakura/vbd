# Architecture

What is built today (P1), drawn as blocks. The ublk server is shown dashed
because it is planned, not built.

## Layers

```
 +----------------------------------------------------------------------+
 |  front ends                                                          |
 |                                                                      |
 |  +--------------+   +-----------------+   . . . . . . . . . . . .   |
 |  |  vbd CLI     |   |  tests          |   .  ublk server (P4)    .   |
 |  |  src/main.c  |   |  tests/*        |   .  -> /dev/ublkbN      .   |
 |  +------+-------+   +--------+--------+   . . . . . . .+. . . . .   |
 +---------|---------------------|-----------------------|-------------+
           |                     |                       |
           v                     v                       v
 +----------------------------------------------------------------------+
 |  libvbd                                               include/vbd.h  |
 |                                                                      |
 |  vbd_create  vbd_open  vbd_close  vbd_read  vbd_write  vbd_flush     |
 +----------------------------------------------------------------------+
 |  engine                                               src/volume.c   |
 |                                                                      |
 |  +-----------------------+        +-------------------------------+  |
 |  |  chunk map            |        |  append-only log              |  |
 |  |  XArray:              |------->|  replay, append, crc32c       |  |
 |  |  chunk -> record no.  |        |  src/crc32c.c                 |  |
 |  +-----------------------+        +-------------------------------+  |
 +----------------------------------------------------------------------+
 |  libxarray (deps/)        |  liburcu           |  POSIX file I/O      |
 +----------------------------------------------------------------------+
                                                   |              |
                                                   v              v
                                           +-------------+  +-------------+
                                           |  disk.log   |  |  base.img   |
                                           |  append     |  |  read-only  |
                                           +-------------+  +-------------+
```

Everything below `include/vbd.h` is the engine. Everything above it is a
consumer: the CLI and the tests today, the ublk server later. The engine has
no knowledge of what sits above it.

## A volume in memory and on disk

```
 struct vbd  (one per open volume)          disk.log
 +------------------------+                 +--------------------------+ 0
 |  log_fd      ----------+---------------->|  super block, 4 KiB      |
 |  base_fd     ----------+-----+           +--------------------------+ 4096
 |  size, chunk_size      |     |           |  rec 0 | hdr | chunk 5   |
 |  rec_size = 64 + chunk |     |           +--------------------------+ 4096 + R
 |  nrec = 3              |     |           |  rec 1 | hdr | chunk 0   |
 |  append_lock (mutex)   |     |           +--------------------------+ 4096 + 2R
 |                        |     |           |  rec 2 | hdr | chunk 5   |
 |  map (XArray)          |     |           +--------------------------+ 4096 + 3R
 |    chunk 0 -> rec 1    |     |              ^ next append            R = rec_size
 |    chunk 5 -> rec 2    |     |
 |    (chunk 1..4: empty) |     |           base.img  (immutable)
 +------------------------+     +---------->+-----+-----+-----+-----+-----+--
                                            | c0  | c1  | c2  | c3  | c4  | ...
                                            +-----+-----+-----+-----+-----+--
                                            0     C     2C    3C    4C         C = chunk_size
```

- The map holds the **newest** record for each written chunk. Chunk 5 was
  written twice; rec 0 is now unreachable through the map but still in the
  file (P3 time travel reads it back).
- Record number and file offset are interchangeable:
  `rec_off(n) = 4096 + n * rec_size`. The map stores only the small integer
  `n` as an XArray value entry; no per-chunk allocation.
- Chunks never written (1..4) have no entry; reads of them go to the base.

## On-disk format

```
 super block (4096 B, written once by create, never rewritten)
 +-------+-------+---------+------------+------+-----------+------------+----------------+
 | crc   | magic | version | chunk_size | size | base_size | base_mtime | base path ...  |
 | u32   | u32   | u32     | u32        | u64  | u64       | i64        | char[4056]     |
 +-------+-------+---------+------------+------+-----------+------------+----------------+
   ^       crc covers everything after itself

 record (64 B header + chunk_size B data)
 +-------+-------+-------+-------+----------+------------------------------------+
 | magic | crc   | chunk | epoch | pad      | data: exactly chunk_size bytes     |
 | u32   | u32   | u64   | u64   | 40 B     |                                    |
 +-------+-------+-------+-------+----------+------------------------------------+
           ^ crc covers the data               epoch: reserved for P3, 0 today
```

A record always carries a whole chunk, even for a one-byte write. That costs
write amplification and buys a format with no index on disk.
