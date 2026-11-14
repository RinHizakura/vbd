# SPDX-License-Identifier: GPL-2.0-or-later
# Shared by the test scripts. Before changing directory, set NAME to what
# the test calls itself in its messages and then:  . "$(dirname "$0")/lib.sh"

fail() { echo "$NAME: FAIL: $*" >&2; exit 1; }

# expect <status> <cmd...>: run cmd quietly, require the given exit status
expect() {
	want=$1; shift
	set +e; "$@" >/dev/null 2>&1; got=$?; set -e
	[ "$got" -eq "$want" ] || fail "$* exited $got, want $want"
}

# A local port that is probably free.
free_port() { echo $(( 20000 + $(od -An -N2 -tu2 /dev/urandom) % 20000 )); }

# start_httpd <dir> <port>: serve <dir> on 127.0.0.1:<port> with busybox
# httpd and wait until it answers. Sets $httpd to its pid.
start_httpd() {
	busybox httpd -f -p "127.0.0.1:$2" -h "$1" &
	httpd=$!
	i=0
	until curl -sI -o /dev/null "http://127.0.0.1:$2/"; do
		i=$((i + 1)); [ $i -lt 50 ] || fail "httpd did not come up"
		sleep 0.1
	done
}
