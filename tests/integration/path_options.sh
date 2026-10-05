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
echo 'path options: syntax, duplicates, limits and mapping failures passed'
