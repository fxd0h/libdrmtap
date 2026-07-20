#!/bin/sh
# Build the libFuzzer + ASan target for drmtap_convert_dmabuf.
# Usage: fuzz/build.sh [output-path]
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-/tmp/fuzz_convert}

clang -g -O1 -std=c11 \
    -fsanitize=fuzzer,address \
    -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -DHAVE_EGL=1 -DHAVE_SECCOMP=1 -DHAVE_LIBCAP=1 \
    $(pkg-config --cflags libdrm) \
    -I "$here/include" -I "$here/src" \
    "$here"/src/*.c "$here"/fuzz/fuzz_convert.c \
    $(pkg-config --libs libdrm) -ldl -lm -lseccomp -lcap \
    -o "$out"

echo "built $out"
