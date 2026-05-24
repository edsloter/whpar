# GUI — Brain Dump (May 2026)

## Preference
- Single program that does both CLI and GUI
- Do not want to maintain two separate programs
- GUI should do everything the CLI can do
- Want to keep CLI for terminal users
- Cross-platform
- Qt is too heavy for this small program
- No changes yet — discussion only for now

## Approaches discussed

### 1. Static lib + separate frontends (recommended approach)
Move `create.cpp`, `repair.cpp`, Wirehair into a static library (`whpar_core`). CLI is `main.cpp` linking it; GUI is `guimain.cpp` linking it. CMake `option(WHPAR_GUI ON)` to toggle. Zero code duplication. Same entry points (`CreateParity`, `RepairDataset`) called from both frontends.

### 2. Single binary, dual-mode
One executable. Detect context: if stdin is terminal or no args → GUI; else → CLI. Console subsystem lets you have both, but console flashes on GUI launch. Use `-mwindows` + `AttachConsole` to suppress.

### 3. Framework options (lightweight, cross-platform)

| Framework | Binary overhead | Look & feel | Notes |
|---|---|---|---|
| **wxWidgets** | ~2-5 MB DLLs | Native | Mature, stable, native file dialogs. Static link = single .exe. |
| **FLTK** | ~500 KB static | Old-school flat | Tiny, compiles seconds. Ugly default but themable. |
| **GTKmm** | ~10-20 MB DLLs | Nativeish | Packaging on Windows is painful. |
| **Embedded web server** | ~100 KB extra | Browser tab | `cpp-httplib` or `libmicrohttpd`. UI in HTML/CSS/JS. Zero C++ GUI dep. Browser required (everyone has one). No native file dialogs unless File System Access API. |
| **Python-shelled** | ~3 MB stub | Tkinter/PySide | Core as static lib, GUI in Python via ctypes/pybind11. Tkinter is zero-dep with Python. |

### Key considerations
- Keep `main.cpp` subsystem-agnostic — no GUI includes in core files
- GUI thread must call `CreateParity` / `RepairDataset` on a background thread
- Web server approach: binary stays tiny, UI can look modern, updates are just HTML
- wxWidgets: best middle ground if native window feel is desired without Qt weight
