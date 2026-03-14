# Contributing to libdrmtap

First off — **thank you** for considering contributing! Every contribution helps solve a real problem: reliable screen capture on Linux.

## How to Contribute

### 🐛 Reporting Bugs

If you find a bug, please open a GitHub issue with:

1. **GPU and driver** (`lspci -v | grep -A5 VGA` + `cat /sys/class/drm/card0/device/driver/module/name`)
2. **Linux kernel version** (`uname -r`)
3. **Desktop environment and compositor** (GNOME/KDE/Sway, Wayland/X11)
4. **Full debug output** from libdrmtap
5. **What you expected** vs **what happened**

### 🔧 Contributing Code

1. **Fork** the repository
2. **Create a branch** (`git checkout -b feature/my-feature`)
3. **Make your changes** — follow the code style (see below)
4. **Test** if possible (see Testing section)
5. **Commit** with clear messages
6. **Open a Pull Request** with a description of what and why

### 📖 Contributing Documentation

Documentation contributions are extremely valuable! This includes:
- Fixing typos or unclear explanations
- Adding examples
- Documenting GPU-specific behaviors you've discovered
- Translating documentation

### 🧪 Testing on Real Hardware

We especially need people to test on real hardware and report results. If you have access to any of these, your testing is invaluable:

| Hardware | Priority |
|---|---|
| Intel iGPU (any generation) | 🔴 High |
| AMD discrete GPU | 🔴 High |
| Nvidia GPU (open or proprietary driver) | 🟡 Medium |
| Raspberry Pi 4/5 | 🟡 Medium |
| VM with virtio-gpu | 🟢 Low (easy to set up) |
| ARM SoCs (iMX8, Jetson, etc.) | 🟢 Low |

## Code Style

- **Language**: C11
- **Indent**: 4 spaces, no tabs
- **Naming**: `snake_case` for functions and variables, `UPPER_CASE` for macros
- **Public API**: prefixed with `drmtap_`
- **Comments**: English, explain *why* not *what*
- **Line length**: 100 characters soft limit
- **Headers**: include guards with `#ifndef DRMTAP_*_H`

## Project Structure

```
libdrmtap/
├── include/          # Public API headers
├── src/              # Implementation
├── helper/           # Privileged helper binary
├── tests/            # Test suite
├── docs/             # Documentation
│   └── research/     # Technical research
├── examples/         # Usage examples
├── meson.build       # Build system
├── LICENSE           # MIT
└── README.md
```

## Building

```bash
meson setup build
meson compile -C build
```

## Testing

### Without hardware (CI)
```bash
sudo modprobe vkms
DRM_DEVICE=/dev/dri/card1 meson test -C build
```

### With real GPU
```bash
meson test -C build
```

## AI-Assisted Development

This project uses AI agents as development tools. If you use AI to help with your contributions, that's great — just make sure:

1. You **understand** the code you're submitting
2. You've **tested** it (or clearly stated that you couldn't)
3. The code is **correct** and follows our style guidelines

We don't discriminate between human-written and AI-assisted code. What matters is quality.

## Code of Conduct

Be respectful, constructive, and inclusive. We're all here to solve the same problem.

- No harassment, discrimination, or personal attacks
- Assume good intent
- Focus on the technical merits of contributions
- Welcome newcomers — everyone started somewhere

## License

By contributing to libdrmtap, you agree that your contributions will be licensed under the [MIT License](LICENSE).

---

Questions? Open an issue or start a discussion. We're happy to help you get started!
