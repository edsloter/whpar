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
* **Streamlined CLI:** Clean, distraction-free interface built specifically for creation (`-c`) and restoration (`-r`).

---

## ⚡ Why whpar over PAR2?

| | whpar | PAR2 (par2j64 / phpar2) |
|---|---|---|
| **Algorithm** | Fountain code (Wirehair) — **O(n)** linear time | Reed-Solomon — **O(n²)** quadratic time |
| **Archive format** | Single `.whpar` file | Split `.par2` + `.volNN+NN.par2` files |
| **Hashing** | xxHash / XXH3\_64bit | CRC32 |
| **Parallel encode** | Yes — multi-track interleaving | No |
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

whpar creates parity ~3.5× faster and repairs ~3.7× faster than par2j64. Parity size is essentially the same at the same overhead percentage. XXH3-64 mode adds a small additional speed advantage over the default XXH32.

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

### Create a Parity Archive
Generate a `.whpar` file to protect a source file with a specified percentage of overhead.

```bash
whpar -c <source_file> <parity_output.whpar> <overhead> [options]
```

| Option | Description |
|---|---|
| `-b <sizeKB>` | Block size in KB (e.g. `64`, `1M`, `4G`). Auto-selected by default. |
| `-j <numJobs>` | Number of parallel encoding tracks (default: CPU core count). |
| `--xxh64` | Use XXH3\_64bit hashing instead of XXH32 for faster/larger checksums. |
| `--no-recursive` | Only process files in the given directory, not subdirectories. |
| `-f, --force` | Overwrite existing output without prompting. |
| `--debug` | Enable debug output during encoding. |

* **Example (10% overhead):** `whpar -c data.iso data.whpar 0.10`
* **Example (8 parallel tracks, 1 MB blocks):** `whpar -c data.iso data.whpar 0.10 -j 8 -b 1M`

### Repair a Damaged File
Recover a corrupted file using your previously generated parity archive.

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
