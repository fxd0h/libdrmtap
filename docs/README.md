# libdrmtap Documentation

## Structure

```
docs/
├── README.md              ← this file
├── AI_DEVELOPMENT.md      ← AI development philosophy
└── research/              ← technical research
    ├── 00_landscape.md    ← existing projects on GitHub
    ├── 01_wayland_capture_problem.md ← why every project struggles with Wayland
    ├── 02_drm_kms_mechanism.md ← how DRM/KMS capture works
    ├── 03_permissions.md  ← permissions model and helper binary
    ├── 04_gpu_and_testing.md ← GPU differences + vkms testing
    ├── 05_api_and_architecture.md ← API, architecture, concurrency, damage tracking
    ├── 06_github_issues_analysis.md ← real issues/PRs analysis + gotcha checklist
    └── 07_potential_adopters.md ← who would integrate libdrmtap (243K+ ⭐)
```

## Key Topics by Document

| Topic | Document |
|---|---|
| Wayland problem (all projects) | `01_wayland_capture_problem.md` |
| Continuous capture API | `05_api_and_architecture.md` → Continuous Capture section |
| Damage tracking / frame diff | `05_api_and_architecture.md` → Damage Tracking section |
| Thread safety / mutex strategy | `05_api_and_architecture.md` → Thread Safety section |
| Coexistence with NoMachine | `05_api_and_architecture.md` → Coexistence section |
| Cursor capture API | `05_api_and_architecture.md` → Proposed API |
| HDR metadata API | `05_api_and_architecture.md` → Proposed API |
| Security model | `../SECURITY.md` (project root) |
| Gotcha checklist | `06_github_issues_analysis.md` → bottom of file |
| Integration targets | `07_potential_adopters.md` |

## Conventions

- File naming: `NN_topic.md` (numbered for reading order)
- Language: English always
- Each doc includes date and sources
