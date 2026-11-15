# vbd

**vbd is a block-device server for development and test machines.** It
serves a disk that is forked from a shared image in O(1), fetches the image
lazily from wherever it lives, and keeps every write so the disk can be
reopened at any earlier moment.

It ships in three parts:

| Part | What it is | Status |
|---|---|---|
| `vbd serve` | exposes a volume as `/dev/ublkbN` through ublk | built |
| `vbd` CLI | creates, inspects, reads and writes volumes from the shell; the scripting tool | built |
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

Requires `liburcu-dev` and `libcurl4-openssl-dev`; liburing and the XArray
come in as submodules. Serving a block device needs the kernel's ublk
driver, `CONFIG_BLK_DEV_UBLK` (`modprobe ublk_drv`), which mainstream
distributions have shipped as a module since Linux 6.0.

The tests also use `busybox` for a local HTTP server, and `sudo -n` plus
`mkfs.ext4` for the block device test; both are skipped when unavailable.

```sh
git submodule update --init
make        # build/vbd, build/libvbd.a (and deps/libxarray)
make test
make clean
```

## Usage

A volume is a log file plus an optional read-only base image, named when the
volume is created. The base can be a local file or an `http://` / `https://`
URL whose server supports range requests; only the parts of it that are
actually read get downloaded, and once read they stay available even when
the server is not. The base must not change while volumes depend on it: a
volume whose base was modified refuses to open.

```
usage: vbd <command> ...
  create   <log> <size> <chunk> [base [manifest]]   new volume; sizes take K, M, G suffixes
  info     <log> [snapshot]                         print size, chunk size, epoch, chunks fetched, base state
  read     <log> <off> <len> [snapshot]             copy a range to stdout, as of a snapshot if given
  write    <log> <off>                              copy stdin to the volume and flush
  snapshot <log>                                    freeze the current content; prints the snapshot number
  serve    <log> [snapshot]                         expose the volume as /dev/ublkbN until SIGINT or SIGTERM; needs root
  manifest <image> <chunk>                          write the manifest of a base image to stdout, for create's [manifest]
```

Sizes and offsets accept `K`, `M` and `G` suffixes. For example:

```sh
vbd create disk.log 1G 64K base.img   # new volume, 1 GiB, 64 KiB chunks
vbd create disk.log 1G 64K https://images.example/ubuntu.img   # or a remote base
vbd info disk.log                     # size, chunk size, base chunks fetched so far
vbd read disk.log 0 4K                # range to stdout
printf 'hi' | vbd write disk.log 4090 # stdin to the volume
vbd snapshot disk.log                 # freeze the current content; prints the snapshot number
vbd read disk.log 0 4K 0              # the same range as it was at snapshot 0
sudo vbd serve disk.log               # prints /dev/ublkbN; Ctrl-C removes it again
sudo vbd serve disk.log 0             # a read-only device showing snapshot 0
```

The served device is an ordinary block device: `mkfs`, `mount`, `dd`, a VM
all work on it. Flushes go to disk before they are acknowledged.

Snapshots are numbered from 0. Each one stores the volume's index as it is
at that moment, 16 bytes per chunk written so far, rounded up to whole
chunks: one chunk of log space for a small volume. Reading as of a
snapshot never changes the volume, and does not get slower with how much
was written before it.

To be sure a base is what you expect, and stays that, give `create` a
manifest of it: the sha256 of every chunk, made from a copy you trust. A
chunk that arrives from the base and does not match is refused, not cached:

```sh
vbd manifest ubuntu.img 64K > ubuntu.manifest
vbd create disk.log 1G 64K https://images.example/ubuntu.img ubuntu.manifest
```

One process at a time can have a volume open for writing. While it is
being served, `vbd info`, `vbd write`, `vbd snapshot` and a `vbd read`
without a snapshot number report that it is busy; reading as of a snapshot
still works.

A volume survives a crash of the process writing it: a write that was cut
short is dropped on the next open, everything before it is kept.

How the engine works is in `docs/ARCHITECTURE.md`; what comes next is in
`docs/ROADMAP.md`.

## License

GPL-2.0-or-later (see `LICENSE`). `deps/libxarray` contains unmodified Linux
kernel code under the same terms.
