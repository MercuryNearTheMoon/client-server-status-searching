#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
compiler=${CC:-cc}
for mode in normal sanitized; do
    if [ "$mode" = sanitized ] && [ "${SANITIZE:-1}" != 1 ]; then continue; fi
    directory="build/tests/client/$mode"
    mkdir -p "$directory"
    flags='-std=c11 -Wall -Wextra -Wpedantic -Werror -g -pthread'
    link=''
    if [ "$mode" = sanitized ]; then
        flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer"
        link='-fsanitize=address,undefined'
    fi
    "$compiler" $flags -Iutil test/client/main.c util/io.c util/protocol.c util/logger.c -o "$directory/unit"
    "$directory/unit"
    if [ "$(uname -s)" = Linux ]; then
        make -B CC="$compiler" BUILD_DIR="$directory/app" CFLAGS="$flags" LDFLAGS="$link" all
        LD_LIBRARY_PATH="$directory/app/lib" python3 test/client/integration.py "$directory/app/client" "$directory/app/server"
    else
        "$compiler" $flags -Iutil client/main.c util/io.c util/protocol.c util/logger.c -o "$directory/client"
        python3 test/client/integration.py "$directory/client"
    fi
    echo "PASS client/main $mode"
done
