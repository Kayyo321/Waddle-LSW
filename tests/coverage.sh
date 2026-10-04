#!/bin/sh
# Enforce implementation line coverage; native Zig test bodies are excluded.
# Prerequisites: GCC/gcov, Zig 0.13 and the base-system kcov package.
set -eu
task_root=$(pwd)
mkdir -p build/coverage
rm -f build/coverage/unit-*.gcda
${CC:-cc} -D_GNU_SOURCE -Iinclude -Isrc -O0 -g -std=c11 -Wall -Wextra -Wpedantic -Werror --coverage \
    tests/unit.c src/protocol.c src/arguments.c build/path_rules.o -o build/coverage/unit
./build/coverage/unit
for source in protocol arguments; do
    ${GCOV:-gcov} -b -o "build/coverage/unit-$source.gcno" "src/$source.c"
    mv "$source.c.gcov" "build/coverage/$source.c.gcov"
    awk -F: -v source="$source.c" '
        $1 ~ /^[[:space:]]*([0-9]+[*]?|#####|=====)[[:space:]]*$/ { total++; if ($1 + 0 > 0) covered++ }
        END {
            if (total == 0) exit 1
            percent = covered * 100 / total
            printf "%s implementation coverage: %.2f%% (%d/%d)\n", source, percent, covered, total
            if (percent < 90) exit 1
        }' "build/coverage/$source.c.gcov"
done
for source in guest_codec path_rules; do
    ${ZIG:-zig} test "src/$source.zig" "-femit-bin=build/${source}_test"
    rm -rf "build/coverage/$source"
    ${KCOV:-kcov} "--include-path=$task_root/src/$source.zig" "build/coverage/$source" "build/${source}_test"
    awk '/^test / { in_test=1 } in_test { print NR } in_test && /^}/ { in_test=0 }' \
        "src/$source.zig" > "build/coverage/${source}_test_lines"
    awk -F'"' -v source="$source.zig" '
        NR == FNR { excluded[$1]=1; next }
        /<line number=/ && !($2 in excluded) { total++; if ($4 + 0 > 0) covered++ }
        END {
            if (total == 0) exit 1
            percent = covered * 100 / total
            printf "%s implementation coverage: %.2f%% (%d/%d)\n", source, percent, covered, total
            if (percent < 90) exit 1
        }' "build/coverage/${source}_test_lines" build/coverage/"$source"/"${source}_test".*/cobertura.xml
done
