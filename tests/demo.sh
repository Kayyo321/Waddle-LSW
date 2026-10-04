#!/bin/sh
set -eu
demo_dir=$(mktemp -d)
mock_pid=
cleanup() {
    if [ -n "$mock_pid" ]; then
        kill "$mock_pid" 2>/dev/null || true
        wait "$mock_pid" 2>/dev/null || true
    fi
    rm -f "$demo_dir/guest.sock"
    rmdir "$demo_dir"
}
trap cleanup EXIT HUP INT TERM
./build/waddle-mock-guest --socket-path "$demo_dir/guest.sock" &
mock_pid=$!
attempt=0
while [ ! -S "$demo_dir/guest.sock" ]; do
    kill -0 "$mock_pid"
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo 'mock startup timed out' >&2
        exit 1
    fi
    sleep 0.02
done
status=0
printf 'stdin crossed the wire\n' |
    ./build/waddle exec --pipe --socket-path "$demo_dir/guest.sock" -- \
        /bin/sh -c 'printf "guest stdout\n"; cat; printf "guest stderr\n" >&2; exit 42' || status=$?
wait "$mock_pid"
mock_pid=
printf 'host received guest exit status: %s\n' "$status"
test "$status" -eq 42
