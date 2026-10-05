#!/bin/sh
# Acceptance Test: VirtIO-FS Live Cross-Filesystem Integration Demonstration
# Verifies mock export path mapping; real VirtIO-FS acceptance is a separate VM gate.

set -eu

echo "=== VirtIO-FS Live Filesystem Acceptance Test ==="

demo_dir=$(mktemp -d /tmp/waddle_fs_demo_XXXXXX)
host_shared="$demo_dir/shared"
mkdir -p "$host_shared"

mock_pid=""
cleanup() {
    if [ -n "$mock_pid" ]; then
        kill "$mock_pid" 2>/dev/null || true
        wait "$mock_pid" 2>/dev/null || true
    fi
    rm -rf "$demo_dir"
}
trap cleanup EXIT HUP INT TERM

export WADDLE_MOCK_ROOT="$host_shared"
export HOME="$host_shared"
export XDG_CONFIG_HOME="$demo_dir/config"
export XDG_STATE_HOME="$demo_dir/state"
export XDG_RUNTIME_DIR="$demo_dir/runtime"
./build/waddle init fs-demo >/dev/null

# Step 1: Run built-in CLI filesystem self-test
echo "[Step 1] Running 'waddle fs test'..."
./build/waddle fs test

# Step 2: Start Mock Guest Execution Agent
echo "[Step 2] Starting mock guest agent on UNIX socket..."
./build/waddle-mock-guest --socket-path "$demo_dir/guest.sock" &
mock_pid=$!

attempt=0
while [ ! -S "$demo_dir/guest.sock" ]; do
    kill -0 "$mock_pid"
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo 'Mock guest startup timed out' >&2
        exit 1
    fi
    sleep 0.02
done

# Step 3: Guest creates directory and file inside the shared mount
echo "[Step 3] Executing guest command to create directory and file in shared volume..."
./build/waddle exec --pipe \
    --socket-path "$demo_dir/guest.sock" \
    --path-map "$host_shared=Z:\\" \
    --cwd "$host_shared" -- \
    sh -c 'mkdir -p new_project_folder && echo "live_waddle_fs_verified" > new_project_folder/guest_output.txt'

wait "$mock_pid"
mock_pid=""

# Step 4: Verify live Linux host visibility (dir & file instantly visible with zero copy)
echo "[Step 4] Verifying immediate appearance of guest artifacts on Linux host..."
if [ ! -d "$host_shared/new_project_folder" ]; then
    echo "ERROR: Directory new_project_folder was not found on host!" >&2
    exit 1
fi

if [ ! -f "$host_shared/new_project_folder/guest_output.txt" ]; then
    echo "ERROR: File guest_output.txt was not found on host!" >&2
    exit 1
fi

content=$(cat "$host_shared/new_project_folder/guest_output.txt")
if [ "$content" != "live_waddle_fs_verified" ]; then
    echo "ERROR: Unexpected content in guest_output.txt: $content" >&2
    exit 1
fi

echo "  -> Directory and file verified on Linux host filesystem:"
ls -ld "$host_shared/new_project_folder"
ls -l "$host_shared/new_project_folder/guest_output.txt"

# Step 5: Host writes file into shared directory; verify guest can read it
echo "[Step 5] Host writes into shared folder; starting guest to read back..."
echo "host_authored_content" > "$host_shared/new_project_folder/host_file.txt"

./build/waddle-mock-guest --socket-path "$demo_dir/guest.sock" &
mock_pid=$!

attempt=0
while [ ! -S "$demo_dir/guest.sock" ]; do
    kill -0 "$mock_pid"
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo 'Mock guest startup timed out' >&2
        exit 1
    fi
    sleep 0.02
done

guest_read=$(./build/waddle exec --pipe \
    --socket-path "$demo_dir/guest.sock" \
    --path-map "$host_shared=Z:\\" \
    --cwd "$host_shared" -- \
    sh -c 'cat new_project_folder/host_file.txt')

wait "$mock_pid"
mock_pid=""

if [ "$guest_read" != "host_authored_content" ]; then
    echo "ERROR: Guest failed to read host file accurately: $guest_read" >&2
    exit 1
fi

echo "  -> Guest accurately read host-created file: $guest_read"
echo "=== VirtIO-FS Acceptance Test Passed Successfully! ==="
