#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# fio against a served volume, next to a loop device over a copy of the same
# image, for a sense of what the engine and the trip through user space
# cost. Not part of `make test`: run it with `make bench`. Needs fio,
# sudo -n and /dev/ublk-control.
#
#   SECS   how long each of the eight runs lasts (default 5)
#   JOBS   fio processes per run, each at queue depth 32 (default 4)
#   CHUNK  the volume's chunk size (default 64K)
#
# The volume keeps every write, so the write runs grow its log by what
# they wrote, several GiB at the defaults; it is all removed afterwards.
set -eu
NAME="bench"
. "$(dirname "$0")/lib.sh"
VBD=$(realpath "${1:-build/vbd}")
SECS=${SECS:-5}
JOBS=${JOBS:-4}
CHUNK=${CHUNK:-64K}
if ! command -v fio > /dev/null || ! sudo -n true 2>/dev/null || [ ! -e /dev/ublk-control ]; then
	echo "bench: skipped (needs fio, sudo -n and /dev/ublk-control)"
	exit 0
fi
T=$(mktemp -d); cd "$T"
pid=""; srv=""; loop=""
cleanup() {
	[ -z "$srv" ] || { sudo -n kill -s TERM "$srv" 2>/dev/null; wait "$pid" 2>/dev/null; } || true
	[ -z "$loop" ] || sudo -n losetup -d "$loop" || true
	rm -rf "$T"
}
trap cleanup EXIT

head -c 256M /dev/urandom > base.img
cp base.img loop.img
"$VBD" create v.log 256M "$CHUNK" base.img
sudo -n "$VBD" serve v.log > dev.txt & pid=$!
i=0
while [ ! -s dev.txt ]; do
	i=$((i + 1)); [ $i -lt 100 ] || fail "server did not come up"
	sleep 0.1
done
ublk=$(cat dev.txt); srv=$(pgrep -P "$pid")
loop=$(sudo -n losetup --find --show loop.img)

# run <dev> <rw> <bs>: prints IOPS (terse fields 8 and 49 are read and write IOPS)
run() {
	sudo -n fio --name=b --filename="$1" --rw="$2" --bs="$3" --direct=1 \
		--ioengine=libaio --iodepth=32 --numjobs="$JOBS" --group_reporting \
		--time_based --runtime="$SECS" --minimal |
		awk -F';' '{ printf "%d\n", $8 + $49 }'
}

echo "$JOBS jobs at depth 32, $SECS s each, $(nproc) CPUs, $CHUNK chunks"
printf '%-10s %-4s %12s %12s\n' job bs "ublk IOPS" "loop IOPS"
for job in "randread 4k" "randwrite 4k" "read 1M" "write 1M"; do
	set -- $job
	printf '%-10s %-4s %12s %12s\n' "$1" "$2" "$(run "$ublk" "$1" "$2")" "$(run "$loop" "$1" "$2")"
done
echo "the volume's log grew to $(( $(stat -c %s v.log) >> 20 )) MiB"
