# vbd

**vbd is a block-device server for development and test machines.** It
serves a disk that is forked from a shared image in O(1), fetches the image
lazily from wherever it lives, and keeps every write so the disk can be
reopened at any earlier moment.

It ships in three parts:

| Part | What it is | Status |
|---|---|---|
| `vbd` server | exposes a volume as `/dev/ublkbN` through ublk | planned (P4) |
| `vbd` CLI | creates, inspects, reads and writes volumes from the shell; the front end until the server exists, and the scripting tool afterwards | built |
| `libvbd` | the engine both of them call; usable on its own, but the API is shaped by the server's needs, not for general embedding | built |

The engine is built on the Linux kernel's **XArray**, compiled unmodified for
userspace: one `chunk -> location` lookup serves branches, lazy fetch and
time travel alike.

## Who it is for

Teams that start many short-lived machines from the same large disk image
and want each one to have its own disk without copying the image:

- a CI runner that forks the golden image per job and discards the branch
- a developer who mounts an image stored on S3 or an HTTP server and only
  pays for the chunks that are actually read
- a tester who reopens a disk at the moment before a test corrupted its
  database, instead of re-running the test and hoping it reproduces

It is not a general-purpose block device: the base image must never change,
the write path favours simplicity over throughput, and old data is never
reclaimed.

## Build

Requires `liburcu-dev` and `libcurl4-openssl-dev`. The tests also use
`busybox` for a local HTTP server.

```sh
git submodule update --init
make        # build/vbd, build/libvbd.a (and deps/libxarray)
make test
make clean
```

## Usage

A volume is a log file plus an optional read-only base image, named when the
volume is created. The base is a local file or an `http://` / `https://` URL
whose server supports range requests. Reads of a chunk that was never
written fall through to the base image; a base chunk is fetched on first
read into `disk.log.cache` and served from there afterwards, so only the
parts of a remote image that are actually read get downloaded. Every write
appends a whole chunk to the log. The base must not change afterwards:
opening a volume whose base has a different size, mtime or ETag than at
create time fails.

Sizes and offsets accept `K`, `M` and `G` suffixes.

```sh
vbd create disk.log 1G 64K base.img   # new volume, 1 GiB, 64 KiB chunks
vbd create disk.log 1G 64K https://images.example/ubuntu.img   # or a remote base
vbd info disk.log                     # size, chunk size, base chunks fetched so far
vbd read disk.log 0 4K                # range to stdout
printf 'hi' | vbd write disk.log 4090 # stdin to the volume
```

Reopening a log replays it to rebuild the chunk map. A record that was only
partly written when the process died is detected by its checksum and dropped.

How the engine works is in `docs/ARCHITECTURE.md`; what comes next is in
`docs/ROADMAP.md`.

## License

GPL-2.0-or-later (see `LICENSE`). `deps/libxarray` contains unmodified Linux
kernel code under the same terms.
