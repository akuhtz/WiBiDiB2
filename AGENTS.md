# WiBiDiB2 — BiDiB ↔ WiThrottle Gateway for Raspberry Pi Pico 2W - AI Agent Guidelines

Guidelines for AI agents working on this codebase.

## Project Info

WiBiDiB2 is a model railroad control gateway that bridges **BiDiB** (the model railroad bus protocol) and **WiThrottle** (the WiFi throttle protocol used by apps like Engine Driver). It runs on a **Raspberry Pi Pico 2W** and uses the onboard WiFi to act as an Access Point.

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) v2.2.0
- CMake ≥ 3.13
- ARM GCC toolchain (GCC 14.2 Rel1 or compatible)
- Raspberry Pi Pico VS Code Extension (recommended)

## AI Agent Rules of Engagement

These rules apply to ALL AI agents working on this codebase.

### Attribution

- All AI-generated content (GitHub PR descriptions, review comments, JIRA comments) MUST clearly
  identify itself as AI-generated and mention the human operator.
  Example: "_Claude Code on behalf of [Human Name]_"
- **Never guess or hallucinate the operator's name.** Always determine it programmatically:
  - Use `gh api /user --jq '.login'` to get the authenticated GitHub username.
  - If for any reason the lookup fails, omit the name rather than guessing.
- AI coding agents MUST be configured to add co-authorship trailers to commits
  (e.g., `Co-authored-by`). For Claude Code, enable this via the
  [attribution settings](https://code.claude.com/docs/en/settings#attribution-settings).
