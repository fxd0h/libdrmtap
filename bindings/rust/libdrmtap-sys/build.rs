// build.rs — Find libdrmtap via pkg-config
fn main() {
    pkg_config::Config::new()
        .atleast_version("0.1.0")
        .probe("libdrmtap")
        .expect("libdrmtap not found. Install with: meson install -C build");
}
