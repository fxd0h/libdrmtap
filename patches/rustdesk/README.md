# RustDesk DRM Backend Patches

These files are the modified/new files needed to add DRM/KMS capture
support to RustDesk v1.4.6 via libdrmtap.

## File mapping

| This file | Goes to in RustDesk |
|---|---|
| `Cargo.toml` | `Cargo.toml` (root) |
| `scrap_Cargo.toml` | `libs/scrap/Cargo.toml` |
| `scrap_build.rs` | `libs/scrap/build.rs` |
| `drm.rs` | `libs/scrap/src/common/drm.rs` (NEW) |
| `linux.rs` | `libs/scrap/src/common/linux.rs` |
| `mod.rs` | `libs/scrap/src/common/mod.rs` |
| `wayland.rs` | `src/server/wayland.rs` |
| `postinst` | `res/DEBIAN/postinst` |

## Additionally needed

Copy `libdrmtap/src/*.c` and `libdrmtap/src/*.h` + `libdrmtap/include/drmtap.h`
into `libs/scrap/src/drmtap/` (the `build.rs` compiles them statically).

## Build

```bash
cargo build --release
# DRM is now a default feature, no --features flag needed
```

## Based on

- RustDesk v1.4.6
- libdrmtap (this repo, main branch)
