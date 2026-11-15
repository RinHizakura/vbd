#!/bin/sh
# Serve a volume as a ublk block device and use it through the kernel: raw
# reads and writes with dd, a read-only snapshot device, writers on several
# CPUs at once, an ext4 filesystem
# that survives a server restart, and an ext4 image fetched over HTTP of
# which a check and a mount download only a small part. Needs sudo without
# a password and the ublk_drv module; skipped otherwise.
set -eu
NAME="ublk test"
. "$(dirname "$0")/lib.sh"
VBD=$(realpath "$1")
if ! sudo -n true 2>/dev/null || [ ! -e /dev/ublk-control ]; then
	echo "ublk test: skipped (needs sudo -n and /dev/ublk-control)"
	exit 0
fi
T=$(mktemp -d); cd "$T"
pid=""; srv=""; mnt=""; httpd=""
cleanup() {
	[ -z "$mnt" ] || sudo -n umount "$mnt" 2>/dev/null || true
	[ -z "$srv" ] || { sudo -n kill -s TERM "$srv" 2>/dev/null; wait "$pid" 2>/dev/null; } || true
	[ -z "$httpd" ] || kill "$httpd" 2>/dev/null || true
	rm -rf "$T"
}
trap cleanup EXIT

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

# one queue per CPU; four writers on different CPUs each fill every fourth
# 4 KiB block of the same 2 MiB, so every 64 KiB chunk is written piecemeal
# by all four at once, and each block must end up as its writer left it
serve t.log
[ "$(ls "/sys/block/${dev#/dev/}/mq" | wc -l)" -eq "$(nproc)" ] || fail "queues: want one per CPU"
: > mixed.img
for w in 0 1 2 3; do
	head -c 4096 /dev/zero | tr '\0' "$w" > "blk$w"
done
i=0; while [ $i -lt 512 ]; do cat "blk$((i % 4))" >> mixed.img; i=$((i + 1)); done
writers=""
for w in 0 1 2 3; do
	sudo -n taskset -c $((w % $(nproc))) sh -c "
		i=$w
		while [ \$i -lt 512 ]; do
			dd if=blk$w of=$dev bs=4k seek=\$i oflag=direct conv=notrunc status=none || exit 1
			i=\$((i + 4))
		done" &
	writers="$writers $!"
done
for w in $writers; do	# these four only: a bare wait would wait for the server too
	wait "$w" || fail "a writer failed"
done
sudo -n dd if="$dev" bs=4k count=512 iflag=direct status=none | cmp -s - mixed.img ||
	fail "writes from several CPUs into the same chunks"
stop
"$VBD" read t.log 0 2M | cmp -s - mixed.img || fail "the same after the server stopped"

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

# the point of fetching lazily: an ext4 image that lives on an HTTP server
# is checked and mounted, and a file read from it, while most of the image
# stays where it is. With a manifest, so every chunk that arrives is verified.
CHUNKS=4096		# 256 MiB in 64 KiB chunks
if command -v mkfs.ext4 > /dev/null && command -v busybox > /dev/null &&
   command -v curl > /dev/null; then
	mkdir -p mnt www files
	echo "over http" > files/hello
	head -c 20M /dev/urandom > files/blob	# something in the image that is never read
	truncate -s 256M www/fs.img
	mkfs.ext4 -q -d files www/fs.img
	port=$(free_port)
	start_httpd "$T/www" "$port"
	"$VBD" manifest www/fs.img 64K > fs.manifest
	"$VBD" create h.log 256M 64K "http://127.0.0.1:$port/fs.img" fs.manifest
	serve h.log
	sudo -n fsck.ext4 -fn "$dev" > /dev/null 2>&1 || fail "fsck of the image over http"
	sudo -n mount -o ro,noload "$dev" mnt; mnt="$T/mnt"
	[ "$(cat mnt/hello)" = "over http" ] || fail "file from the image over http"
	sudo -n umount mnt; mnt=""
	stop
	fetched=$("$VBD" info h.log | sed -n 's/^fetched //p')
	[ "$fetched" -gt 0 ] && [ "$fetched" -lt $((CHUNKS / 4)) ] ||
		fail "fsck and mount fetched $fetched of $CHUNKS chunks"
	echo "  image over http: fsck, mount and a file read fetched $fetched of $CHUNKS chunks"
fi

echo "ublk test: ok"
