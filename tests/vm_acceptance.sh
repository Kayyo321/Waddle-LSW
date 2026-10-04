#!/bin/bash
# Real VM acceptance; requires the listener and a dedicated mounted export.
set -euo pipefail
: "${WADDLE_VSOCK_CID:?Set the real Windows VM CID}"
: "${WADDLE_EXPORT_SOURCE:?Set the dedicated POSIX export mounted as X:}"
source_path=$(realpath "$WADDLE_EXPORT_SOURCE")
fixture="$source_path/windows_guest_test.exe"
test -f "$fixture"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"; rm -f "$source_path/marker.txt" "$source_path/result.txt"; rmdir "$source_path/waddle_日本語" 2>/dev/null || true' EXIT
base=(./build/waddle exec --pipe --vsock-cid "$WADDLE_VSOCK_CID" --timeout 60 --path-map "$source_path=X:\\" --cwd "$source_path")
run_status() {
    local expected=$1; shift
    local status=0
    "${base[@]}" -- "$@" > "$work_dir/out" 2> "$work_dir/err" || status=$?
    test "$status" -eq "$expected"
}
run_status 42 "$fixture" --child streams
printf 'stdout\n' > "$work_dir/expected"; cmp "$work_dir/expected" "$work_dir/out"
printf 'stderr\n' > "$work_dir/expected"; cmp "$work_dir/expected" "$work_dir/err"
run_status 3 "$fixture" --child exit259
run_status 0 "$fixture" --child args '' 'a b' 'foo"bar' 'C:\Program Files\' '日本語'
printf 'args ok\n' > "$work_dir/expected"; cmp "$work_dir/expected" "$work_dir/out"
# cmd /c is a Windows switch, so this case uses literal Windows paths.
status=0
./build/waddle exec --pipe --vsock-cid "$WADDLE_VSOCK_CID" --timeout 60 --cwd 'C:\' -- cmd.exe /c 'exit 37' > "$work_dir/out" 2> "$work_dir/err" || status=$?
test "$status" -eq 37
run_status 127 no-such-waddle-acceptance-program.exe
mkdir "$source_path/waddle_日本語"
"${base[@]}" --cwd "$source_path/waddle_日本語" --env WADDLE_TEST=first --env waddle_test=日本語 -- "$fixture" --child context > "$work_dir/out"
printf 'context ok\n' > "$work_dir/expected"; cmp "$work_dir/expected" "$work_dir/out"
printf 'export read marker\n' > "$source_path/marker.txt"
run_status 0 "$fixture" --child files "$source_path/marker.txt" "$source_path/result.txt"
printf 'files ok\n' > "$work_dir/expected"; cmp "$work_dir/expected" "$work_dir/out"
printf 'guest export write\n' > "$work_dir/expected"; cmp "$work_dir/expected" "$source_path/result.txt"
# The guest emits both initial outputs before reading this 5 MiB stdin stream.
head -c 5242880 /dev/zero > "$work_dir/input"
"${base[@]}" -- "$fixture" --child large < "$work_dir/input" > "$work_dir/out" 2> "$work_dir/err"
test "$(wc -c < "$work_dir/out")" -eq 8388608
test "$(wc -c < "$work_dir/err")" -eq 3145728
head -c 3145728 /dev/zero | tr '\000' O > "$work_dir/expected"
cat "$work_dir/input" >> "$work_dir/expected"; cmp "$work_dir/expected" "$work_dir/out"
head -c 3145728 /dev/zero | tr '\000' E > "$work_dir/expected"; cmp "$work_dir/expected" "$work_dir/err"
# Deadline/disconnect must cancel a guest stdin worker blocked on a sleeping child.
status=0
head -c 1048576 "$work_dir/input" > "$work_dir/blocked-input"
"${base[@]}" --timeout 1 -- "$fixture" --child sleep < "$work_dir/blocked-input" > "$work_dir/out" 2> "$work_dir/err" || status=$?
test "$status" -eq 124
grep -q READY "$work_dir/out"
grep -q 'session timed out' "$work_dir/err"
run_status 3 "$fixture" --child exit259
# Kill a live frontend to exercise peer-loss cleanup over the installed provider.
"${base[@]}" -- "$fixture" --child sleep < /dev/null > "$work_dir/out" 2> "$work_dir/err" &
frontend_pid=$!
ready=0
for attempt in $(seq 1 400); do
    if grep -q READY "$work_dir/out"; then ready=1; break; fi
    if ! kill -0 "$frontend_pid" 2>/dev/null; then break; fi
    sleep 0.05
done
if test "$ready" -ne 1; then kill -KILL "$frontend_pid" 2>/dev/null || true; wait "$frontend_pid" || true; exit 1; fi
kill -KILL "$frontend_pid"
wait "$frontend_pid" 2>/dev/null || true
run_status 3 "$fixture" --child exit259
./build/vm_terminal ./build/waddle "$WADDLE_VSOCK_CID" "$source_path"
for iteration in $(seq 1 32); do run_status 3 "$fixture" --child exit259; done
printf 'VM acceptance: streams, status, argv, cwd/env, export read/write, 16 MiB duplex, timeout/peer-loss cleanup, interactive console, 32 reconnects passed\n'
