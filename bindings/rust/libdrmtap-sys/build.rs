// build.rs — Compile libdrmtap C sources statically via cc crate
//
// No external shared library needed — all C code is embedded in this crate.

fn main() {
    let csrc = std::path::Path::new("csrc");

    // Recompile when any embedded C source or header changes.
    println!("cargo:rerun-if-changed=csrc");
    if let Ok(entries) = std::fs::read_dir(csrc) {
        for e in entries.flatten() {
            println!("cargo:rerun-if-changed={}", e.path().display());
        }
    }

    let mut build = cc::Build::new();
    build
        .files(&[
            csrc.join("drmtap.c"),
            csrc.join("drm_enumerate.c"),
            csrc.join("drm_grab.c"),
            csrc.join("privilege_helper.c"),
            csrc.join("pixel_convert.c"),
            csrc.join("cursor.c"),
            csrc.join("gpu_egl.c"),
            csrc.join("gpu_intel.c"),
            csrc.join("gpu_amd.c"),
            csrc.join("gpu_nvidia.c"),
            csrc.join("gpu_generic.c"),
        ])
        .include(csrc)
        .define("_GNU_SOURCE", None)
        .define("_POSIX_C_SOURCE", "200809L")
        .define("HAVE_EGL", "1")
        .define("HAVE_SECCOMP", "1")
        .define("HAVE_LIBCAP", "1")
        .flag("-std=c11")
        .flag("-Wno-unused-parameter")
        .flag("-Wno-sign-compare");

    // libdrm headers (xf86drm.h, xf86drmMode.h) live in a libdrm subdir. Find
    // them portably via pkg-config, falling back to the common Debian path.
    match pkg_config::Config::new().cargo_metadata(false).probe("libdrm") {
        Ok(lib) => {
            for inc in &lib.include_paths {
                build.include(inc);
            }
        }
        Err(_) => {
            build.include("/usr/include/libdrm");
        }
    }

    build.compile("drmtap");

    // System dependencies.
    println!("cargo:rustc-link-lib=drm");
    println!("cargo:rustc-link-lib=EGL");
    println!("cargo:rustc-link-lib=GLESv2");
    println!("cargo:rustc-link-lib=seccomp");
    println!("cargo:rustc-link-lib=cap");
}
