#!/bin/sh
# Exercise option grammar and ownership cleanup before any peer connection.
set -eu
expect_status() {
    expected=$1
    shift
    if ./build/waddle exec "$@" >/dev/null 2>/dev/null; then
        actual=0
    else
        actual=$?
    fi
    test "$actual" -eq "$expected" || {
        echo "path options: expected $expected, got $actual" >&2
        exit 1
    }
}
expect_status 2 --path-map missing -- /bin/true
expect_status 2 --path-map 'relative=X:\export' -- /bin/true
expect_status 2 --path-map '/a=X:relative' -- /bin/true
expect_status 2 --path-map '/a=X:\bad?' -- /bin/true
expect_status 2 --path-map '/a=X:\one' --path-map '/a/=Y:\two' -- /bin/true
expect_status 125 --socket-path /no/such/waddle/socket --path-map '/export=X:\shared' --cwd /unmapped -- command
expect_status 125 --socket-path /no/such/waddle/socket --path-map '/export=X:\shared' --cwd relative -- /unmapped
expect_status 125 --socket-path /no/such/waddle/socket --path-map '/export=X:\shared' --cwd /export -- /export/program
set --
i=0
while test "$i" -lt 65; do
    set -- "$@" --path-map "/source$i=X:\\shared"
    i=$((i + 1))
done
expect_status 2 "$@" -- command

# Automatic rules describe configured exports only; a failure must occur before
# the absent peer is contacted. Explicit root exports still reach that peer.
mapping_root=$(mktemp -d)
trap 'rm -rf "$mapping_root"' EXIT HUP INT TERM
mkdir -p "$mapping_root/waddle" "$mapping_root/shared"
check_auto_mapping() {
    expected_error=$1
    shift
    if LC_ALL=C XDG_CONFIG_HOME="$mapping_root" ./build/waddle exec \
        --socket-path "$mapping_root/missing.sock" --translate-path \
        --cwd 'C:\' -- "$@" >"$mapping_root/out" 2>"$mapping_root/error"; then
        echo 'automatic mapping unexpectedly succeeded' >&2
        exit 1
    else
        actual=$?
    fi
    test "$actual" -eq 125
    grep -q "$expected_error" "$mapping_root/error"
}
printf '[filesystem]\nexport=%s:Z:\\:rw\n' "$mapping_root/shared" >"$mapping_root/waddle/config.ini"
check_auto_mapping 'Invalid cross-device link' /outside/tool.exe
check_auto_mapping 'Invalid cross-device link' "$mapping_root/shared-sibling/tool.exe"
check_auto_mapping 'No such file or directory' "$mapping_root/shared/tool.exe"
cat >"$mapping_root/waddle/config.ini" <<'EOF'
[filesystem]
EOF
check_auto_mapping 'Invalid cross-device link' /outside/tool.exe
cat >"$mapping_root/waddle/config.ini" <<'EOF'
[filesystem]
export=/:Z:\:rw
EOF
check_auto_mapping 'No such file or directory' /outside/tool.exe
cat >"$mapping_root/waddle/config.ini" <<'EOF'
[av]
unknown=1
EOF
check_auto_mapping 'Invalid argument' /outside/tool.exe
echo 'path options: syntax, duplicates, limits, export-only mapping and failures passed'
