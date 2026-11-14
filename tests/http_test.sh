#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Use an image served over HTTP as the base: chunks are fetched by range
# request on first read, cached, and never fetched twice. Needs busybox httpd.
set -eu

NAME="http test"
. "$(dirname "$0")/lib.sh"
VBD=$(realpath "${1:-build/vbd}")
command -v busybox >/dev/null || { echo "http test: skipped (no busybox)"; exit 0; }
T=$(mktemp -d)
httpd=""
trap 'test -n "$httpd" && kill "$httpd" 2>/dev/null; rm -rf "$T"' EXIT
cd "$T"

fetched() { "$VBD" info t.log | sed -n 's/^fetched //p'; }

mkdir www
head -c 1M /dev/urandom > www/base.img
port=$(free_port)
url=http://127.0.0.1:$port/base.img
start_httpd "$T/www" "$port"

expect 1 "$VBD" create u.log 2M 64K "http://127.0.0.1:$port/missing.img"	# 404

"$VBD" create t.log 2M 64K "$url"
[ "$(fetched)" = 0 ] || fail "fresh volume fetched $(fetched)"

"$VBD" read t.log 0 1M | cmp -s - www/base.img || fail "base over http"
[ "$(fetched)" = 16 ] || fail "1 MiB / 64 KiB should fetch 16 chunks, got $(fetched)"

head -c 1M /dev/zero > zero.img
"$VBD" read t.log 1M 1M | cmp -s - zero.img || fail "past the end of the base"
"$VBD" read t.log 0 64K >/dev/null
[ "$(fetched)" = 16 ] || fail "cached chunks were fetched again"

# partial write: read-modify-write against the cached chunk
printf 'over http' | "$VBD" write t.log 70000
{ head -c 70000 www/base.img; printf 'over http'; tail -c +70010 www/base.img; } > expect.img
"$VBD" read t.log 0 1M | cmp -s - expect.img || fail "read-modify-write"

# server down: the volume opens offline; cached chunks read, misses fail
"$VBD" create v.log 2M 64K "http://127.0.0.1:$port/base.img"	# nothing cached
kill "$httpd"; wait "$httpd" 2>/dev/null || true; httpd=""
"$VBD" info t.log | grep -q '^base offline' || fail "info should report offline"
"$VBD" read t.log 0 1M | cmp -s - expect.img || fail "offline read from cache"
expect 1 "$VBD" read v.log 0 64K

# a base that changed on the server is refused
touch -d 2000-01-01 www/base.img
start_httpd "$T/www" "$port"
expect 1 "$VBD" read t.log 0 64K

echo "http test: ok"
