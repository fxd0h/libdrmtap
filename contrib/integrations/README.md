# Third-Party Integrations

Example integration modules for projects that can use libdrmtap.

These are provided as **reference implementations** — they are not part of the
core library and may need adaptation for the target project's current API.

## Available Integrations

| Project | Directory | Description | Status |
|---|---|---|---|
| [RustDesk](https://github.com/rustdesk/rustdesk) | `rustdesk/` | DRM/KMS capture backend for RustDesk's `scrap` crate (via `libdrmtap-sys`), avoiding the Wayland portal consent dialog | In progress — upstream PR [rustdesk/rustdesk#15420](https://github.com/rustdesk/rustdesk/pull/15420) under maintainer review |

## Adding a New Integration

If you integrate libdrmtap into your project, consider contributing an example
integration module here. See the RustDesk integration for reference.
