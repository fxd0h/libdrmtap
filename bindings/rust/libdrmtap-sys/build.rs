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
    let drm_includes: Vec<std::path::PathBuf> =
        match pkg_config::Config::new().cargo_metadata(false).probe("libdrm") {
            Ok(lib) => lib.include_paths,
            Err(_) => vec![std::path::PathBuf::from("/usr/include/libdrm")],
        };
    for inc in &drm_includes {
        build.include(inc);
    }

    // Feature-detect HDR metadata: old libdrm (e.g. Ubuntu 18.04) lacks
    // `struct hdr_output_metadata` in drm_mode.h. drm_grab.c / drmtap-helper.c
    // guard the HDR readout behind HAVE_HDR_METADATA so they still build there
    // (HDR is simply skipped when the struct is unavailable).
    let have_hdr = probe_hdr_metadata(&build, &drm_includes);
    if have_hdr {
        build.define("HAVE_HDR_METADATA", "1");
    }

    build.compile("drmtap");

    // System dependencies. EGL/GLESv2 are intentionally NOT linked: gpu_egl.c
    // dlopen()s them lazily on first convert so a privileged process that
    // never converts never maps the GPU stack (their headers are still needed
    // at compile time).
    println!("cargo:rustc-link-lib=drm");
    println!("cargo:rustc-link-lib=seccomp");
    println!("cargo:rustc-link-lib=cap");
    // libm: HDR tone-mapping in pixel_convert.c uses pow()/tanh().
    println!("cargo:rustc-link-lib=m");
    // libdl: only a real library pre-glibc-2.34; harmless (a stub) after.
    println!("cargo:rustc-link-lib=dl");

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

    if have_hdr {
        cmd.arg("-DHAVE_HDR_METADATA=1");
    }

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

// Probe whether the available libdrm defines `struct hdr_output_metadata`
// (added in a newer libdrm than some LTS bases ship). True if a tiny snippet
// using the struct compiles; drives the HAVE_HDR_METADATA guard in the C.
fn probe_hdr_metadata(build: &cc::Build, drm_includes: &[std::path::PathBuf]) -> bool {
    let out_dir = match std::env::var("OUT_DIR") {
        Ok(d) => std::path::PathBuf::from(d),
        Err(_) => return false,
    };
    let test_c = out_dir.join("hdr_probe.c");
    let src = concat!(
        "#include <xf86drmMode.h>\n",
        "#include <drm_mode.h>\n",
        "int main(void) {\n",
        "    struct hdr_output_metadata m;\n",
        "    const struct hdr_metadata_infoframe *inf = &m.hdmi_metadata_type1;\n",
        "    (void)sizeof(m); (void)inf;\n",
        "    return 0;\n",
        "}\n",
    );
    if std::fs::write(&test_c, src).is_err() {
        return false;
    }
    let mut cmd = build.get_compiler().to_command();
    cmd.arg("-fsyntax-only");
    for inc in drm_includes {
        cmd.arg("-I").arg(inc);
    }
    cmd.arg(&test_c);
    matches!(cmd.status(), Ok(s) if s.success())
}
