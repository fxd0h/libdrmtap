#!/usr/bin/env bash
# The symbols libdrmtap.so exports must be exactly the global list of
# libdrmtap.map, and every one of them must be declared in include/drmtap.h.
#   tools/check-abi-symbols.sh <libdrmtap.so> [<repo root>]
set -euo pipefail
so="$1"
root="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
map_syms=$(sed -n '/global:/,/local:/p' "$root/libdrmtap.map" | grep -oE '^[[:space:]]*drmtap_[a-z0-9_]+;' \
    | tr -d ' ;' | sort)
so_syms=$(nm -D --defined-only "$so" | awk '$2 ~ /^[TDBR]$/ {print $3}' | sed 's/@.*//' | sort)
fail=0
if [ "$map_syms" != "$so_syms" ]; then
    echo "MISMATCH between libdrmtap.map and the exports of $so:" >&2
    diff <(echo "$map_syms") <(echo "$so_syms") >&2 || true
    fail=1
fi
for s in $map_syms; do
    if ! grep -qE "[^a-z_]$s\(" "$root/include/drmtap.h"; then
        echo "exported but not declared in include/drmtap.h: $s" >&2
        fail=1
    fi
done
[ "$fail" = 0 ] && echo "ok: $(echo "$so_syms" | wc -l) exported symbols = libdrmtap.map, all declared"
exit $fail
