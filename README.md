# vbd

A userspace block-device engine built on the Linux kernel's **XArray**. One
`chunk -> location` lookup path serves three features:

1. **Branches**: fork a whole disk in O(1), no data copied
2. **Lazy fetch**: the base image lives remotely; chunks are fetched on first read
3. **Time travel**: every write keeps the old version; mount a read-only view
   of any instant

## Build

Requires `liburcu-dev`.

```sh
git submodule update --init
make        # build/libvbd.a (and deps/libxarray)
make test
make clean
```

## Usage

A volume is a log file plus an optional read-only base image, named when the
volume is created. Reads of a chunk that was never written fall through to
the base image; every write appends a whole chunk to the log. The base must
not change afterwards: opening a volume whose base has a different size or
mtime than at create time fails.

```c
#include <vbd.h>

rcu_register_thread();                       /* once per thread */

vbd_create("disk.log", 1 << 30, 64 << 10, "base.img");   /* 1 GiB, 64 KiB chunks */
struct vbd *v = vbd_open("disk.log");

vbd_write(v, data, len, offset);             /* pwrite semantics: bytes written or -errno */
vbd_read(v, data, len, offset);              /* pread semantics */
vbd_flush(v);                                /* fdatasync */

vbd_close(v);
```

Reopening a log replays it to rebuild the chunk map. A record that was only
partly written when the process died is detected by its checksum and dropped.

### Command line

`build/vbd` drives a volume from the shell. Sizes and offsets accept `K`,
`M` and `G` suffixes.

```sh
vbd create disk.log 1G 64K base.img   # new volume, 1 GiB, 64 KiB chunks
vbd info disk.log
vbd read disk.log 0 4K                # range to stdout
printf 'hi' | vbd write disk.log 4090 # stdin to the volume
```

## Layout

```
include/vbd.h        public API
include/common.h     arithmetic helpers (IS_POWER_OF_2, IS_ALIGNED)
src/main.c           the vbd command
src/volume.c         chunk map (XArray) over an append-only log
src/crc32c.c         record checksums
tests/cli_test.sh    the vbd command end to end
tests/crash_test.sh  SIGKILL a writer at random, check every chunk is intact
tests/volume_test.c  random I/O against an in-memory reference, with reopen
                     and torn-tail recovery
deps/libxarray/      the kernel XArray as a userspace library (submodule)
docs/ROADMAP.md      what is built, in what order, and how each step is checked
```

## License

GPL-2.0-or-later (see `LICENSE`). `deps/libxarray` contains unmodified Linux
kernel code under the same terms.
