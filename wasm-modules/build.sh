#!/bin/sh
# Compiles a .c file into a lean .wasm module for the WAMR interpreter on
# RIOT. Flags are modeled after RIOT's own WASM example:
#   <RIOT>/examples/lang_support/community/wasm/wasm_sample/Makefile
#
# Usage: ./build.sh demo_module.c
#        -> produces demo_module.wasm in the same directory

set -e

SRC="$1"
if [ -z "$SRC" ]; then
    echo "Usage: $0 <source.c>" >&2
    exit 1
fi

BASE=$(basename "$SRC" .c)
OUT_O="${BASE}.o"
OUT_WASM="${BASE}.wasm"

CLANG=${CLANG:-clang}
WASM_LD=${WASM_LD:-wasm-ld}

"$CLANG" \
    -c \
    -Wall \
    --target=wasm32-unknown-unknown-wasm \
    -Os \
    -flto \
    -fvisibility=hidden \
    -ffunction-sections \
    -fdata-sections \
    -o "$OUT_O" \
    "$SRC"

"$WASM_LD" \
    -o "$OUT_WASM" \
    -z stack-size=4096 \
    --export=run \
    --export=__heap_base \
    --export=__data_end \
    --allow-undefined \
    --strip-all \
    --export-dynamic \
    -error-limit=0 \
    --lto-O3 \
    -O3 \
    --gc-sections \
    --initial-memory=65536 \
    --no-entry \
    "$OUT_O"

echo "OK: wrote $OUT_WASM ($(wc -c < "$OUT_WASM") bytes)"
