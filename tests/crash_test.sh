#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Kill a writer with SIGKILL at random moments and check that the volume
# reopens and every chunk is intact: either the base content or a value
# that was written whole. A torn write must never show through.
set -eu


VBD=$(realpath "${1:-build/vbd}")
ROUNDS=${2:-5}
CHUNK=65536
N=32			# chunks in the volume
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"

fail() { echo "crash test: FAIL: $*" >&2; exit 1; }

head -c $((CHUNK * N)) /dev/urandom > base.img
"$VBD" create t.log $((CHUNK * N)) $CHUNK base.img

# writer loop: fill chunk i with the byte (i + round) % 256, round robin
cat > writer.sh <<'EOS'
#!/bin/sh
VBD=$1; CHUNK=$2; N=$3; round=$4; i=0
while :; do
	b=$(( (i + round) % 256 ))
	head -c "$CHUNK" /dev/zero | tr '\0' "\\$(printf %03o $b)" |
		"$VBD" write t.log $((i * CHUNK))
	i=$(( (i + 1) % N ))
done
EOS
chmod +x writer.sh

round=0
while [ $round -lt "$ROUNDS" ]; do
	round=$((round + 1))
	setsid ./writer.sh "$VBD" $CHUNK $N $round >/dev/null 2>&1 &	# own process group
	pid=$!
	sleep 0.$(od -An -N1 -tu1 /dev/urandom | tr -d ' ')
	kill -s KILL -- -$pid	# the whole group: loop shell and the vbd child
	wait $pid 2>/dev/null || true

	before=$(stat -c %s t.log)
	"$VBD" info t.log >/dev/null || fail "round $round: reopen"	# replays, drops a torn tail
	size=$(stat -c %s t.log)
	[ $(( (size - 4096) % (64 + CHUNK) )) -eq 0 ] ||
		fail "round $round: log is not whole records after recovery"

	i=0
	while [ $i -lt $N ]; do
		"$VBD" read t.log $((i * CHUNK)) $CHUNK > got.bin
		dd if=base.img of=base.bin bs=$CHUNK skip=$i count=1 2>/dev/null
		if ! cmp -s got.bin base.bin; then
			b=$(head -c1 got.bin | od -An -tu1 | tr -d ' ')
			[ "$(tr -d "\\$(printf %03o "$b")" < got.bin | wc -c)" -eq 0 ] ||
				fail "round $round: chunk $i is torn"
		fi
		i=$((i + 1))
	done
	torn=""
	[ "$before" -eq "$size" ] || torn=", dropped $((before - size)) torn bytes"
	echo "  round $round: $(( (size - 4096) / (64 + CHUNK) )) records intact$torn"
done

echo "crash test: ok"
