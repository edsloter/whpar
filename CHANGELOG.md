# Changelog

## 0.0.4 — 2026-05-25

### Added
- 1-track fast paths for `-c` and `-a`: skip interleaving overhead when archive uses a single track.
- Real-time per-track completion messages during repair — each track prints "decoded" as soon as it finishes.
- Incremental verify output in `-i` mode: per-file corruption status printed inline during the analysis loop.
- `-l`/`--list` mode: list archive manifest — file names, sizes, timestamps — without any source I/O.
- `--max-mem` auto-tunes BATCH_SIZE in repair based on `maxMemBytes / perDecoder` instead of hardcoded 2/3.
- mmap-based streaming output in repair (`MapWrite`+`memcpy`) for better write throughput.

### Changed
- `SetFileSize` and `MapWrite` added to `portability.h` for the mmap streaming output path.
- `walkArchivePackets` and `scanSupplementArchives` extracted as template functions, replacing duplicated 80-line lambdas in RepairDataset and InfoCheck.

## 0.0.3 — 2026-05-24

### Added
- `-i`/`--info` mode: inspect archive integrity, hash-check all source files, report corruption, parity stats, and overhead estimate, then optionally run repair.
- `--max-mem` flag for create (`-c`), repair (`-r`), and add (`-a`) commands to cap memory usage via stripe-based processing.
- `-j` flag for repair to control concurrent decoder track count (BATCH_SIZE).
- Streaming output path in repair when `--max-mem` is active: writes individual blocks directly without a full-message buffer.
- RAM warning in create and repair when file exceeds total system RAM and `--max-mem` not set.
- `.whpar` extension on bare command name defaults to info mode (case-insensitive).
- Source file resolution in `-r` and `-i` searches `-o` dir, then archive dir, then current dir.

### Changed
- Non-stripe archives decode track-by-track with streaming output under `--max-mem` (Wirehair cannot decode a subset of blocks independently).
- Repair `-i` reuses `-o` for source lookup and default repair output; no separate prompt for output directory.

### Fixed
- Linux build: added `#include <cstdint>` to `repair.h` for GCC compatibility.
- Linux build: added `-mssse3` to CMakeLists.txt for SSSE3 intrinsics in gf256.

## 0.0.2 — 2026-05-24

### Added
- `-a` mode: generate supplemental parity archives — adds additional overhead without re-encoding existing parity. Output naming: `<base>.p<base>+<add>.whpar`. Supplements chain indefinitely.
- Repair auto-discovers supplemental `.whpar` files in the same directory as the primary archive, aggregating parity packets from all of them.
- Cross-platform support (Windows + Linux) via POSIX portability layer.

### Changed
- 

## 0.0.1 — 2026-05-20
initial version
