# Changelog

All notable changes to Logos Package Downloader are documented here.

## [1.0.0-alpha.1] - 2026-07-24

### Added

- Persistent controls for adding, disabling, removing, and restoring package
  repositories.
- Portable Linux x86_64, Linux ARM64, and Apple Silicon macOS release
  artifacts with native runtime smoke tests and published checksums.

### Fixed

- Resolve repository metadata before fresh-process scoped catalog lookup by
  canonical name or descriptor URL.
- Preserve explicitly pinned top-level dependency versions during transitive
  dependency resolution.
- Rank catalog releases by SemVer precedence instead of publication time.
