#!/bin/sh
# Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause
# Build from the distribution's source selection, even in a configured tree.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: build.sh native-build-directory output-directory" >&2
	exit 1
fi
native_build=$(cd "$1" && pwd -P)
mkdir -p "$2"
output=$(cd "$2" && pwd -P)
for tool in emcc emconfigure emmake python3; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "$tool is required to build Wasm" >&2
		exit 1
	}
done
mkdir -p "$native_build/disposable"
work=$(mktemp -d "$native_build/disposable/wasm.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# Source and build directories are siblings: configure rejects an out-of-tree
# build whose source directory was configured in place.
make -C "$native_build" distdir am__skip_mode_fix=yes distdir="$work/source"
mkdir "$work/build" "$work/dist"
cd "$work/build"
: "${EM_CACHE:=$native_build/disposable/em-cache}"
export EM_CACHE
emconfigure "$work/source/configure" --host=wasm32-unknown-emscripten \
    --disable-shared --disable-threads --with-libedit=no CFLAGS=-O2
emmake make -j2 librelic.la
if [ "${RELIC_WASM_CHECK:-no}" = yes ]; then
	for suite in api arith value query regex; do
		emcc -O2 -I. -I"$work/source/include" -I"$work/source/lib" \
		    -I"$work/source/compat" -I"$work/source/tests/unit" \
		    "$work/source/tests/unit/test_$suite.c" \
		    "$work/source/tests/unit/harness.c" .libs/librelic.a \
		    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=268435456 \
		    -sSTACK_SIZE=1048576 -o "test-$suite.cjs"
		node "test-$suite.cjs"
		echo "PASS Wasm C suite: $suite"
	done
fi
emcc -std=gnu11 -O2 -Wall -Wextra -Werror -I. -I"$work/source/include" \
    "$work/source/wasm/adapter.c" .libs/librelic.a \
    --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=268435456 \
    -sSTACK_SIZE=1048576 -sABORTING_MALLOC=0 -sDYNAMIC_EXECUTION=0 \
    -sEXPORTED_FUNCTIONS=_malloc,_free,_relic_wasm_evaluate,_relic_version \
    -sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8 \
    -o "$work/dist/relic-engine.mjs"
cp "$work/source/wasm/relic.mjs" "$work/source/wasm/relic-worker.mjs" \
    "$work/source/wasm/relic.d.mts" "$work/source/wasm/README.md" \
    "$work/source/LICENSE" "$work/dist/"
python3 "$work/source/wasm/notices.py" "$work/dist"
cp "$work/dist/"* "$output/"
echo "Wasm distribution: $output"
