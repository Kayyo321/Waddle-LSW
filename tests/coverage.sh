#!/bin/sh
# Enforce implementation line coverage; native Zig test bodies are excluded.
# Prerequisites: GCC/gcov, Zig 0.13 and the base-system kcov package.
set -eu
task_root=$(pwd)
mkdir -p build/coverage
rm -f build/coverage/unit-*.gcda
${CC:-cc} -D_GNU_SOURCE -Iinclude -Isrc -Isrc/common -Isrc/cli -Isrc/daemon -Isrc/guest -Isrc/mock -O0 -g -std=c11 -Wall -Wextra -Wpedantic -Werror --coverage \
    tests/unit/unit.c src/common/protocol.c src/common/arguments.c build/path_rules.o -o build/coverage/unit
./build/coverage/unit
for source in protocol arguments; do
    ${GCOV:-gcov} -b -o "build/coverage/unit-$source.gcno" "src/common/$source.c"
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
for pair in "guest/guest_codec" "common/path_rules"; do
    source_dir=$(dirname "$pair")
    source_name=$(basename "$pair")
    ${ZIG:-zig} test "src/$pair.zig" "-femit-bin=build/${source_name}_test"
    rm -rf "build/coverage/$source_name"
    ${KCOV:-kcov} "--include-path=$task_root/src/$pair.zig" "build/coverage/$source_name" "build/${source_name}_test"
    awk '/^test / { in_test=1 } in_test { print NR } in_test && /^}/ { in_test=0 }' \
        "src/$pair.zig" > "build/coverage/${source_name}_test_lines"
    awk -F'"' -v source="$source_name.zig" '
        NR == FNR { excluded[$1]=1; next }
        /<line number=/ && !($2 in excluded) { total++; if ($4 + 0 > 0) covered++ }
        END {
            if (total == 0) exit 1
            percent = covered * 100 / total
            printf "%s implementation coverage: %.2f%% (%d/%d)\n", source, percent, covered, total
            if (percent < 90) exit 1
        }' "build/coverage/${source_name}_test_lines" build/coverage/"$source_name"/"${source_name}_test".*/cobertura.xml
done

