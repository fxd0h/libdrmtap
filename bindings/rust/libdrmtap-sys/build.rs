// build.rs — Compile libdrmtap C sources statically via cc crate
//
// No external shared library needed — all C code is embedded in this crate.

fn main() {
    let csrc = std::path::Path::new("csrc");

    cc::Build::new()
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
        .include("/usr/include/libdrm")
        .define("_GNU_SOURCE", None)
        .define("_POSIX_C_SOURCE", "200809L")
        .flag("-std=c11")
        .flag("-Wno-unused-parameter")
        .flag("-Wno-sign-compare")
        .compile("drmtap");

    // System dependencies
    println!("cargo:rustc-link-lib=drm");
    println!("cargo:rustc-link-lib=EGL");
    println!("cargo:rustc-link-lib=GLESv2");
    println!("cargo:rustc-link-lib=seccomp");
    println!("cargo:rustc-link-lib=cap");
}
