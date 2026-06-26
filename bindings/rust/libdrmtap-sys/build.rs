// build.rs — Compile libdrmtap C sources statically via cc crate.
// Also compiles drmtap-helper as a standalone executable and emits its path
// as cargo:HELPER_BIN so downstream build scripts can copy/install it.

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
    // libm: HDR tone-mapping in pixel_convert.c uses pow()/tanh().
    println!("cargo:rustc-link-lib=m");

    // Compile drmtap-helper as a standalone executable.
    // It inherits a socketpair fd from the parent, opens the DRM device with
    // CAP_SYS_ADMIN and returns DMA-BUF fds via SCM_RIGHTS.  The binary is
    // written to OUT_DIR and its path is emitted as cargo:HELPER_BIN so that
    // downstream build scripts (e.g. rustdesk's build.rs) can copy or install it.
    let out_dir = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let helper_out = out_dir.join("drmtap-helper");

    let compiler = build.get_compiler();
    let mut cmd = compiler.to_command();
    cmd.arg("-o").arg(&helper_out)
        .arg(csrc.join("drmtap-helper.c"))
        .arg(format!("-I{}", csrc.display()))
        .arg("-D_GNU_SOURCE")
        .arg("-D_POSIX_C_SOURCE=200809L")
        .arg("-DHAVE_SECCOMP=1")
        .arg("-DHAVE_LIBCAP=1")
        // Exploit-mitigation hardening for this privileged (CAP_SYS_ADMIN) binary.
        .arg("-O2")
        .arg("-fstack-protector-strong")
        .arg("-U_FORTIFY_SOURCE")
        .arg("-D_FORTIFY_SOURCE=2")
        .arg("-fPIE")
        .arg("-pie")
        .arg("-Wl,-z,relro,-z,now");

    // Include libdrm headers for drmtap-helper.c (same probe as the library).
    match pkg_config::Config::new().cargo_metadata(false).probe("libdrm") {
        Ok(lib) => {
            for inc in &lib.include_paths {
                cmd.arg(format!("-I{}", inc.display()));
            }
        }
        Err(_) => { cmd.arg("-I/usr/include/libdrm"); }
    }

    cmd.arg("-ldrm").arg("-lcap").arg("-lseccomp");

    let status = cmd.status().expect("failed to compile drmtap-helper");
    assert!(status.success(), "drmtap-helper compilation failed");

    println!("cargo:rerun-if-changed={}", csrc.join("drmtap-helper.c").display());
    // Expose the compiled helper path to downstream crates via DEP_DRMTAP_HELPER_BIN.
    println!("cargo:HELPER_BIN={}", helper_out.display());
}
