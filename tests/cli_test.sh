#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Exercise the vbd command against a small volume with a base image.
set -eu

VBD=${1:-build/vbd}
VBD=$(realpath "$VBD")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"

fail() { echo "cli test: FAIL: $*" >&2; exit 1; }
# expect <status> <cmd...>: run cmd, require the given exit status
expect() {
	want=$1; shift
	set +e; "$@" >/dev/null 2>&1; got=$?; set -e
	[ "$got" -eq "$want" ] || fail "$* exited $got, want $want"
}

head -c 1M /dev/urandom > base.img
head -c 1M /dev/zero > zero.img

"$VBD" create t.log 2M 4K base.img
[ "$("$VBD" info t.log)" = "size 2097152
chunk 4096
fetched 0" ] || fail info

# first MiB falls through to the base, second MiB was never written
"$VBD" read t.log 0 1M | cmp -s - base.img || fail base passthrough
"$VBD" read t.log 1M 1M | cmp -s - zero.img || fail zero fill
[ "$("$VBD" info t.log | sed -n 3p)" = "fetched 256" ] || fail "cache holds the base half only"
[ -f t.log.cache ] || fail "cache file"

# unaligned write across two chunks
printf 'hello vbd' | "$VBD" write t.log 4090
{ head -c 4090 base.img; printf 'hello vbd'; tail -c +4100 base.img; } > expect.img
"$VBD" read t.log 0 1M | cmp -s - expect.img || fail read-modify-write
[ "$(stat -c %s t.log)" -eq $((4096 + 2 * (64 + 4096))) ] || fail "log holds two records"

# the write survives a reopen
"$VBD" read t.log 4090 9 | cmp -s - <<'EOT' || fail reopen
hello vbd
EOT

# errors
expect 1 "$VBD" create t.log 2M 4K                   # exists
expect 1 "$VBD" create u.log 2M 4K missing.img       # no such base
expect 1 "$VBD" create u.log 2M 3K                   # chunk not a power of two
expect 2 "$VBD" create u.log 2M+1 4K                 # bad size
expect 1 sh -c "head -c 10 /dev/zero | '$VBD' write t.log 2M"    # at end
expect 1 sh -c "head -c 10 /dev/zero | '$VBD' write t.log 2093K" # runs past end
[ "$("$VBD" read t.log 2M 10 | wc -c)" -eq 0 ] || fail "read at end"
expect 2 "$VBD" bogus t.log
expect 2 "$VBD" read t.log 0

# a base that changed since create is refused
touch base.img
expect 1 "$VBD" read t.log 0 4K

echo "cli test: ok"
