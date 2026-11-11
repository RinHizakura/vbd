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
- crash test: `tests/crash_test.sh` SIGKILLs a writer loop at random
  moments; the volume must reopen with whole records only and every chunk
  either base content or a value written in full

Remaining:

- checkpoint: sorted `(chunk, record)` array written at `flush`, so reopen
  does not have to re-read the whole log; only needed once replay is slow

## P2: lazy fetch

Done:

- base cache: a second log file (`disk.log.cache`, same format) with its own
  XArray; a base chunk is fetched on first read, appended there and served
  from the cache afterwards; chunks past the end of the base are never
  fetched. `vbd info` reports `fetched`.
- `fetch_chunk` is the single place that talks to the base, through
  `struct base_ops` (`src/base.h`): `base_file.c` is `pread` of a local
  file, `base_http.c` an HTTP range request over libcurl. `probe` gives the
  stored name, size and a validator (mtime, or ETag / Last-Modified) that
  `create` records and `open` compares, refusing a base that changed. `tests/http_test.sh` serves an image with
  busybox httpd and checks fetch counts, caching and the stale check.
- Offline open: a base that cannot be probed leaves the volume open with
  `vbd_base_err` set; cached chunks read, misses return that error. Only a
  base that answers and has changed is refused.
- Concurrent misses on one chunk fetch it once: the first reader stores an
  INFLIGHT pointer entry in the cache map, the others wait on a condition
  variable until it becomes a record number.

Remaining:

- background prefetcher uses `xa_find` to fill holes near recent reads
- verify every fetched chunk against a manifest hash before caching
- test: mount a raw image over HTTP, `fsck` it, confirm only a small
  fraction of the image was downloaded

## P3: branches and time travel

- `fork(parent, name)`: new directory, empty XArray, parent pointer; parent
  is frozen at its current epoch
- lookup walks the branch chain before falling back to the base cache
- epochs advance on `snapshot` or a timer; manifest records epoch -> time
- `at(branch, time)`: load the newest checkpoint at or before the time,
  replay the log up to that epoch, expose the result as a read-only branch
- test: write, snapshot, write again, then check that `at` returns the
  earlier content byte-for-byte

## P4: ublk front end

- target built on `ublksrv` or `libublk-rs`; one engine queue per ublk queue
- every queue thread registers with RCU; reads take no lock
- `mkfs.ext4`, `fio`, boot a VM from the device
- compare throughput against a loop device over the same base file

## Open questions

- metadata durability ordering between log fsync, checkpoint rename and
  manifest update
- node memory: the XArray only grows; decide whether to rebuild from
  checkpoint periodically or add shrinking
- whether the mapping layer should move to a Rust XArray once one lands in
  the kernel
