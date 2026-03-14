# 🤖 AI-Assisted Development

## Philosophy

libdrmtap is built using AI coding agents as first-class development tools. We believe this represents the future of open-source development, and we're transparent about it.

**We don't hide AI usage — we document it, celebrate it, and encourage it.**

## Why AI Agents?

The DRM/KMS screen capture problem requires deep knowledge across multiple domains:

- Linux kernel internals (DRM subsystem, ioctls, DMA-BUF)
- GPU driver specifics (Intel, AMD, Nvidia — each completely different)
- Security models (CAP_SYS_ADMIN, DRM Master, Unix sockets, SCM_RIGHTS)
- Pixel format handling (tiling, deswizzle, HDR tone mapping)
- Cross-referencing dozens of existing projects, issues, and kernel docs

A single developer researching all of this manually would take weeks. With AI agents, the research phase — analyzing 11 projects, reading 25+ GitHub issues, studying 3 reference codebases line by line — was completed in hours.

## Tools Used

### Antigravity (Google DeepMind)
- **Role**: Primary agentic coding assistant
- **Used for**: Codebase analysis, GitHub issue research, architecture design, documentation writing, code generation
- **Why**: Full agent capabilities — can browse GitHub, read source code, analyze issues, and produce structured technical documents

### Gemini (Google DeepMind)
- **Role**: Research and analysis engine
- **Used for**: Web searches, cross-referencing kernel documentation, understanding DRM APIs
- **Why**: Strong reasoning over technical documentation and API specifications

### Claude (Anthropic)
- **Role**: Code generation and technical reasoning
- **Used for**: Code generation, API design review, documentation structure
- **Why**: Excellent at generating clean, well-structured C code and technical writing

## What AI Did

### Research Phase (documented in `docs/research/`)
1. **Landscape analysis** — Found and analyzed 11 DRM/KMS projects on GitHub
2. **RustDesk deep dive** — Traced 4 years of Wayland issues, PRs, and discussions
3. **Source code analysis** — Read `kmsvnc/drm.c` (~800 lines), `kms-screenshot.c` (~1500 lines), FFmpeg `kmsgrab.c` line by line
4. **Issue autopsy** — Read all 25 kmsvnc issues, categorized patterns, extracted gotchas
5. **API design** — Compared 3 implementations, proposed unified API
6. **Architecture** — Designed the helper binary pattern based on gpu-screen-recorder's approach

### Implementation Phase (ongoing)
- Guided by the research findings
- Every architectural decision traceable to a specific finding in the research docs

## What AI Did NOT Do

- **Make final decisions** — A human developer reviewed and approved all designs
- **Test on real hardware** — AI can't access GPU hardware
- **Replace understanding** — The human developer understands every line of the research and code
- **Work unsupervised** — Every step was reviewed, questioned, and refined through human-AI dialogue

## How to Contribute with AI

If you use AI tools (Copilot, ChatGPT, Claude, Gemini, or others) to help with your contributions:

1. ✅ **Go for it** — We encourage AI-assisted contributions
2. ✅ **Understand your code** — Don't submit code you can't explain
3. ✅ **Test it** — AI-generated code needs the same testing as human-written code
4. ✅ **Credit the tool** — Optionally mention in your PR if AI was used (not required)
5. ❌ **Don't auto-generate and submit** — Review, understand, then submit

## The Thesis

> A single developer with AI agents can produce research and code that would traditionally require a team of specialists.

This project is living proof of that thesis. The entire research corpus (7 documents, ~5000 lines of technical analysis) was produced by one human + AI agents in a single session.

We hope this inspires other open-source developers to embrace AI as a tool — not a replacement, but an amplifier of human capability.

---

*"The best tool is the one that makes you more effective without making you less thoughtful."*
