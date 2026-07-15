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

# Advisory only (never fails the build): surface version-looking strings that are
# not the canonical version so a human can decide whether each is a stale
# current-pointer (bump it) or intentional history ("shipped in X.Y.Z" — leave
# it). Scans the WHOLE repo's docs + config (every *.md, *.toml, *.rs — the
# top-level READMEs, AGENTS.md, docs/, contrib/, patches/, and the crates), NOT
# just the top-level READMEs: contrib/ + patches/ + docs/research/ carry
# current-pointer versions (integration snippets, "published on crates.io" status
# lines, scrap dependency pins) that a 4-file scan silently missed and let ship
# stale. Build artifacts are excluded. The wrapper's own 0.3.x line is filtered
# out (its version is intentionally a separate track).
echo "advisory: libdrmtap version strings that are not $ver (review: stale current-pointer vs intentional history):"
# Scan with RELATIVE paths (cd "$root") so the repo dir name ("libdrmtap") is not
# in the path — otherwise the drmtap scope filter below would match every line.
# Keep only lines whose CONTENT mentions drmtap (a libdrmtap version reference),
# dropping unrelated deps (rustdesk 1.x, tokio, etc.); drop the canonical version
# and the wrapper's separate 0.3.x line.
( cd "$root" && grep -rnE "$sem" . \
    --include='*.md' --include='*.toml' --include='*.rs' \
    --exclude-dir='.git' --exclude-dir='build' --exclude-dir='build-pkg' --exclude-dir='target' \
    2>/dev/null ) \
    | grep -iE 'drmtap' \
    | grep -vF "$ver" | grep -vE "0\.3\.[0-9]+" \
    | sed 's/^/    /' || echo "    (none)"

if [ $fail -ne 0 ]; then
    echo "version coherence FAILED" >&2
    exit 1
fi
echo "version coherence OK"
