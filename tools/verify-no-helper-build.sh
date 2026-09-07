#!/bin/bash
# Proves what -Dhelper=disabled is supposed to buy: a library with NO fork/exec
# path, not merely a build that skips installing the helper binary.
# Builds both ways and compares. Run from the source root.
set -u
SRC="${1:-$PWD}"
A=$(mktemp -d); B=$(mktemp -d); trap 'rm -rf "$A" "$B"' EXIT
fail=0
ok() { printf 'PASS  %s\n' "$1"; }
no() { printf 'FAIL  %s\n' "$1"; fail=1; }
# Fails when there is no library to inspect. Without this, a missing artifact
# makes nm and strings return nothing and every "no spawn symbols" check passes
# for the wrong reason - the same shape of false pass this script exists to stop.
so_of() {
    local so
    so=$(ls "$1"/libdrmtap.so.0.* 2>/dev/null | grep -v '\.p$' | head -1)
    [ -n "$so" ] && [ -f "$so" ] || return 1
    printf '%s\n' "$so"
}
# The symbols a spawn needs. Matched WITHOUT a $ anchor: nm prints them
# versioned (fork@GLIBC_2.2.5), and anchoring silently matched nothing - the
# first version of this check reported 0 for both builds and proved nothing.
SPAWN='(^|[[:space:]])(fork|execl|execv|execve|posix_spawn|socketpair|waitpid)@'

meson setup "$A" "$SRC" -Dhelper=disabled >"$A/setup.log" 2>&1 && ninja -C "$A" >"$A/build.log" 2>&1 \
  && ok "builds with -Dhelper=disabled" || { no "-Dhelper=disabled does not build"; tail -5 "$A/setup.log" "$A/build.log"; }
meson setup "$B" "$SRC" >"$B/setup.log" 2>&1 && ninja -C "$B" >"$B/build.log" 2>&1 \
  && ok "builds with the default (helper=auto)" || { no "the default build broke"; tail -5 "$B/build.log"; }

# POSITIVE control first: if the default build shows no spawn symbols either,
# the check is broken, not the code.
SO_B=$(so_of "$B") || { no "the default build produced no .so to inspect"; SO_B=/dev/null; }
SO_A=$(so_of "$A") || { no "the -Dhelper=disabled build produced no .so to inspect"; SO_A=/dev/null; }
n_def=$(nm -D --undefined-only "$SO_B" 2>/dev/null | grep -cE "$SPAWN")
[ "$n_def" -gt 0 ] && ok "control: the default .so does reference spawn symbols ($n_def)" \
                   || no "control failed: the default .so shows none either, so this check discriminates nothing"
n_off=$(nm -D --undefined-only "$SO_A" 2>/dev/null | grep -cE "$SPAWN")
[ "$n_off" -eq 0 ] && ok "the -Dhelper=disabled .so references no fork/exec/socketpair" \
                   || { no "spawn symbols survive with the helper disabled:"; nm -D --undefined-only "$SO_A" | grep -E "$SPAWN"; }

# The search paths are strings; they should be gone too, or the .so still tells
# an attacker where a helper would be looked for.
n_paths=$(strings "$SO_A" | grep -c '^/usr.*drmtap-helper$')
[ "$n_paths" -eq 0 ] && ok "no helper search paths left in the .so" || { no "$n_paths search path(s) still in the .so"; }
[ -x "$A/drmtap-helper" ] && no "the helper binary was built anyway" || ok "no helper binary produced"
[ -x "$B/drmtap-helper" ] && ok "control: the default build does produce the helper" \
                          || printf 'SKIP  the default build produced no helper (libseccomp/libcap missing here)\n'

for d in "$A" "$B"; do
  r=$( cd "$d" && meson test 2>&1 | grep -E '^Fail:' | tr -s ' ' )
  case "$r" in *"Fail: 0"*) ok "tests pass in $(basename "$d")";; *) no "tests fail in $(basename "$d"): $r";; esac
done

echo
[ $fail -eq 0 ] && echo "ALL CHECKS PASS" || echo "SOMETHING FAILED"
exit $fail
