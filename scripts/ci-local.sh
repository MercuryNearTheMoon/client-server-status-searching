#!/bin/sh
# Local/CI entry point; needs a C11 compiler, no Docker or test framework.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
compiler=${CC:-cc}
if [ "$#" -gt 1 ]; then
    echo "Usage: sh scripts/ci-local.sh [all|util/protocol|util/logger|util/io]" >&2
    exit 2
fi
case "${1:-all}" in
    all|'') units='util/protocol util/logger util/io' ;;
    util/protocol|util/logger|util/io) units=$1 ;;
    *) echo "Unknown unit: $1 (available: util/protocol, util/logger, util/io)" >&2; exit 2 ;;
esac

for unit in $units; do
    test_dir="build/tests/$unit"
    mkdir -p "$test_dir"
    for debug in 0 1; do
        for level in -1 0 1 2 3; do
            name="debug${debug}-level${level}"
            binary="$test_dir/$name"
            "$compiler" -std=c11 -Wall -Wextra -Wpedantic -Werror -I. \
                -DENABLE_DEBUG="$debug" -DSYSINFO_COMPILE_LOG_LEVEL="$level" \
                "test/$unit.c" "$unit.c" -o "$binary"
            if ! "$binary" > "$binary.stdout" 2> "$binary.stderr"; then
                cat "$binary.stdout" "$binary.stderr"
                exit 1
            fi
            if [ "$unit" = util/logger ]; then
                # Dedicated logging test: one diagnostic per enabled level.
                for entry in ERROR:0 WARN:1 INFO:2 DEBUG:3; do
                    tag=${entry%%:*}
                    threshold=${entry##*:}
                    expected=1
                    if [ "$level" -lt "$threshold" ] || { [ "$tag" = DEBUG ] && [ "$debug" = 0 ]; }; then
                        expected=0
                    fi
                    actual=$(awk -v tag="[$tag]" 'index($0, tag) == 1 {n++} END {print n+0}' "$binary.stderr")
                    if [ "$actual" -ne "$expected" ]; then
                        echo "FAIL $unit $name: $tag count=$actual, expected=$expected" >&2
                        cat "$binary.stderr"
                        exit 1
                    fi
                done
            fi
            echo "PASS $unit $name"
        done
    done

    # Enabled by default; opt out explicitly if the compiler lacks support.
    if [ "${SANITIZE:-1}" = 1 ]; then
        binary="$test_dir/sanitized"
        "$compiler" -std=c11 -Wall -Wextra -Wpedantic -Werror -g -I. \
            -fsanitize=address,undefined -fno-omit-frame-pointer \
            -DSYSINFO_COMPILE_LOG_LEVEL=-1 \
            "test/$unit.c" "$unit.c" -o "$binary"
        "$binary"
        echo "PASS $unit sanitizers"
    fi
done
echo "Local CI passed. Logs: build/tests/<unit>/"
