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
make        # builds deps/libxarray and the engine
make test
make clean
```

## Layout

```
deps/libxarray/    the kernel XArray as a userspace library (submodule)
docs/ROADMAP.md    what is built, in what order, and how each step is checked
```

## License

GPL-2.0-or-later (see `LICENSE`). `deps/libxarray` contains unmodified Linux
kernel code under the same terms.
