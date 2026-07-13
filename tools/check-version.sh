#!/usr/bin/env bash
# Fail if any version site disagrees with the canonical source
# (include/drmtap.h), or if the crate's bundled csrc/ sources have drifted from
# the library. This is the CI safety net so a stale version / stale source can
# never reach a release.
#
# Canonical version = the DRMTAP_VERSION_* macros in include/drmtap.h.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
h="$root/include/drmtap.h"
sem='[0-9]+\.[0-9]+\.[0-9]+'

hv() { grep -E "^#define DRMTAP_VERSION_$1 " "$2" | awk '{print $3}'; }
ver="$(hv MAJOR "$h").$(hv MINOR "$h").$(hv PATCH "$h")"
echo "canonical version (include/drmtap.h) = $ver"

fail=0
check() {  # $1 = description, $2 = actual, $3 = expected
    if [ "$2" != "$3" ]; then
        echo "  MISMATCH: $1 = '$2' (expected '$3')" >&2
        fail=1
    else
        echo "  ok: $1 = $2"
    fi
}

csh="$root/bindings/rust/libdrmtap-sys/csrc/drmtap.h"
check "csrc/drmtap.h macros" \
    "$(hv MAJOR "$csh").$(hv MINOR "$csh").$(hv PATCH "$csh")" "$ver"

mver=$(grep -E "^[[:space:]]*version:[[:space:]]*'$sem'" "$root/meson.build" \
    | head -1 | grep -oE "$sem")
check "meson project version" "$mver" "$ver"

sver=$(grep -E "^version = \"$sem\"" "$root/bindings/rust/libdrmtap-sys/Cargo.toml" \
    | head -1 | grep -oE "$sem")
check "libdrmtap-sys crate version" "$sver" "$ver"

dver=$(grep -E "libdrmtap-sys = \{ version = \"$sem\"" \
    "$root/bindings/rust/libdrmtap/Cargo.toml" | grep -oE "$sem" | head -1)
check "wrapper's libdrmtap-sys dependency" "$dver" "$ver"

# The crate's bundled C sources must match the library byte-for-byte
# (modulo the packaging include-path fixup that sync-crate.sh applies).
echo "checking csrc/ source drift..."
if ! "$root/tools/sync-crate.sh" --check; then
    fail=1
fi

# Advisory only (never fails the build): surface version-looking strings in the
# user-facing docs that are not the canonical version, so a human can decide
# whether they are stale current-pointers or intentional history.
echo "advisory: doc version strings that are not $ver (review manually):"
grep -rnoE "$sem" \
    "$root/README.md" "$root/AGENTS.md" \
    "$root/bindings/rust/libdrmtap-sys/README.md" \
    "$root/bindings/rust/libdrmtap/README.md" 2>/dev/null \
    | grep -vE ":$ver$" | grep -vE "0\.3\.[0-9]+$" | sed 's/^/    /' || echo "    (none)"

if [ $fail -ne 0 ]; then
    echo "version coherence FAILED" >&2
    exit 1
fi
echo "version coherence OK"
