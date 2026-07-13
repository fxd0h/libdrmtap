#!/usr/bin/env bash
# Regenerate the libdrmtap-sys crate's bundled C sources (the csrc/ directory)
# from the canonical library sources.
#
# The crate cannot reference ../src at publish time (a crates.io package must be
# self-contained), so csrc/ holds copies. Keeping them a MANUAL copy once let
# the crate ship a stale, unhardened helper under a bumped version number — this
# script makes that drift impossible: csrc/ is a deterministic function of the
# library.
#
#   tools/sync-crate.sh          copy library sources into csrc/
#   tools/sync-crate.sh --check  verify csrc/ already matches (CI); nonzero on drift
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
csrc="$root/bindings/rust/libdrmtap-sys/csrc"
check=0; [ "${1:-}" = "--check" ] && check=1

# canonical library source  ->  csrc/ destination filename
# (must stay in step with the file list build.rs compiles)
pairs="
src/drmtap.c:drmtap.c
src/drm_enumerate.c:drm_enumerate.c
src/drm_grab.c:drm_grab.c
src/privilege_helper.c:privilege_helper.c
src/pixel_convert.c:pixel_convert.c
src/cursor.c:cursor.c
src/gpu_egl.c:gpu_egl.c
src/gpu_intel.c:gpu_intel.c
src/gpu_amd.c:gpu_amd.c
src/gpu_nvidia.c:gpu_nvidia.c
src/gpu_generic.c:gpu_generic.c
helper/drmtap-helper.c:drmtap-helper.c
include/drmtap.h:drmtap.h
src/drmtap_internal.h:drmtap_internal.h
src/wire.h:wire.h
"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
rc=0

for pair in $pairs; do
    srcrel="${pair%%:*}"
    dst="${pair##*:}"
    [ -f "$root/$srcrel" ] || { echo "error: missing source $srcrel" >&2; exit 1; }
    # Apply the one packaging fixup: in csrc/ wire.h is a sibling, not at ../src/.
    sed 's#include "../src/wire.h"#include "wire.h"#' "$root/$srcrel" > "$tmp/$dst"

    if [ "$check" = 1 ]; then
        if ! diff -q "$csrc/$dst" "$tmp/$dst" >/dev/null 2>&1; then
            echo "DRIFT: csrc/$dst differs from $srcrel" >&2
            rc=1
        fi
    else
        cp "$tmp/$dst" "$csrc/$dst"
    fi
done

if [ "$check" = 1 ]; then
    [ $rc -eq 0 ] && echo "csrc/ is in sync with the library"
    exit $rc
fi
echo "csrc/ regenerated from the library"
