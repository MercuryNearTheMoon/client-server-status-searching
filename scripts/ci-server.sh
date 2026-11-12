#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
if [ "$(uname -s)" != Linux ]; then
    echo 'Server requires Linux. Run: docker compose run --rm compile make test UNIT=server/main' >&2
    exit 2
fi
compiler=${CC:-cc}
for mode in normal sanitized; do
    if [ "$mode" = sanitized ] && [ "${SANITIZE:-1}" != 1 ]; then continue; fi
    directory="build/tests/server/$mode"
    mkdir -p "$directory"
    flags='-std=c11 -Wall -Wextra -Wpedantic -Werror -g -pthread'
    link=''
    if [ "$mode" = sanitized ]; then
        flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer"
        link='-fsanitize=address,undefined'
    fi
    # Intentional word splitting for compiler flag lists.
    "$compiler" $flags -Iutil test/server/main.c util/io.c util/protocol.c util/logger.c -o "$directory/unit"
    "$directory/unit"
    make -B CC="$compiler" BUILD_DIR="$directory/app" CFLAGS="$flags" LDFLAGS="$link" "$directory/app/server"
    LD_LIBRARY_PATH="$directory/app/lib" python3 test/server/integration.py "$directory/app/server"
    echo "PASS server/main $mode"
done
