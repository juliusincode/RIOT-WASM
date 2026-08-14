# Fed into WAMR's CMake build via -DWAMR_CONFIG by pkg/wamr/Makefile.include
# (see RIOT: examples/lang_support/community/wasm/config.cmake).
#
# Deliberately kept lean for this prototype:
#  - no WASI (we use our own lightweight control protocol instead of a
#    full POSIX environment -> saves flash)
#  - the built-in mini libc is enough for printf() etc. in the demo modules
#  - pure interpreter, no JIT (wouldn't make sense on the C6 anyway)

set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_BULK_MEMORY 1)
