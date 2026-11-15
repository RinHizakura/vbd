# Roadmap

Each phase ends with something that runs. Phases P1 to P3 need no kernel
support beyond what any Linux box has; P4 needs `CONFIG_BLK_DEV_UBLK`.

## P0: XArray in userspace (done)

Lives in its own repository, [libxarray](../deps/libxarray), pulled in as a
submodule: the kernel's `lib/xarray.c` compiled unmodified against
hand-written shims.

## P1: engine library and CLI

Scope: a volume backed by a local base file, no branches, no history yet.

Done:

- chunk size configurable at `create` (power of two)
- base image bound at `create`: absolute path, size and mtime in the super
  block; open refuses a base that changed
- super block carries a version and a crc32c; open re-checks everything
  create enforced, so a damaged super is refused instead of truncating the
  log
- one XArray per volume: chunk index -> value entry holding the log record
  number
- append-only log of fixed-size records: `[magic, crc32c, chunk, epoch][data]`
- read path: XArray hit -> log record; miss -> base file (or zeros)
- write path: read-modify-write to a full chunk, append, `xa_store`
- `flush` = `fdatasync`; reopen replays the log and truncates a torn tail
- test: random I/O compared byte-for-byte against an in-memory reference,
  across reopen and a torn record
- CLI: `vbd create | info | read | write`; `write` flushes before exit;
  `tests/cli_test.sh` drives it end to end
- one writer per volume: a writable open takes an exclusive `flock` on the
  log and a second one gets EBUSY. A snapshot view takes no lock and
  changes nothing in the file, so it can be open beside the writer.
- crash test: `tests/crash_test.sh` SIGKILLs a writer loop at random
  moments; the volume must reopen with whole records only and every chunk
  either base content or a value written in full

- checkpoint: the map as a sorted `(chunk, record)` array, appended to the
  log itself as a run of records with a header of their own (magic
  `vbck`), so the volume stays one file. Open walks the record headers
  back from the end to the last complete checkpoint, loads it and replays
  only what follows. Written
  at `flush` and at close, once 64 MiB of records have piled up and at least
  four times what the previous checkpoint took, because a checkpoint stays
  in the log for good. Covered records are not checksummed again on open:
  that is the time it saves. The base cache checkpoints the same way, at
  close only.

## P2: lazy fetch

Done:

- base cache: a second log file (`disk.log.cache`, same format) with its own
  XArray; a base chunk is fetched on first read, appended there and served
  from the cache afterwards; chunks past the end of the base are never
  fetched. `vbd info` reports `fetched`.
- `fetch_chunk` is the single place that talks to the base, through
  `struct base_ops` (`src/base/`): `file.c` is `pread` of a local
  file, `http.c` an HTTP range request over libcurl. `probe` gives the
  stored name, size and a validator (mtime, or ETag / Last-Modified) that
  `create` records and `open` compares, refusing a base that changed. `tests/http_test.sh` serves an image with
  busybox httpd and checks fetch counts, caching and the stale check.
- Offline open: a base that cannot be probed leaves the volume open with
  `vbd_base_err` set; cached chunks read, misses return that error. Only a
  base that answers and has changed is refused.
- Manifest: `vbd manifest <image> <chunk>` writes the sha256 of every
  chunk of an image, and `vbd create ... base manifest` takes it. It is
  kept as `disk.log.manifest` and its own sha256 goes in the super block,
  so the copy cannot be swapped unnoticed. Every chunk fetched from the
  base is hashed and compared before it enters the cache; one that differs
  is not stored and the read fails with EIO. `tests/http_test.sh` changes
  a byte of the image behind an unchanged size and date and expects that
  chunk, and only that chunk, to be refused.
- The cache file is shared between processes, a volume's writer and views
  of it: an append takes a `flock`, reads in what the others appended and
  drops its own record if the chunk is there by now, and a miss looks at
  the file once more before going to the base. The file holds each chunk
  once; two processes that miss at the same moment both download.
- Concurrent misses on one chunk fetch it once: the first reader enters
  the chunk in a table of fetches in progress, the others wait on a
  condition variable until it has left and then find it in the cache.

- Prefetch (`vbd_prefetch`, `vbd -p <chunks>`): a thread in the base cache
  that, after a read had to fetch chunk n, fetches the hole that starts at
  n + 1: up to the given number of chunks, ending early at the first one
  `xa_find` reports as already in the cache, or at the end of the base. A
  newer miss elsewhere takes over. Off by default, since it downloads what
  nobody asked for.
- `tests/ublk_test.sh` puts a 256 MiB ext4 image behind busybox httpd,
  creates a volume over it with a manifest, serves it, and runs
  `fsck.ext4 -f`, mounts it and reads a file: 24 of its 4096 chunks are
  fetched, 0.6% of the image. The test fails above a quarter.

## P3: branches and time travel

Done:

- Epochs: every record is stamped with the epoch it was written in;
  `vbd_snapshot` steps it. `vbd snapshot` prints the frozen number,
  `vbd info` the current epoch.
- A snapshot is a checkpoint: `vbd_snapshot` appends the map as it is, in
  a checkpoint whose header names the next epoch, so the step survives a
  reopen with no further writes. It costs the size of the map rather than
  one record of zeros; for a small volume that is still one record. A
  chunk must be 16 bytes at least, so that a record can hold a map entry.
- `vbd_open_at(log, N)`: a read-only view (writes and snapshots return
  EROFS), loaded from the checkpoint snapshot N wrote, without reading any
  record before or after it. `vbd read ... N` and `vbd info ... N` use it.

- Time: a snapshot's checkpoint records when it was taken, and
  `vbd_open_before(log, time)` opens the newest snapshot taken by then, by
  walking the checkpoint headers back from the end of the log. Wherever
  the CLI takes a snapshot number it takes `@<seconds since 1970>` too.
  `test_open_before` writes, snapshots, writes everything again, and gets
  the earlier content back byte for byte.

- Fork: `vbd_fork(parent, snapshot, log)`, `vbd fork <log> <new log>
  [snapshot]`. A fork is a log of its own whose super block names the
  parent's log and the snapshot it continues from; it starts as that super
  block alone, with an empty map and no base or cache. With no snapshot
  given, the parent is snapshotted first. Only that snapshot is frozen:
  the parent stays writable and the fork never sees what it writes later.
- Lookup walks the chain: the fork's data log, then each ancestor as a
  view at its fork point, then the base cache of the volume at the bottom,
  which every volume of the chain fetches into, in whatever process.
  `test_fork` covers the two going separate ways, a fork of a fork, and
  the shared cache.

## P4: ublk front end

Done:

- `vbd serve` (`src/ublk/server.c`, ~300 lines on liburing, no ublksrv):
  64 tags a queue. READ and
  WRITE map to `vbd_read` / `vbd_write`, FLUSH to `vbd_flush`; the device
  advertises a volatile cache so flushes arrive. A volume opened at a
  snapshot is served read-only. SIGINT or SIGTERM stops and deletes the
  device.
- One queue per CPU, each a thread with its own ring, descriptor array and
  buffer; the engine underneath takes calls from all of them at once.
- `UBLK_F_USER_COPY`: the queue thread moves a request's data itself, with
  `pread` / `pwrite` on `/dev/ublkcN`, into one 256 KiB buffer per queue
  where there was a bounce buffer per tag, 64 to a queue. With 20 queues
  the server takes 8 MiB idle and 16 MiB after reads and writes on every
  CPU, where the bounce buffers alone could reach 320 MiB. Not zero copy:
  the engine checksums what it stores, so the data has to pass through
  user memory once anyway. Needs Linux 6.5.
- DISCARD and WRITE_ZEROES both map to `vbd_zero`. One record says that a
  run of whole chunks is zero, however long the run, and the map marks
  those chunks with a reserved value in place of a record number; part of
  a chunk at either end is rewritten. A hole in the map would not do: it
  would show the base or the parent again. The device advertises a
  discard granularity of one chunk.
- `tests/ublk_test.sh`: dd through the device, read-only snapshot device,
  four writers on different CPUs filling the same chunks piecemeal,
  mkfs.ext4 + mount + fsck across a server restart. Skipped without
  `sudo -n` or `/dev/ublk-control`.

- `tests/bench.sh` (`make bench`): fio on the served device and on a loop
  device over a copy of the same image. On a 20-CPU WSL2 guest, 64 KiB
  chunks, 3 s runs, IOPS at 1 / 4 / 16 fio jobs of depth 32:

  ```
               ublk, jobs:    1       4      16     loop, 4 jobs
  randread  4k            139k    775k    906k            276k
  randwrite 4k            5.4k   16.4k   14.3k            234k
  read      1M            3.7k   13.4k   17.5k            7.0k
  write     1M             380     779     878            7.3k
  ```

  Reads scale with the queues. Writes stop at about 14 thousand records a
  second, some 900 MiB/s of log, whether as 4 KiB writes that each rewrite
  a 64 KiB chunk or as 1 MiB writes of 16 chunks each. The server uses
  three CPUs at that point (two in user mode, mostly the checksum; one in
  the kernel), and its other threads wait: every append goes through the
  one end of the log, one at a time. With 4 KiB chunks a 4 KiB write is a
  4 KiB record and random writes reach 98k IOPS at 4 jobs, while 1 MiB
  writes, now 256 appends each, fall to 375.

Remaining:

- boot a VM from the device

## Open questions

- metadata durability ordering between log fsync and manifest update. The
  checkpoint is settled: it is appended to the log after an `fdatasync`,
  so it only describes records that are already durable, and one that is
  torn is skipped for the one before it.
- node memory: the XArray only grows; decide whether to rebuild from
  checkpoint periodically or add shrinking
- whether the mapping layer should move to a Rust XArray once one lands in
  the kernel
- a fork finds its parent by the absolute path of the parent's log, and by
  nothing else. Move or rename the parent and its forks no longer open;
  put another volume of the same size and chunk size at that path and a
  fork takes it for its parent
