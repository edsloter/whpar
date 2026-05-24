# Changelog

## 0.0.2 — 2026-05-24

### Added
- `-a` mode: generate supplemental parity archives — adds additional overhead without re-encoding existing parity. Output naming: `<base>.p<base>+<add>.whpar`. Supplements chain indefinitely.
- Repair auto-discovers supplemental `.whpar` files in the same directory as the primary archive, aggregating parity packets from all of them.
- Cross-platform support (Windows + Linux) via POSIX portability layer.

### Changed
- 

## 0.0.1 — 2026-05-20
initial version
