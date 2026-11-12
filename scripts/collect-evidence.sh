#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
[ "$(uname -s)" = Linux ] || { echo 'Run this script in the Linux Docker container.' >&2; exit 2; }
mkdir -p build/evidence submission/evidence
make -B all > submission/evidence/build.txt 2>&1
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread -Iutil \
    test/demo/baseline.c util/io.c util/protocol.c util/logger.c -o build/evidence/baseline
for debug in 0 1; do
    directory="build/evidence/debug$debug"
    make -B BUILD_DIR="$directory" ENABLE_DEBUG="$debug" \
        CPPFLAGS="-Iutil -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DENABLE_DEBUG=$debug -DSYSINFO_COMPILE_LOG_LEVEL=3" \
        "$directory/client" > "submission/evidence/build-debug$debug.txt" 2>&1
done
{
    date -u
    uname -a
    cc --version
    python3 --version
    tcpdump --version
} > submission/evidence/environment.txt 2>&1
{
    ldd build/client
    ldd build/server
    readelf -d build/client
    readelf -d build/server
} > submission/evidence/linkage.txt 2>&1
python3 scripts/collect-evidence.py > submission/evidence/demo-results.txt 2>&1
tcpdump -nn -tttt -X -r submission/evidence/capture.pcap > submission/evidence/tcpdump.txt 2>&1
python3 scripts/analyze-capture.py
