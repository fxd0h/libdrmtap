# Release tooling

The libdrmtap version lives in several files that different build systems read
(the C header for `drmtap_version()`, `meson.build`, the crate `Cargo.toml`, and
the crate's bundled copy of the C header). Cargo requires a literal version in
`Cargo.toml`, so it cannot be reduced to a single physical constant — instead
one script stamps them all and CI fails the build if any drift.

**Canonical source of truth:** the `DRMTAP_VERSION_*` macros in
`include/drmtap.h`.

## Cutting a release

```sh
tools/set-version.sh X.Y.Z   # stamp all version sites at once
tools/sync-crate.sh          # if any C source changed: refresh the crate's csrc/
tools/check-version.sh       # verify everything agrees (CI runs this too)
git commit -am "release: X.Y.Z"
# publish sys first, then the wrapper if it changed:
(cd bindings/rust && cargo publish -p libdrmtap-sys)
```

`major` in the version is also the `.so` soversion / ABI major — bump it only on
a breaking ABI change (see `DRMTAP_ABI_MAJOR` in the rustdesk loader).

## The scripts

| Script | What it does |
| --- | --- |
| `set-version.sh X.Y.Z` | Stamps the 5 code sites (C header, csrc header, meson, `libdrmtap-sys` crate, and the wrapper's dependency on it). Leaves the wrapper crate's own separate version line and prose in docs untouched. |
| `sync-crate.sh [--check]` | Regenerates `bindings/rust/libdrmtap-sys/csrc/` from the library sources (with the one packaging include-path fixup). `--check` fails on drift instead of writing — used by CI. |
| `check-version.sh` | Fails if any version site disagrees with the canonical header, or if `csrc/` has drifted from the library. Also prints a non-fatal advisory listing doc version strings for manual review. Wired into CI as the `version-coherence` job. |

Docs and READMEs carry version numbers in prose (both "current" pointers and
historical statements). Those are intentionally not machine-stamped; the
`check-version.sh` advisory lists them so a human can update the current ones and
leave the historical ones alone.
