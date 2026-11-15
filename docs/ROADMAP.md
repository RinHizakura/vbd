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
- Manifest: `vbd sums <image> <chunk>` writes the sha256 of every chunk of
  an image, and `vbd create ... base sums` takes it. It is kept as
  `disk.log.sums` and its own sha256 goes in the super block, so the copy
  cannot be swapped unnoticed. Every chunk fetched from the base is hashed
  and compared before it enters the cache; one that differs is not stored
  and the read fails with EIO. `tests/http_test.sh` changes a byte of the
  image behind an unchanged size and date and expects that chunk, and only
  that chunk, to be refused.
- The cache file is shared between processes, a volume's writer and views
  of it: an append takes a `flock`, reads in what the others appended and
  drops its own record if the chunk is there by now, and a miss looks at
  the file once more before going to the base. The file holds each chunk
  once; two processes that miss at the same moment both download.
- Concurrent misses on one chunk fetch it once: the first reader enters
  the chunk in a table of fetches in progress, the others wait on a
  condition variable until it has left and then find it in the cache.

Remaining:

- background prefetcher uses `xa_find` to fill holes near recent reads
- test: mount a raw image over HTTP, `fsck` it, confirm only a small
  fraction of the image was downloaded

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

Remaining:

- `fork(parent, name)`: new directory, empty XArray, parent pointer; parent
  is frozen at its current epoch
- lookup walks the branch chain before falling back to the base cache
- a timer that snapshots periodically; manifest records epoch -> time
- `at(branch, time)`: load the newest checkpoint at or before the time,
  replay the log up to that epoch, expose the result as a read-only branch
- test: write, snapshot, write again, then check that `at` returns the
  earlier content byte-for-byte

## P4: ublk front end

Done:

- `vbd serve` (`src/ublk/server.c`, ~300 lines on liburing, no ublksrv):
  one queue, one thread, 64 tags, 256 KiB bounce buffer per tag. READ and
  WRITE map to `vbd_read` / `vbd_write`, FLUSH to `vbd_flush`; the device
  advertises a volatile cache so flushes arrive. A volume opened at a
  snapshot is served read-only. SIGINT or SIGTERM stops and deletes the
  device.
- `tests/ublk_test.sh`: dd through the device, read-only snapshot device,
  mkfs.ext4 + mount + fsck across a server restart. Skipped without
  `sudo -n` or `/dev/ublk-control`.

Remaining:

- one queue per CPU, each with its own thread and ring
- zero copy or `UBLK_F_USER_COPY` instead of bounce buffers
- DISCARD and WRITE_ZEROES (a zero record, or a hole in the map)
- `UBLK_F_USER_RECOVERY`: survive a server restart without unmounting
- `fio` against a loop device over the same image; boot a VM from the device

## Open questions

- metadata durability ordering between log fsync and manifest update. The
  checkpoint is settled: it is appended to the log after an `fdatasync`,
  so it only describes records that are already durable, and one that is
  torn is skipped for the one before it.
- node memory: the XArray only grows; decide whether to rebuild from
  checkpoint periodically or add shrinking
- whether the mapping layer should move to a Rust XArray once one lands in
  the kernel
