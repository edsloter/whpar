# whpar

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)
[![Platform: Windows | Linux](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-blue.svg)](https://microsoft.com/windows)

**whpar** is a high-speed, linear-time fountain parity CLI utility. It produces and consumes Wirehair-style fountain parity archives (`.whpar`) to protect and repair files against data corruption. 

It is intended as a compact par2-like encoder/repair tool that produces linear-time parity streams and can repair damaged files using an attached parity archive, it leverages advanced fountain codes to achieve near-instantaneous encoding and decoding speeds.

---

## 🚀 Key Features

* **High-Speed Performance:** Uses the optimized Wirehair library for linear-time erasure coding.
* **Self-Describing Packets:** Every parity packet includes precise metadata containing a validation token and target block index.
* **Resilient Architecture:** Mirrored headers — the primary header and hash-index are written at both the start and end of the archive to resist localized index corruption. Both copies are further protected by an XXH3-64 ECC checksum to detect silent corruption.
* **Streamlined CLI:** Clean, distraction-free interface for creation (`-c`), restoration (`-r`), and supplementary add (`-a`).

---

## ⚡ Why whpar over PAR2?

| | whpar | PAR2 (par2j64 / phpar2) |
|---|---|---|
| **Algorithm** | Fountain code (Wirehair) — **O(n)** linear time | Reed-Solomon — **O(n²)** quadratic time |
| **Archive format** | Single/multi `.whpar` files (supplemental chaining) | Split `.par2` + `.volNN+NN.par2` files |
| **Hashing** | xxHash / XXH3\_64bit | CRC32 |
| **Parallel encode** | Yes — multi-track interleaving | No |
| **Multi-file/folder** | Native support for multiple directories and multiple source files in a single archive | depends on client |
| **Header resilience** | Redundant header/footer with XXH3-64 ECC protecting file/folder manifest | Centralized index, single point of failure |
| **Block metadata** | Self-describing per-packet headers with payload hash + expected block hash | Centralized index only |

PAR2's Reed-Solomon implementation must solve a full matrix inversion for every block — time scales quadratically with block count. For large files with thousands of blocks this becomes extremely slow.

whpar uses **fountain codes** (Wirehair): each parity packet is generated independently in constant time, and decoding is a simple linear feed that stops as soon as enough packets arrive. Encoding and repair both scale **linearly** with file size.

The result: whpar creates parity and repairs damaged files **many times faster** than PAR2, especially on large datasets.

### Benchmark — 50 GB file, 10% overhead, 5% corruption

| Measure | whpar (XXH32) | whpar (XXH3-64) | par2j64 | Speedup (XXH3-64 vs par2j64) |
|---|---|---|---|---|
| **Create time** | 151.1 s | 136.6 s | 505.9 s | **3.70×** |
| **Repair time** | 149.5 s | 147.2 s | 554.3 s | **3.77×** |
| **Total time** | 300.5 s | 283.8 s | 1060.2 s | **3.74×** |
| **Parity size** | 5121 MB | 5121 MB | 5120.6 MB | ≈ identical |
| **Verify** | PASS | PASS | PASS | — |

whpar creates parity ~3.5× faster and repairs ~3.7× faster than par2j64. Parity size is essentially the same at the same overhead percentage. XXH3-64 mode adds a small additional speed advantage over the default XXH32 (on very large data sets).

---

## 🛠️ Installation & Building

`whpar` supports **Windows** (native) and **Linux** (via CMake / POSIX portability layer).

### Prerequisites
* **Windows:** Visual Studio or Build Tools for VS (C++ workflow), PowerShell
* **Linux:** GCC or Clang, CMake, make

### Build Steps
Clone the repository and run the provided PowerShell helper from the repository root:

```powershell
# Clone the repository
git clone https://github.com/edsloter/whpar
cd whpar

# Build the executable
./build.ps1
```
The compiled binary will be located at `build\Release\whpar.exe`.

---

## 💻 Usage

### Quick Reference

```bash
whpar -c <source> [<source>...] <overhead>  [options]   # create parity
whpar -r <archive.whpar>                    [options]   # repair (auto-discovers supplements)
whpar -a <archive.whpar> <overhead>                     # add supplementary parity
```

### Create a Parity Archive
Generate a `.whpar` file to protect one or more source files/directories with a specified percentage of overhead.

```bash
whpar -c <source> [<source>...] <overhead> [-o <output>] [options]
```

#### Multiple sources and directory prefixing

When you pass more than one source argument to `-c`, all files from directory sources are prefixed with the directory's basename in the archive manifest. This prevents filename collisions during restore.

- **Single source, single file:** `whpar -c data.iso 0.10` — manifest lists `data.iso`
- **Single source, directory:** `whpar -c docs/ 0.10` — manifest lists `report.txt`, `subdir/file.pdf`
- **Multiple sources:** `whpar -c data.iso docs/ 0.10` — manifest lists `data.iso`, `docs/report.txt`, `docs/subdir/file.pdf`

When restoring a multi-source archive, output must be a directory. Files are placed relative to that directory matching their manifest paths:

```bash
whpar -r archive.p10.whpar -o restored/
# → restored/data.iso
# → restored/docs/report.txt
# → restored/docs/subdir/file.pdf
```

To avoid the prefix, run separate `-c` commands — each source gets its own `.whpar` archive without any prefixing.

#### Naming scheme

Primary archives follow the pattern `<name>.pNN.whpar`, where **N** is the overhead percentage (integer). For example, 10% overhead produces `.p10.whpar`. Supplemental archives use `<name>.pNN+MM.whpar` — the first number is the cumulative base of the source archive, the second is what this supplement adds.

- **No -o flag** — the archive is auto-named after the first source file:
  - `whpar -c movie.mkv 0.10` → creates `movie.p10.whpar`
  - `whpar -c photos.tar.gz 0.05` → creates `photos.tar.p05.whpar`
- **-o flag** — the given name gets `.pN.whpar` appended (strips `.whpar` if present):
  - `whpar -c data.iso 0.10 -o mybackup` → creates `mybackup.p10.whpar`
  - `whpar -c data.iso 0.10 -o backup.whpar` → creates `backup.p10.whpar`
- **Supplementary files** follow the cumulative convention:
  - `whpar -a archive.p10.whpar 0.05` → creates `archive.p10+05.whpar`
  - `whpar -a archive.p10+05.whpar 0.03` → creates `archive.p15+03.whpar`

The **pNN** value is the *cumulative* overhead of the archive being supplemented, and **+MM** is what this supplement adds. Repair auto-discovers all `.whpar` files with a matching base name.

| Option | Description |
|---|---|
| `-b <sizeKB>` | Block size in KB (e.g. `64`, `1M`, `4G`). Auto-selected by default. |
| `-j <numJobs>` | Number of parallel encoding tracks (default: CPU core count). |
| `--xxh64` | Use XXH3\_64bit hashing instead of XXH32 for faster/larger checksums. |
| `--no-recursive` | Only process files in the given directory, not subdirectories. |
| `-f, --force` | Overwrite existing output without prompting. |
| `--debug` | Enable debug output during encoding. |

* **Example (10% overhead):** `whpar -c data.iso 0.10` → `data.p10.whpar`
* **Example (8 parallel tracks, custom output):** `whpar -c data.iso 0.10 -o myarchive -j 8` → `myarchive.p10.whpar`

### Repair a Damaged File
Recover a corrupted file using a parity archive. Repair **auto-discovers all supplemental archives** in the same directory with a matching base name — keep the primary and all supplements together.

```bash
whpar -r <parity.whpar> [-o <outpath>] [-f] [--debug] [--timing]
```

| Option | Description |
|---|---|
| `-o <outpath>` | Destination path. For multi-file archives this must be a directory. For single-file archives, may be a directory or explicit filename. |
| `-f, --force` | Force overwrite / suppress interactive warnings. |
| `--debug` | Enable debug output during repair. |
| `--timing` | Show detailed timing breakdown (hash, decode, inject phases). |

* **Example:** `whpar -r .\project.whpar -o C:\restore\out -f`
* **Example (with timing):** `whpar -r .\project.whpar --timing`

#### When repair fails

If the parity archives don't have enough packets to recover the data, whpar tells you exactly what's missing:

```
FAILURE: Repair incomplete. Have 2341 data blocks, 320 parity packets.
         Need ~51 more packets (≈2.2% additional overhead).
```

Repair auto-discovers all supplemental `.whpar` files in the same directory as the primary archive. If more packets are needed, create a new supplement with the shortfall (or more) using `-a`, then re-run repair with all archives present.

### Add Supplementary Parity

Generate additional parity for an existing archive without re-creating the whole thing. This is useful when:
- A repair attempt failed with "Need ~X more packets" — generate a supplement with that shortfall
- You want to distribute incremental parity later (e.g., user A has the source + original archive, user B needs more recovery packets without re-downloading everything)

```bash
whpar -a <archive.whpar> <overhead>
```

`-a` reads an existing `.whpar`, generates **only the requested overhead** in new parity packets, and writes a new supplemental file. The original archive is never modified.

| Example | What happens |
|---|---|
| `whpar -c data.iso 0.10` | Creates `data.p10.whpar` (primary, 10%) |
| `whpar -a data.p10.whpar 0.05` | Creates `data.p10+05.whpar` (supplement, adds 5%) |
| `whpar -a data.p10+05.whpar 0.03` | Creates `data.p15+03.whpar` (another supplement, adds 3% to cumulative 15% base) |

**Workflow example — two users:**

```bash
# ── User A: creates source + primary archive ──
whpar -c photos.tar.gz 0.10
# → photos.p10.whpar (10% overhead)

# User B downloads source + photos.p10.whpar
# Source gets damaged, repair fails with 10%
whpar -r photos.p10.whpar
# → FAILURE: Need ~200 more packets (≈16% additional overhead)

# User B contacts User A to provide more parity

# ── User A: generates supplement (needs original source) ──
whpar -a photos.p10.whpar 0.20
# → photos.p10+20.whpar (supplement adding 20%)

# User A sends only the .whpar file (much smaller than source)

# ── User B: now has 3 files ──
#   photos.tar.gz (damaged)
#   photos.p10.whpar (primary, 10%)
#   photos.p10+20.whpar (supplement, +20%)

# Repair auto-discovers both archives
whpar -r photos.p10.whpar -o restored/
# → Full recovery from aggregated packets
```

**Key points:**
- The supplement only contains the *additional* parity packets, not a full re-encode
- The original archive and all supplements must be kept together in the same directory — repair auto-discovers them by base name
- Each supplement is a fully self-contained archive with duplicate headers, but generates no redundant parity
- Supplements can be chained indefinitely: `p10` → `p10+05` → `p15+03` → `p18+10` → ...

---

## 🧪 Automated Testing

You can run a full end-to-end test cycle (generate 100MB of random data, apply 10% overhead, corrupt 5MB, and verify successful repair) using the built-in test suite:

```powershell
Set-Location '<repo-root>'  # Navigate to repo root
./run_test.ps1
```

---

## 📄 License

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See the [LICENSE](LICENSE) file for the full text.

Wirehair is used under the BSD 3-Clause license, and xxHash under BSD 2-Clause — see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for full texts.
