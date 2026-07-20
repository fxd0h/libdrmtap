# SPDX-License-Identifier: MIT

project('libdrmtap', 'c',
    version: '0.4.11',
    license: 'MIT',
    default_options: [
        'c_std=c11',
        'warning_level=3',
        'werror=true',
        'b_ndebug=if-release',
    ],
)

# ============================================================================
# Version coherence
# ============================================================================
# The library version lives in three places: this project() version, the
# DRMTAP_VERSION_* macros in include/drmtap.h (returned by drmtap_version()),
# and the Rust crate version (cross-checked by the version unit tests). Parse
# the public header here and fail configuration early if it drifts from the
# project version, so the sources can never silently diverge.
fs = import('fs')
_ver_parts = {'MAJOR': '', 'MINOR': '', 'PATCH': ''}
foreach _line : fs.read('include/drmtap.h').split('\n')
    _tokens = _line.split()
    if _tokens.length() >= 3 and _tokens[0] == '#define'
        foreach _part : ['MAJOR', 'MINOR', 'PATCH']
            if _tokens[1] == 'DRMTAP_VERSION_' + _part
                _ver_parts += {_part: _tokens[2]}
            endif
        endforeach
    endif
endforeach
_hdr_version = '@0@.@1@.@2@'.format(
    _ver_parts['MAJOR'], _ver_parts['MINOR'], _ver_parts['PATCH'])
assert(_hdr_version == meson.project_version(),
    'version mismatch: meson project version is ' + meson.project_version() +
    ' but DRMTAP_VERSION_* in include/drmtap.h is ' + _hdr_version +
    ' — keep them equal')

# ============================================================================
# Dependencies
# ============================================================================

# The real floor is the FB2 GEM API (drmModeGetFB2), which landed in libdrm
# 2.4.95. HDR metadata (struct hdr_output_metadata) came in a newer libdrm than
# some bases ship, but it is optional and guarded behind HAVE_HDR_METADATA (the
# has_type probe below), so it must not raise the hard floor. Requiring 2.4.113
# needlessly broke builds on common LTS bases (e.g. ubuntu 18.04 ships 2.4.101).
libdrm_dep = dependency('libdrm', version: '>= 2.4.95')
cc = meson.get_compiler('c')
have_hdr_metadata = cc.has_type('struct hdr_output_metadata',
    prefix: '#include <xf86drmMode.h>\n#include <drm_mode.h>',
    dependencies: libdrm_dep)
if have_hdr_metadata
    add_project_arguments('-DHAVE_HDR_METADATA=1', language: 'c')
endif
threads_dep = dependency('threads')

# Optional: libva for GPU-accelerated backends
libva_dep = dependency('libva', required: false)
libva_drm_dep = dependency('libva-drm', required: false)

# Optional: EGL/GLES2 for GPU-universal detiling backend
egl_dep = dependency('egl', required: false)
glesv2_dep = dependency('glesv2', required: false)

# libseccomp (seccomp allowlist) and libcap (capability drop) confine the
# privileged helper. They are NOT optional when the helper is built — they are
# resolved as mandatory in the helper section below so an unconfined helper is
# never produced. Building the library alone does not need them.

# libm: HDR tone-mapping in pixel_convert.c uses pow() (PQ EOTF / sRGB OETF).
m_dep = meson.get_compiler('c').find_library('m', required: false)

# ============================================================================
# Library sources
# ============================================================================

lib_sources = files(
    'src/drmtap.c',
    'src/drm_enumerate.c',
    'src/drm_grab.c',
    'src/gpu_generic.c',
    'src/pixel_convert.c',
    'src/privilege_helper.c',
)

lib_deps = [libdrm_dep, threads_dep, m_dep]

# GPU backends: Intel and AMD (always compiled — match functions needed by drm_grab.c)
lib_sources += files(
    'src/gpu_intel.c',
    'src/gpu_amd.c',
)

# Link VAAPI if available (used by gpu_intel/gpu_amd _process functions)
if libva_dep.found() and libva_drm_dep.found()
    lib_deps += [libva_dep, libva_drm_dep]
    add_project_arguments('-DHAVE_VAAPI=1', language: 'c')
    message('VAAPI backends: enabled (Intel/AMD GPU-accelerated)')
else
    message('VAAPI backends: disabled (match-only stubs)')
endif

# Nvidia backend (no extra deps — CPU deswizzle)
lib_sources += files('src/gpu_nvidia.c')

# EGL/GLES2 GPU-universal detiling backend.
# Compile-time headers ONLY: the backend dlopen()s libEGL/libGLESv2 lazily on
# first use (see src/gpu_egl.c), so the GPU stack must never appear in the .so
# DT_NEEDED NOR in libdrmtap.pc Requires.private (a static consumer resolving
# the .pc would otherwise re-link -lEGL/-lGLESv2 into its own — possibly
# privileged — binary, defeating the whole split). We therefore do NOT attach
# egl/glesv2 as dependencies of the library target; we only pull their include
# dirs in as plain compile args. dl IS a real link dep (dlopen) and stays.
dl_dep = cc.find_library('dl', required: false)
lib_sources += files('src/gpu_egl.c')
have_egl = egl_dep.found() and glesv2_dep.found() \
    and cc.has_header('EGL/egl.h') and cc.has_header('EGL/eglext.h') \
    and cc.has_header('GLES2/gl2.h') and cc.has_header('GLES2/gl2ext.h')
if have_egl
    # Add the pkg-config include dirs (if any) as compile args, without making
    # egl/glesv2 dependencies of the target — so they stay out of the .pc.
    egl_incs = run_command('pkg-config', '--cflags-only-I', 'egl', 'glesv2',
                           check: false)
    if egl_incs.returncode() == 0
        foreach _inc : egl_incs.stdout().split()
            add_project_arguments(_inc, language: 'c')
        endforeach
    endif
    if dl_dep.found()
        lib_deps += dl_dep
    endif
    add_project_arguments('-DHAVE_EGL=1', language: 'c')
    message('EGL backend: enabled (GPU-universal detiling, lazy dlopen)')
else
    message('EGL backend: disabled (stub only)')
endif

# Cursor capture
lib_sources += files('src/cursor.c')

# ============================================================================
# Build the shared + static library
# ============================================================================

lib_include = include_directories('include', 'src')

# Export ONLY the public API from the shared object (the one the privileged
# service dlopen()s): a linker version script keeps every internal symbol
# local. Guard on GNU-ld support so a toolchain without --version-script still
# builds (it just exports the default symbol set there).
version_script = meson.current_source_dir() / 'libdrmtap.map'
libdrmtap_link_args = []
if cc.has_link_argument('-Wl,--version-script,' + version_script)
    libdrmtap_link_args += '-Wl,--version-script,' + version_script
endif

# Build BOTH a shared and a static library from one compile. The shared object
# (the artifact the privileged service dlopen()s) is locked down to the public
# ABI by the version script above. The static archive keeps every symbol so the
# white-box unit tests can reach internal helpers (e.g. drmtap_convert_rgb16);
# the version script does not apply to an `ar` archive.
libdrmtap_both = both_libraries('drmtap',
    lib_sources,
    include_directories: lib_include,
    dependencies: lib_deps,
    version: meson.project_version(),
    soversion: '0',
    link_args: libdrmtap_link_args,
    link_depends: 'libdrmtap.map',
    install: true,
)
libdrmtap = libdrmtap_both.get_shared_lib()

# Install the public header
install_headers('include/drmtap.h')

# pkg-config file
pkg = import('pkgconfig')
pkg.generate(libdrmtap,
    name: 'libdrmtap',
    description: 'DRM/KMS screen capture library for Linux',
    url: 'https://github.com/fxd0h/libdrmtap',
)

# Public dependency (shared, version-scripted): examples/subprojects link this
# and can only reach the public API.
libdrmtap_dep = declare_dependency(
    link_with: libdrmtap,
    include_directories: lib_include,
)

# Test-only dependency (static, full symbols): unit tests white-box internal
# helpers not part of the public ABI.
libdrmtap_test_dep = declare_dependency(
    link_with: libdrmtap_both.get_static_lib(),
    include_directories: lib_include,
)

# ============================================================================
# Helper binary (privileged)
# ============================================================================

# Fail closed: the helper is confined by seccomp + a capability drop, so both
# libseccomp and libcap are MANDATORY whenever it is built. With the feature
# 'enabled' a missing dependency hard-errors; with 'auto' it degrades to
# library-only (helper skipped) rather than shipping an unconfined helper; with
# 'disabled' the helper is not built at all.
helper_feature = get_option('helper')
build_helper = false
if not helper_feature.disabled()
    # 'enabled' -> hard require (error if missing); 'auto' -> allow graceful skip.
    confine_required = helper_feature.enabled()
    seccomp_dep = dependency('libseccomp', required: confine_required)
    libcap_dep = dependency('libcap', required: confine_required)
    if seccomp_dep.found() and libcap_dep.found()
        build_helper = true
    elif helper_feature.enabled()
        # Unreachable (required:true already errored), kept as a defensive guard.
        error('drmtap-helper requires libseccomp and libcap for confinement')
    else
        message('drmtap-helper: libseccomp/libcap not found; building library only')
    endif
endif

if build_helper
helper_deps = [libdrm_dep, seccomp_dep, libcap_dep]
helper_c_args = ['-DHAVE_SECCOMP=1', '-DHAVE_LIBCAP=1']

if have_hdr_metadata
    helper_c_args += '-DHAVE_HDR_METADATA=1'
endif

# Exploit-mitigation hardening for the privileged (CAP_SYS_ADMIN) helper.
helper_c_args += '-fstack-protector-strong'
# _FORTIFY_SOURCE needs an optimized build; at -O0 — and in 'plain' mode, where
# Meson adds no -O flag at all — it only warns, and the CI build uses -Werror, so
# enable it only when optimization is on. -Og ('g') satisfies Fortify, so keep it.
if get_option('optimization') not in ['plain', '0']
    helper_c_args += ['-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=2']
endif
helper_link_args = ['-Wl,-z,relro,-z,now']

executable('drmtap-helper',
    'helper/drmtap-helper.c',
    dependencies: helper_deps,
    c_args: helper_c_args,
    link_args: helper_link_args,
    pie: true,
    install: true,
    install_dir: get_option('libexecdir'),
    # Install NON-world-executable (0750, owner root). A file capability applies
    # to everyone who can exec the binary, so a world-exec cap_sys_admin helper
    # would let any local user read the DRM scanout. A bare `meson install` must
    # therefore not leave it 0755; the packager/admin then chowns it to a trusted
    # capture group and applies the capability (see README.md / SECURITY.md).
    install_mode: ['rwxr-x---', 'root', 'root'],
)
endif # build_helper

# ============================================================================
# Tests
# ============================================================================

# Unit/integration tests link the STATIC lib (libdrmtap_test_dep) so they can
# white-box internal helpers the version-scripted .so no longer exports.
test_enumerate = executable('test_enumerate',
    'tests/test_enumerate.c',
    dependencies: [libdrmtap_test_dep, libdrm_dep],
    include_directories: lib_include,
)

test_capture = executable('test_capture',
    'tests/test_capture.c',
    dependencies: [libdrmtap_test_dep, libdrm_dep],
    include_directories: lib_include,
)

test_formats = executable('test_formats',
    'tests/test_formats.c',
    dependencies: [libdrmtap_test_dep],
    include_directories: lib_include,
)
test_deswizzle = executable('test_deswizzle',
    'tests/test_deswizzle.c',
    dependencies: [libdrmtap_test_dep],
    include_directories: lib_include,
)
test_helper = executable('test_helper',
    'tests/test_helper.c',
    dependencies: [libdrmtap_test_dep],
    include_directories: lib_include,
)
test_hdr = executable('test_hdr',
    'tests/test_hdr.c',
    dependencies: [libdrmtap_test_dep, m_dep],
    include_directories: lib_include,
)
test_convert = executable('test_convert',
    'tests/test_convert.c',
    dependencies: [libdrmtap_test_dep],
    include_directories: lib_include,
)
# Wire-protocol tests are self-contained (socketpair + fake peer, no DRM, no
# libdrmtap link): they guard the helper IPC's fd-access-mode / CLOEXEC /
# ancillary-truncation / fixed-frame-reassembly invariants.
test_wire = executable('test_wire',
    'tests/test_wire.c',
)

# Unit tests (no hardware needed)
test('formats', test_formats, suite: 'unit')
test('deswizzle', test_deswizzle, suite: 'unit')
test('helper', test_helper, suite: 'unit')
test('hdr', test_hdr, suite: 'unit')
test('wire', test_wire, suite: 'unit')
test('convert', test_convert, suite: 'unit')

# Integration tests (need vkms or real GPU)
# LSan: suppress the deliberate process-lifetime EGL/Mesa display allocation
# (see tests/lsan.supp) so a real leak in a sanitized run stands out.
lsan_supp = meson.project_source_root() / 'tests' / 'lsan.supp'
integration_env = [
    'DRM_DEVICE=/dev/dri/card1',
    'LSAN_OPTIONS=suppressions=' + lsan_supp,
]

test('enumerate', test_enumerate, suite: 'integration',
    env: integration_env,
    is_parallel: false,
)

test('capture', test_capture, suite: 'integration',
    env: integration_env,
    is_parallel: false,
)

# ============================================================================
# Examples
# ============================================================================

executable('screenshot',
    'examples/screenshot.c',
    dependencies: [libdrmtap_dep],
    include_directories: lib_include,
    install: false,
)

executable('stream',
    'examples/stream.c',
    dependencies: [libdrmtap_dep],
    include_directories: lib_include,
    install: false,
)

# Split-capture reference: privileged export + unprivileged convert across a
# process boundary (fork + SCM_RIGHTS fd-pass). Exercises drmtap_grab_desc +
# drmtap_open_render + drmtap_convert_dmabuf end to end.
executable('split_capture',
    'examples/split_capture.c',
    dependencies: [libdrmtap_dep],
    include_directories: lib_include,
    install: false,
)

# VNC server example (optional — requires libvncserver)
libvncserver_dep = dependency('libvncserver', required: false)
if libvncserver_dep.found()
    executable('vnc_server',
        'examples/vnc_server.c',
        dependencies: [libdrmtap_dep, libvncserver_dep, libdrm_dep],
        include_directories: lib_include,
        install: false,
    )
endif
