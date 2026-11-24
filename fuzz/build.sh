#!/bin/sh
# Build a libFuzzer + ASan target: fuzz_convert (drmtap_convert_dmabuf, the
# default) or fuzz_packet (drmtap_convert_packet).
# Usage: fuzz/build.sh [output-path] [convert|packet]
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=${2:-convert}
out=${1:-/tmp/fuzz_$target}

clang -g -O1 -std=c11 \
    -fsanitize=fuzzer,address \
    -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -DHAVE_EGL=1 -DHAVE_SECCOMP=1 -DHAVE_LIBCAP=1 \
    $(pkg-config --cflags libdrm) \
    -I "$here/include" -I "$here/src" -I "$here/tests" \
    $(ls "$here"/src/*.c | grep -v privilege_helper_stub.c) "$here/fuzz/fuzz_$target.c" \
    $(pkg-config --libs libdrm) -ldl -lm -lseccomp -lcap \
    -o "$out"

echo "built $out"
