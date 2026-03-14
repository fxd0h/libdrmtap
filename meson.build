# SPDX-License-Identifier: MIT

project('libdrmtap', 'c',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'c_std=c11',
        'warning_level=3',
        'werror=true',
        'b_ndebug=if-release',
    ],
)

# ============================================================================
# Dependencies
# ============================================================================

libdrm_dep = dependency('libdrm', version: '>= 2.4.113')
threads_dep = dependency('threads')

# Optional: libva for GPU-accelerated backends
libva_dep = dependency('libva', required: false)
libva_drm_dep = dependency('libva-drm', required: false)

# Optional: EGL/GLES2 for GPU-universal detiling backend
egl_dep = dependency('egl', required: false)
glesv2_dep = dependency('glesv2', required: false)

# Optional: libseccomp for the helper binary
seccomp_dep = dependency('libseccomp', required: false)

# Optional: libcap for capability management
libcap_dep = dependency('libcap', required: false)

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

lib_deps = [libdrm_dep, threads_dep]

# Add GPU backends if VAAPI is available
if libva_dep.found() and libva_drm_dep.found()
    lib_sources += files(
        'src/gpu_intel.c',
        'src/gpu_amd.c',
    )
    lib_deps += [libva_dep, libva_drm_dep]
endif

# Nvidia backend (no extra deps — CPU deswizzle)
lib_sources += files('src/gpu_nvidia.c')

# EGL/GLES2 GPU-universal detiling backend
lib_sources += files('src/gpu_egl.c')
if egl_dep.found() and glesv2_dep.found()
    lib_deps += [egl_dep, glesv2_dep]
    add_project_arguments('-DHAVE_EGL=1', language: 'c')
    message('EGL backend: enabled (GPU-universal detiling)')
else
    message('EGL backend: disabled (stub only)')
endif

# Cursor capture
lib_sources += files('src/cursor.c')

# ============================================================================
# Build the shared + static library
# ============================================================================

lib_include = include_directories('include', 'src')

libdrmtap = library('drmtap',
    lib_sources,
    include_directories: lib_include,
    dependencies: lib_deps,
    version: meson.project_version(),
    soversion: '0',
    install: true,
)

# Install the public header
install_headers('include/drmtap.h')

# pkg-config file
pkg = import('pkgconfig')
pkg.generate(libdrmtap,
    name: 'libdrmtap',
    description: 'DRM/KMS screen capture library for Linux',
    url: 'https://github.com/fxd0h/libdrmtap',
)

# Declare dependency for subproject use
libdrmtap_dep = declare_dependency(
    link_with: libdrmtap,
    include_directories: lib_include,
)

# ============================================================================
# Helper binary (privileged)
# ============================================================================

helper_deps = [libdrm_dep]
helper_c_args = []

if seccomp_dep.found()
    helper_deps += seccomp_dep
    helper_c_args += '-DHAVE_SECCOMP=1'
endif

if libcap_dep.found()
    helper_deps += libcap_dep
    helper_c_args += '-DHAVE_LIBCAP=1'
endif

executable('drmtap-helper',
    'helper/drmtap-helper.c',
    dependencies: helper_deps,
    c_args: helper_c_args,
    install: true,
    install_dir: get_option('libexecdir'),
)

# ============================================================================
# Tests
# ============================================================================

test_enumerate = executable('test_enumerate',
    'tests/test_enumerate.c',
    dependencies: [libdrmtap_dep, libdrm_dep],
    include_directories: lib_include,
)

test_capture = executable('test_capture',
    'tests/test_capture.c',
    dependencies: [libdrmtap_dep, libdrm_dep],
    include_directories: lib_include,
)

test_formats = executable('test_formats',
    'tests/test_formats.c',
    dependencies: [libdrmtap_dep],
    include_directories: lib_include,
)
test_deswizzle = executable('test_deswizzle',
    'tests/test_deswizzle.c',
    dependencies: [libdrmtap_dep],
    include_directories: lib_include,
)
test_helper = executable('test_helper',
    'tests/test_helper.c',
    dependencies: [libdrmtap_dep],
    include_directories: lib_include,
)

# Unit tests (no hardware needed)
test('formats', test_formats, suite: 'unit')
test('deswizzle', test_deswizzle, suite: 'unit')
test('helper', test_helper, suite: 'unit')

# Integration tests (need vkms or real GPU)
test('enumerate', test_enumerate, suite: 'integration',
    env: ['DRM_DEVICE=/dev/dri/card1'],
    is_parallel: false,
)

test('capture', test_capture, suite: 'integration',
    env: ['DRM_DEVICE=/dev/dri/card1'],
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
