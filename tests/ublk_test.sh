#!/bin/sh
# Serve a volume as a ublk block device and use it through the kernel: raw
# reads and writes with dd, a read-only snapshot device, then an ext4
# filesystem that survives a server restart. Needs sudo without a password
# and the ublk_drv module; skipped otherwise.
set -eu
VBD=$(realpath "$1")
if ! sudo -n true 2>/dev/null || [ ! -e /dev/ublk-control ]; then
	echo "ublk test: skipped (needs sudo -n and /dev/ublk-control)"
	exit 0
fi
T=$(mktemp -d); cd "$T"
pid=""; srv=""; mnt=""
cleanup() {
	[ -z "$mnt" ] || sudo -n umount "$mnt" 2>/dev/null || true
	[ -z "$srv" ] || { sudo -n kill -s TERM "$srv" 2>/dev/null; wait "$pid" 2>/dev/null; } || true
	rm -rf "$T"
}
trap cleanup EXIT
fail() { echo "ublk test: FAIL: $*" >&2; exit 1; }

# serve <vbd serve args>: start the server; sets $dev, $pid (sudo) and $srv (vbd).
# Signals go to $srv: sudo does not relay a signal sent from its own process group.
serve() {
	: > dev.txt
	sudo -n "$VBD" serve "$@" > dev.txt & pid=$!
	i=0
	while [ ! -s dev.txt ]; do
		i=$((i + 1)); [ $i -lt 100 ] || fail "server did not come up"
		sleep 0.1
	done
	dev=$(cat dev.txt)
	srv=$(pgrep -P "$pid")
}
stop() {	# SIGTERM, then the device must be gone when the server has exited
	sudo -n kill -s TERM "$srv"
	wait "$pid" || fail "server exit status $?"
	pid=""; srv=""
	[ ! -e "$dev" ] || fail "$dev still exists after stop"
}

head -c 4M /dev/urandom > base.img
head -c 4M /dev/zero > zero.img
"$VBD" create t.log 8M 64K base.img

# raw device: base in the first half, zeros in the second, a write that lands in the log
serve t.log
[ "$(sudo -n blockdev --getsize64 "$dev")" -eq $((8 << 20)) ] || fail "device size"
sudo -n dd if="$dev" bs=1M count=4 status=none | cmp -s - base.img || fail "base through the device"
sudo -n dd if="$dev" bs=1M skip=4 count=4 status=none | cmp -s - zero.img || fail "zeros past the base"
printf 'via ublk' | sudo -n dd of="$dev" bs=1 seek=4090 conv=notrunc,fsync status=none
stop
{ head -c 4090 base.img; printf 'via ublk'; tail -c +4099 base.img; } > expect.img
"$VBD" read t.log 0 4M | cmp -s - expect.img || fail "write reached the log"

# a snapshot served read-only shows the old content
"$VBD" snapshot t.log > /dev/null
printf 'later' | "$VBD" write t.log 4090
serve t.log 0
[ "$(sudo -n blockdev --getro "$dev")" -eq 1 ] || fail "snapshot device should be read-only"
sudo -n dd if="$dev" bs=1M count=4 status=none | cmp -s - expect.img || fail "snapshot content through the device"
stop

# a filesystem on top, written, then checked and read back after a restart
if command -v mkfs.ext4 > /dev/null; then
	mkdir mnt
	serve t.log
	sudo -n mkfs.ext4 -q "$dev"
	sudo -n mount "$dev" mnt; mnt="$T/mnt"
	echo hello | sudo -n tee mnt/hello > /dev/null
	sudo -n umount mnt; mnt=""
	stop
	serve t.log
	sudo -n fsck.ext4 -n "$dev" > /dev/null 2>&1 || fail "fsck after restart"
	sudo -n mount -o ro "$dev" mnt; mnt="$T/mnt"
	[ "$(cat mnt/hello)" = hello ] || fail "file did not survive the restart"
	sudo -n umount mnt; mnt=""
	stop
fi

echo "ublk test: ok"
