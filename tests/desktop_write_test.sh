#!/bin/sh
# Test: Write a file to the Linux Desktop via Waddle subsystem daemon manager
# Verifies that the shared directory mapping includes the Desktop folder

set -eu

echo "=== Desktop Write Test ==="

# Determine Desktop path (XDG user directories)
DESKTOP_DIR="${HOME}/Desktop"
if [ ! -d "$DESKTOP_DIR" ]; then
    echo "Desktop directory not found at $DESKTOP_DIR" >&2
    exit 1
fi

# Use temporary file name
TMPFILE="waddle_desktop_test_$(date +%s).txt"
FULLPATH="$DESKTOP_DIR/$TMPFILE"

# Ensure any previous leftover is removed
rm -f "$FULLPATH"

# Set up mock guest for testing since CI lacks /dev/kvm
export WADDLE_MOCK_GUEST_SOCK="/tmp/waddle_mock_guest_$$.sock"
./build/waddle-mock-guest --socket-path "$WADDLE_MOCK_GUEST_SOCK" > /dev/null 2>&1 &
MOCK_PID=$!
sleep 0.5

# Run waddle to write a file in the shared mount (Desktop is under default mapping)
./build/waddle exec \
  --cwd "$DESKTOP_DIR" -- \
  sh -c "echo 'waddle desktop test' > '$TMPFILE'"

# Cleanup mock guest
kill $MOCK_PID 2>/dev/null || true
rm -f "$WADDLE_MOCK_GUEST_SOCK"

# Verify file exists on host
if [ -f "$FULLPATH" ]; then
    echo "File successfully created on Desktop: $FULLPATH"
    cat "$FULLPATH"
    rm -f "$FULLPATH"
    echo "Desktop write test passed."
    exit 0
else
    echo "Failed to create file on Desktop" >&2
    exit 1
fi
