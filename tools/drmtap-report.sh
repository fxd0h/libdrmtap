#!/usr/bin/env bash
# Collect everything a libdrmtap capture bug report needs, in one run, so a
# report does not turn into five rounds of "please also paste X".
#
# Most libdrmtap failures are decided by facts the reporter has no reason to
# think are relevant: whether the process has CAP_SYS_ADMIN or is going through
# the helper, whether the build actually has the EGL backend (meson treats it as
# optional and silently ships a CPU-only stub without it), which grab entry point
# was called, and the scanout modifier. This gathers those together with an
# actual capture attempt and says what the result means.
#
#   tools/drmtap-report.sh                 build in ./build, capture, analyze
#   tools/drmtap-report.sh --build DIR     use an existing meson build dir
#   tools/drmtap-report.sh --so PATH       point at an installed .so instead
#
# Prints to stdout. It reports hardware, driver and build facts only: no
# hostname, no user name, no window contents (the captured image is reduced to
# "how many non-black bytes", never included).

set -uo pipefail

BUILD_DIR=build
SO_PATH=
KEEP_IMAGE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD_DIR=${2:?--build needs a directory}; shift 2 ;;
        --so)    SO_PATH=${2:?--so needs a path}; shift 2 ;;
        --keep-image) KEEP_IMAGE=1; shift ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

say() { printf '\n== %s ==\n' "$1"; }
val() { printf '%-22s %s\n' "$1" "$2"; }

# ---------------------------------------------------------------- environment
say "system"
val kernel "$(uname -r)"
val arch "$(uname -m)"
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    val distro "$(. /etc/os-release; echo "${PRETTY_NAME:-$ID}")"
fi
val session_type "${XDG_SESSION_TYPE:-unset}"
val wayland_display "${WAYLAND_DISPLAY:-unset}"
val x_display "${DISPLAY:-unset}"

say "drm devices"
for dev in /dev/dri/*; do
    [ -e "$dev" ] || continue
    perms=$(stat -c '%A %U:%G' "$dev" 2>/dev/null)
    val "$(basename "$dev")" "$perms"
done
for card in /sys/class/drm/card[0-9]*; do
    [ -d "$card" ] || continue
    name=$(basename "$card")
    case "$name" in *-*) continue ;; esac   # skip connector entries
    drv=$(basename "$(readlink -f "$card/device/driver" 2>/dev/null)" 2>/dev/null)
    val "$name driver" "${drv:-unknown}"
done
say "connectors (status, current mode)"
for conn in /sys/class/drm/card[0-9]*-*; do
    [ -r "$conn/status" ] || continue
    st=$(cat "$conn/status" 2>/dev/null)
    # First entry of modes is the active/preferred one. A capture bug is often about
    # a specific mode (a refresh rate, or a resolution that does not match the fb),
    # so do not label this "modes" and then print only the status.
    mode=$(head -1 "$conn/modes" 2>/dev/null)
    val "$(basename "$conn")" "$st${mode:+ $mode}"
done

# A para-virtualized driver hides its cursor plane from an atomic client that has not
# declared DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, which read as "cursor permanently hidden"
# until 0.5.4. Report what the driver actually offers so a cursor report needs no round trip.
say "cursor plane (needs root; blank means the dump is unreadable here)"
for st in /sys/kernel/debug/dri/[0-9]*/state; do
    [ -r "$st" ] || continue
    dri=$(basename "$(dirname "$st")")
    planes=$(grep -c '^plane\[' "$st" 2>/dev/null)
    # Anything but "(null)" is bound. Matching CRTC names instead would only count the
    # drivers whose naming we happen to have seen.
    bound=$(grep -A 2 '^plane\[' "$st" 2>/dev/null | grep -c 'crtc=[^(]')
    val "dri/$dri planes" "$planes total, $bound bound to a crtc"
done

# ------------------------------------------------------------------ the build
say "libdrmtap build"
if [ -d .git ]; then
    val git_commit "$(git rev-parse HEAD 2>/dev/null)"
    val git_describe "$(git describe --tags --always --dirty 2>/dev/null)"
    val git_dirty "$(test -n "$(git status --porcelain 2>/dev/null)" && echo yes || echo no)"
else
    val git_commit "(not run from a libdrmtap checkout)"
fi

if [ -z "$SO_PATH" ]; then
    # Prefer the build dir, then a system install. The REAL object, not a symlink,
    # so the version in the name is the one actually loaded.
    for cand in "$BUILD_DIR"/libdrmtap.so.0.*.* \
                /usr/local/lib/libdrmtap.so.0.*.* \
                /usr/lib/libdrmtap.so.0.*.*; do
        if [ -f "$cand" ] && [ ! -L "$cand" ]; then SO_PATH=$cand; break; fi
    done
fi
val so_path "${SO_PATH:-NOT FOUND}"

if [ -n "$SO_PATH" ] && [ -f "$SO_PATH" ]; then
    # The EGL backend is dlopen'd by name, so it never shows in ldd; the string is
    # present only if src/gpu_egl.c was compiled in. 0 here means the build is a
    # CPU-only stub and cannot detile ANY tiled scanout, whatever the version says.
    egl_count=$(strings "$SO_PATH" 2>/dev/null | grep -c 'libEGL\.so\.1')
    val egl_backend "$([ "$egl_count" -gt 0 ] && echo "present" || echo "ABSENT (built without egl)")"
    val gles_symbols "$(strings "$SO_PATH" 2>/dev/null | grep -c 'eglCreateImageKHR')"
else
    val egl_backend "unknown (no .so found)"
fi

if command -v meson >/dev/null 2>&1 && [ -d "$BUILD_DIR/meson-info" ]; then
    opt=$(meson introspect "$BUILD_DIR" --buildoptions 2>/dev/null \
        | tr '{' '\n' | grep '"name": "egl"' | grep -o '"value": "[^"]*"' | cut -d'"' -f4)
    [ -n "$opt" ] && val meson_egl_opt "$opt"
fi

say "helper"
helper=$(command -v drmtap-helper 2>/dev/null)
[ -z "$helper" ] && [ -x /usr/local/bin/drmtap-helper ] && helper=/usr/local/bin/drmtap-helper
[ -z "$helper" ] && [ -x "$BUILD_DIR/drmtap-helper" ] && helper=$BUILD_DIR/drmtap-helper
val helper_path "${helper:-not installed}"
if [ -n "$helper" ]; then
    val helper_caps "$(getcap "$helper" 2>/dev/null || echo '(getcap unavailable)')"
    val helper_perms "$(stat -c '%A %U:%G' "$helper" 2>/dev/null)"
fi
# Whether THIS process could capture without the helper at all. Read the EFFECTIVE
# capability set straight from /proc: `capsh --print | grep cap_sys_admin` also
# matches the bounding set, so it answers "yes" for a process that does not have it
# and flatly contradicts the "No CAP_SYS_ADMIN (needs helper)" line in the log below.
caps_eff=$(awk '/^CapEff:/ {print $2}' /proc/self/status 2>/dev/null)
if [ -n "$caps_eff" ]; then
    # CAP_SYS_ADMIN is bit 21.
    if (( (0x$caps_eff >> 21) & 1 )); then
        val caller_sysadmin "yes (capture runs in-process)"
    else
        val caller_sysadmin "no, uid $(id -u) (capture must go through the helper)"
    fi
else
    val caller_sysadmin "$([ "$(id -u)" = 0 ] && echo root || echo "no (uid $(id -u))")"
fi

# ------------------------------------------------------------- capture attempt
say "capture attempt (examples/screenshot, drmtap_grab_mapped)"
shot=$BUILD_DIR/screenshot
if [ ! -x "$shot" ]; then
    echo "no $shot; build first:  meson setup $BUILD_DIR -Degl=enabled && ninja -C $BUILD_DIR"
    exit 1
fi

img=$(mktemp -t drmtap-report-XXXXXX.ppm)
log=$(mktemp -t drmtap-report-XXXXXX.log)
cleanup() { [ "$KEEP_IMAGE" = 1 ] || rm -f "$img"; rm -f "$log"; }
trap cleanup EXIT

DRMTAP_DEBUG=1 "$shot" > "$img" 2> "$log"
rc=$?
val exit_code "$rc"
val ppm_bytes "$(stat -c %s "$img" 2>/dev/null || echo 0)"

# Is the image actually black? Strip the PPM header, then count bytes that are
# not 0x00. A correct desktop has millions; a black frame has none. This is the
# single number that separates "the detile produced nothing" from "it produced
# a wrong image" from "it worked".
ppm_ok=0
if [ -s "$img" ]; then
    # Parse the PPM header ("P6\n<w> <h>\n255\n") rather than assuming it: the byte
    # after its third newline is where pixels start, and a fixed guess leaves header
    # bytes in the count -- "255\n" alone is enough non-zero data to stop a fully
    # black frame from reading as black, the one case this section exists to catch.
    #
    # And validate it before drawing any conclusion. A truncated write, or anything
    # else that lands on stdout, would otherwise be reported as "every pixel is
    # black", which accuses the EGL path of a failure that did not happen.
    read -r magic ppm_w ppm_h ppm_max hdr_len < <(
        head -c 64 "$img" | LC_ALL=C awk '
            BEGIN { RS="\n" }
            { n++; len += length($0) + 1
              # $1, not $0: arbitrary stdout can have spaces on its first line, and a
              # multi-word field here would shift every later one in the read below,
              # leaving hdr_len holding words instead of a number.
              if (n == 1) m = $1
              else if (n == 2) { w = $1; h = $2 }
              else if (n == 3) { print m, w+0, h+0, $0+0, len; exit } }')
    body() { tail -c "+$((hdr_len + 1))" "$img"; }
    if [ "$magic" != "P6" ]; then
        val ppm_valid "no (stdout is not a P6 ppm)"
    elif [ "${ppm_w:-0}" -le 0 ] || [ "${ppm_h:-0}" -le 0 ] || [ "${ppm_max:-0}" -ne 255 ]; then
        val ppm_valid "no (bad header ${ppm_w}x${ppm_h} max=${ppm_max})"
    else
        want=$(( ppm_w * ppm_h * 3 ))
        got=$(( $(stat -c %s "$img") - hdr_len ))
        if [ "$got" -ne "$want" ]; then
            val ppm_valid "no (truncated: ${got} pixel bytes, expected ${want} for ${ppm_w}x${ppm_h})"
        else
            ppm_valid=yes
            ppm_ok=1
            val ppm_valid "yes (${ppm_w}x${ppm_h})"
        fi
    fi
fi

if [ "$ppm_ok" = 1 ]; then
    nonzero=$(body | tr -d '\000' | wc -c)
    val nonblack_bytes "$nonzero"
    total=$(( ppm_w * ppm_h * 3 ))
    if [ "$total" -gt 0 ]; then
        val nonblack_pct "$(( nonzero * 100 / total ))%"
    fi
    # A tiled buffer read as if it were linear is not black, it is structured
    # noise; distinct byte values separate that from a solid fill. Sampled from the
    # first 2 MB: od plus sort over a whole 4K frame is tens of seconds for a number
    # that does not get more informative with more input.
    val distinct_bytes "$(body | head -c 2000000 | od -An -tu1 -v 2>/dev/null \
        | tr -s ' ' '\n' | sort -u | grep -c .)"
fi

say "verdict"
if grep -q 'unknown command' "$log"; then
    # The helper speaks a versioned command frame and closes the connection on a
    # mismatch, so an installed helper older than the library fails as an opaque
    # "unknown command" plus a generic helper-failed errno. Name it here, because
    # nothing in that pair points at the actual cause.
    echo "the INSTALLED HELPER IS STALE: it does not speak this library's helper"
    echo "protocol. reinstall it from the same build as the .so above. keep it"
    echo "restricted while you do -- it carries CAP_SYS_ADMIN, so it must not be"
    echo "world-executable (SECURITY.md), which is why this is not a plain install:"
    # %q so a build dir with a space or a quote in it cannot produce a copy-paste
    # line that means something other than what it looks like.
    printf '    sudo install -m 0750 -o root -g <capture-group> %q /usr/local/bin/drmtap-helper\n' \
        "$BUILD_DIR/drmtap-helper"
    echo "    sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper"
    echo "then re-run this script. everything below is downstream of that."
elif [ "$ppm_ok" = 0 ] && [ "$rc" -eq 0 ]; then
    echo "the capture reported success but its output is not a usable ppm (see"
    echo "ppm_valid above). that is a problem with the run, not with the detile;"
    echo "the debug log below says what happened."
elif [ "$rc" -ne 0 ]; then
    echo "capture FAILED. the error line from the log:"
    # screenshot.c has three failure messages, not two: drmtap_open failing prints
    # "Failed to open". Without it this heading is printed with nothing under it.
    grep -iE 'Capture failed|No pixel data|Failed to open' "$log" | head -3
elif [ "${nonzero:-0}" -eq 0 ]; then
    echo "capture returned a frame but every pixel is black."
    echo "the EGL convert result below is the diagnosis."
else
    echo "capture produced a non-black image (${nonzero} non-zero bytes)."
    echo "if it still looks wrong, it is decoded with the wrong tiling, not missing."
fi

say "decisive log lines"
grep -E 'CAP_SYS_ADMIN|needs helper|helper V3|helper:|FB2:|EGL check|EGL convert|auto-process|fast2:|not CPU-mappable' \
    "$log" | head -40

say "full debug log"
cat "$log"

if [ "$KEEP_IMAGE" = 1 ]; then
    say "image kept"
    echo "$img"
fi
