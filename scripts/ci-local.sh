#!/bin/sh
# Local/CI entry point; needs a C11 compiler, no Docker or test framework.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
compiler=${CC:-cc}
if [ "$#" -gt 1 ]; then
    echo "Usage: sh scripts/ci-local.sh [all|util/protocol|util/logger|util/io|server/main|client/main]" >&2
    exit 2
fi
case "${1:-all}" in
    all|'') units='util/protocol util/logger util/io client/main'
        if [ "$(uname -s)" = Linux ]; then units="$units server/main";
        else echo 'SKIP server/main: Linux required; run in Docker'; fi ;;
    util/protocol|util/logger|util/io|server/main|client/main) units=$1 ;;
    *) echo "Unknown unit: $1 (available: util/protocol, util/logger, util/io, server/main, client/main)" >&2; exit 2 ;;
esac

for unit in $units; do
    if [ "$unit" = client/main ]; then
        sh scripts/ci-client.sh
        continue
    fi
    if [ "$unit" = server/main ]; then
        sh scripts/ci-server.sh
        continue
    fi
    set -- "test/$unit.c" "$unit.c"
    if [ "$unit" != util/logger ]; then
        set -- "$@" util/logger.c
    fi
    test_dir="build/tests/$unit"
    mkdir -p "$test_dir"
    for debug in 0 1; do
        for level in -1 0 1 2 3; do
            name="debug${debug}-level${level}"
            binary="$test_dir/$name"
            "$compiler" -std=c11 -Wall -Wextra -Wpedantic -Werror -I. \
                -DENABLE_DEBUG="$debug" -DSYSINFO_COMPILE_LOG_LEVEL="$level" \
                "$@" -o "$binary"
            if ! "$binary" > "$binary.stdout" 2> "$binary.stderr"; then
                cat "$binary.stdout" "$binary.stderr"
                exit 1
            fi
            echo "PASS $unit $name"
        done
    done

    # Enabled by default; opt out explicitly if the compiler lacks support.
    if [ "${SANITIZE:-1}" = 1 ]; then
        binary="$test_dir/sanitized"
        "$compiler" -std=c11 -Wall -Wextra -Wpedantic -Werror -g -I. \
            -fsanitize=address,undefined -fno-omit-frame-pointer \
            -DENABLE_DEBUG=1 -DSYSINFO_COMPILE_LOG_LEVEL=3 \
            "$@" -o "$binary"
        "$binary"
        echo "PASS $unit sanitizers"
    fi
done
echo "Local CI passed. Logs: build/tests/<unit>/"
