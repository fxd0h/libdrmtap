#!/usr/bin/env bash
# Single-command version stamp for libdrmtap.
#
# Updates every place that must carry the *current* library version:
#   1. include/drmtap.h                         (DRMTAP_VERSION_* macros)
#   2. bindings/rust/libdrmtap-sys/csrc/drmtap.h (the crate's bundled copy)
#   3. meson.build                               (project() version)
#   4. bindings/rust/libdrmtap-sys/Cargo.toml    (crate version)
#   5. bindings/rust/libdrmtap/Cargo.toml        (its libdrmtap-sys dependency)
#
# It does NOT touch the libdrmtap wrapper crate's own version (that is an
# intentionally separate 0.3.x line), nor prose in docs/READMEs. Run
# tools/check-version.sh afterwards (CI does) to catch anything left behind.
#
# Usage: tools/set-version.sh X.Y.Z
set -euo pipefail

usage() { echo "usage: $0 X.Y.Z" >&2; exit 2; }
[ $# -eq 1 ] || usage
ver="$1"
echo "$ver" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || { echo "error: version must be X.Y.Z (got '$ver')" >&2; exit 2; }
IFS=. read -r major minor patch <<<"$ver"
root="$(cd "$(dirname "$0")/.." && pwd)"

stamp_header() {  # $1 = header file with DRMTAP_VERSION_* macros
    sed -i -E \
        -e "s/^#define DRMTAP_VERSION_MAJOR .*/#define DRMTAP_VERSION_MAJOR $major/" \
        -e "s/^#define DRMTAP_VERSION_MINOR .*/#define DRMTAP_VERSION_MINOR $minor/" \
        -e "s/^#define DRMTAP_VERSION_PATCH .*/#define DRMTAP_VERSION_PATCH $patch/" \
        "$1"
}
stamp_header "$root/include/drmtap.h"
stamp_header "$root/bindings/rust/libdrmtap-sys/csrc/drmtap.h"

# meson project() version — the only `version: '<semver>'` line (the other
# `version:` uses meson.project_version(), which has no quoted digits).
sed -i -E \
    "s/^([[:space:]]*version:[[:space:]]*)'[0-9]+\.[0-9]+\.[0-9]+'/\1'$ver'/" \
    "$root/meson.build"

# libdrmtap-sys [package] version — the FIRST `version = "<semver>"` line.
sed -i -E \
    "0,/^version = \"[0-9]+\.[0-9]+\.[0-9]+\"/s//version = \"$ver\"/" \
    "$root/bindings/rust/libdrmtap-sys/Cargo.toml"

# The wrapper crate's dependency ON libdrmtap-sys (not the wrapper's own line).
sed -i -E \
    "s/(libdrmtap-sys = \{ version = )\"[0-9]+\.[0-9]+\.[0-9]+\"/\1\"$ver\"/" \
    "$root/bindings/rust/libdrmtap/Cargo.toml"

echo "stamped version $ver across the 5 code sites"
echo "next: tools/check-version.sh  (verify) and, if C sources changed, tools/sync-crate.sh"
