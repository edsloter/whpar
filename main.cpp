// whpar - High-Speed Fountain Parity CLI Tool
// Copyright (C) 2026 Edward Sloter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "parity.h"
#include "include/wirehair/wirehair.h"
#include "create.h"
#include "repair.h"

static void printUsage() {
    std::cout << "whpar v" << WHPAR_VERSION << " - High-Speed Fountain Parity CLI Tool\n";
    std::cout << "Copyright (C) 2026 Edward Sloter\n\n";
    std::cout << "Usage:\n";
    std::cout << "  Create:  whpar -c <source> [<source>...] <overhead>\n";
    std::cout << "           [-o <output>] [-b <sizeKB>] [-j <numJobs>] [--xxh64] [--no-recursive] [-f] [--debug]\n";
    std::cout << "  Repair:  whpar -r <archive.whpar> [-o <outdir>] [-j <numJobs>] [--max-mem <size>] [-f] [--debug] [--timing]\n";
    std::cout << "  Add:     whpar -a <archive.whpar> <overhead>        [-j <numJobs>] [--max-mem <size>] [-f] [--debug]\n";
    std::cout << "  Info:    whpar -i <archive.whpar> [-o <dir>]                 [--debug]\n";
    std::cout << "  List:    whpar -l <archive.whpar>                                    \n";
    std::cout << "\n";
    std::cout << "  Default: whpar <archive.whpar>  is equivalent to whpar -i <archive.whpar>\n\n";
    std::cout << "Options:\n";
    std::cout << "  -c <source>        Create parity archive for one or more source files/directories\n";
    std::cout << "                     Output is auto-named after the first source (use -o to override)\n";
    std::cout << "  --no-recursive     Do not recurse into subdirectories (for -c with directory)\n";
    std::cout << "  -r <archive.whpar> Repair a damaged file using a parity archive\n";
    std::cout << "                     Auto-discovers supplemental archives in the same directory\n";
    std::cout << "  -a <archive.whpar> Generate a supplemental parity file (keeps original intact)\n";
    std::cout << "                     Requires original source files; output: <base>.p<old>+<add>.whpar\n";
    std::cout << "  -i <archive.whpar> Inspect a parity archive: check all source files against hashes,\n";
    std::cout << "                     report corruption status, and optionally start repair\n";
    std::cout << "  -l <archive.whpar> List archive manifest: file names, sizes, timestamps (no source I/O)\n";
    std::cout << "  -o <output>        Output path for create or repair\n";
    std::cout << "  -b <sizeKB>        Block size in KB (e.g. 64, 1M, 4G). Default: auto\n";
    std::cout << "  -j <numJobs>       Parallel tracks (create/add) or concurrent decoders (repair). Default: CPU cores\n";
    std::cout << "  -f, --force        Overwrite existing output without prompting\n";
    std::cout << "  --xxh64            Use XXH3_64bit hashing instead of XXH32\n";
    std::cout << "  --max-mem <size>   Limit memory usage (e.g. 512MB, 4GB). Create/add: stripe processing.\n";
    std::cout << "                     Repair: streams output to avoid full output buffer.\n";
    std::cout << "  --debug            Enable debug output\n";
    std::cout << "  --timing           Show detailed timing breakdown after repair\n";
    std::cout << "  --version          Show version and exit\n";
    std::cout << "  -h, --help         Show this help message\n";
    std::cout << "\nExamples:\n";
    std::cout << "  whpar -c movie.mkv 0.10\n";
    std::cout << "  whpar -c data.zip 0.10 -j 8\n";
    std::cout << "  whpar -r archive.whpar -o restored/\n";
    std::cout << "  whpar -a backup.p10.whpar 0.05   # creates backup.p10+05.whpar\n";
    std::cout << "  whpar -r backup.p10.whpar -o ./   # auto-uses backup.p10+05.whpar\n";
    std::cout << "  whpar -l archive.whpar             # list archive manifest (no source I/O)\n";
    std::cout << "  whpar -i archive.whpar             # check source files against archive\n";
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version") {
            std::cout << "whpar version " << WHPAR_VERSION << "\n";
            return 0;
        }
        if (a == "-h" || a == "--help") {
            printUsage();
            return 0;
        }
    }

    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string mode = argv[1];

    // If first arg is a .whpar file, default to info mode
    if (mode.size() > 6) {
        std::string ext = mode.substr(mode.size() - 6);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".whpar") {
            if (wirehair_init() != Wirehair_Success) {
                std::cerr << "CRITICAL: Wirehair initialization failed!\n";
                return 1;
            }
            InfoCheck(mode, false, "");
            return 0;
        }
    }

    if (wirehair_init() != Wirehair_Success) {
        std::cerr << "CRITICAL: Wirehair initialization failed!\n";
        return 1;
    }

    auto parseSizeKB = [](const std::string& s) -> uint32_t {
        std::string v = s;
        // Find where digits end
        size_t pos = 0;
        while (pos < v.size() && (v[pos] == '.' || (v[pos] >= '0' && v[pos] <= '9'))) pos++;
        if (pos == 0) throw std::invalid_argument("no digits");
        double num = std::stod(v.substr(0, pos));
        std::string suf;
        for (size_t i = pos; i < v.size(); i++) suf += static_cast<char>(std::toupper(v[i]));
        if (suf == "G" || suf == "GB" || suf == "GIB") return static_cast<uint32_t>(num * 1024 * 1024);
        if (suf == "M" || suf == "MB" || suf == "MIB") return static_cast<uint32_t>(num * 1024);
        if (suf == "K" || suf == "KB" || suf == "KIB") return static_cast<uint32_t>(num);
        if (suf == "B") return static_cast<uint32_t>(num / 1024.0 + 0.5);
        return static_cast<uint32_t>(num);
    };

    if (mode == "-c") {
        std::vector<std::string> posArgs;
        bool debug = false;
        bool force = false;
        bool useXxh64 = false;
        bool noRecursive = false;
        uint32_t blockSizeKB = 0;
        uint32_t numJobs = 0;
        uint64_t maxMemBytes = 0;
        std::string outArg;

        auto parseMemSize = [](const std::string& s) -> uint64_t {
            std::string v = s;
            size_t pos = 0;
            while (pos < v.size() && (v[pos] == '.' || (v[pos] >= '0' && v[pos] <= '9'))) pos++;
            if (pos == 0) throw std::invalid_argument("no digits");
            double num = std::stod(v.substr(0, pos));
            std::string suf;
            for (size_t i = pos; i < v.size(); i++) suf += static_cast<char>(std::toupper(v[i]));
            if (suf == "G" || suf == "GB" || suf == "GIB") return static_cast<uint64_t>(num * 1024ULL * 1024ULL * 1024ULL);
            if (suf == "M" || suf == "MB" || suf == "MIB") return static_cast<uint64_t>(num * 1024ULL * 1024ULL);
            if (suf == "K" || suf == "KB" || suf == "KIB") return static_cast<uint64_t>(num * 1024ULL);
            if (suf == "B") return static_cast<uint64_t>(num);
            return static_cast<uint64_t>(num);
        };

        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--debug") debug = true;
            else if (a == "-f" || a == "--force") force = true;
            else if (a == "--xxh64") useXxh64 = true;
            else if (a == "--no-recursive") noRecursive = true;
            else if (a == "--max-mem") {
                if (i + 1 < argc) {
                    try {
                        maxMemBytes = parseMemSize(argv[++i]);
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid --max-mem value '" << argv[i] << "' (" << e.what() << ")\n";
                        return 1;
                    }
                    if (maxMemBytes < 256ULL * 1024ULL) {
                        std::cerr << "Error: --max-mem must be at least 256 KB\n";
                        return 1;
                    }
                } else { std::cerr << "Error: --max-mem requires a value\n"; return 1; }
            }
            else if (a == "-b" || a == "--block-size") {
                if (i + 1 < argc) {
                    try {
                        blockSizeKB = parseSizeKB(argv[++i]);
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid block size '" << argv[i] << "' (" << e.what() << ")\n";
                        return 1;
                    }
                } else { std::cerr << "Error: --block-size requires a value\n"; return 1; }
                if (blockSizeKB > 1048576) {
                    std::cerr << "Error: Block size too large (max 1 GB = 1048576 KB)\n";
                    return 1;
                }
            }
            else if (a == "-j" || a == "--jobs") {
                if (i + 1 < argc) {
                    try {
                        numJobs = static_cast<uint32_t>(std::stoul(argv[++i]));
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid job count '" << argv[i] << "' (" << e.what() << ")\n";
                        return 1;
                    }
                } else { std::cerr << "Error: --jobs requires a value\n"; return 1; }
                if (numJobs > 128) {
                    std::cerr << "Error: Job count too large (max 128)\n";
                    return 1;
                }
                if (numJobs == 0) {
                    std::cerr << "Error: Job count must be at least 1\n";
                    return 1;
                }
            }
            else if (a == "-o") {
                if (i + 1 < argc) outArg = argv[++i];
                else { std::cerr << "Error: -o requires a value\n"; return 1; }
            }
            else posArgs.push_back(a);
        }

        if (posArgs.size() < 2) {
            std::cout << "Parity overhead percentage (e.g. 0.10 for 10%): ";
            std::string line;
            std::getline(std::cin, line);
            posArgs.push_back(line);
        }

        std::vector<std::string> sourcePaths(posArgs.begin(), posArgs.end() - 1);
        std::string overheadStr = posArgs.back();

        float overhead = 0;
        try {
            overhead = std::stof(overheadStr);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid overhead value '" << overheadStr << "' (" << e.what() << ")\n";
            return 1;
        }
        if (overhead <= 0.0f || overhead > 1.0f) {
            if (overhead >= 1.0f)
                std::cerr << "Error: Overhead value " << overheadStr << " looks like a percentage (e.g. 10 for 10%).\n"
                          << "       whpar uses decimal fractions: 0.10 = 10%, 0.05 = 5%, etc.\n"
                          << "       Try: whpar -c <source> 0.10\n";
            else
                std::cerr << "Error: Overhead must be greater than 0.\n";
            return 1;
        }

        int pct = static_cast<int>(overhead * 100.0f + 0.5f);
        if (pct < 1) pct = 1;
        if (pct > 99) pct = 99;
        std::string pStr = (pct < 10 ? "0" : "") + std::to_string(pct);
        std::string pSuffix = ".p" + pStr + ".whpar";

        std::string parityPath;
        if (outArg.empty()) {
            size_t lastSep = sourcePaths[0].find_last_of("/\\");
            std::string baseName = (lastSep == std::string::npos) ? sourcePaths[0] : sourcePaths[0].substr(lastSep + 1);
            std::string sourceDir = (lastSep == std::string::npos) ? "" : sourcePaths[0].substr(0, lastSep + 1);
            size_t dot = baseName.find_last_of('.');
            if (dot != std::string::npos)
                baseName = baseName.substr(0, dot);
            parityPath = sourceDir + baseName + pSuffix;
            std::cerr << "Warning: No output specified. Using '" << parityPath << "'.\n";
        } else {
            parityPath = outArg;
            size_t dot = parityPath.find_last_of('.');
            if (dot != std::string::npos) {
                std::string ext = parityPath.substr(dot);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".whpar")
                    parityPath = parityPath.substr(0, dot);
            }
            parityPath += pSuffix;
        }

        if (!force) {
            std::ifstream test(parityPath);
            if (test.good()) {
                test.close();
                std::cout << "Warning: '" << parityPath << "' already exists. Overwrite? (y/N): ";
                std::string answer;
                std::getline(std::cin, answer);
                if (answer != "y" && answer != "Y") {
                    std::cout << "Aborted.\n";
                    return 1;
                }
            }
        }

        if (blockSizeKB > 0 && debug) std::cout << "Using custom block size: " << blockSizeKB << " KB\n";
        if (numJobs > 0) std::cout << "Using " << numJobs << " parallel job(s)\n";
        if (useXxh64) std::cout << "Using XXH3_64bit hashing (--xxh64)\n";
        if (maxMemBytes > 0) {
            double memMB = static_cast<double>(maxMemBytes) / (1024.0 * 1024.0);
            std::cout << "Max memory: " << memMB << " MB";
            if (maxMemBytes >= 1024ULL * 1024ULL * 1024ULL)
                std::cout << " (" << (maxMemBytes / (1024ULL * 1024ULL * 1024ULL)) << " GB)";
            std::cout << "\n";
        }
        CreateParity(sourcePaths, parityPath, overhead, debug, blockSizeKB, useXxh64, numJobs, noRecursive, maxMemBytes);
    }
    else if (mode == "-r") {
        if (argc < 3) {
            std::cerr << "Error: Missing arguments for repair mode.\n";
            std::cerr << "Usage: whpar -r <archive.whpar> [-o <outdir_or_file>] [-f]\n";
            return 1;
        }

        std::vector<std::string> posArgs;
        bool force = false;
        bool debug = false;
        bool showTiming = false;
        uint32_t numJobs = 0;
        uint64_t maxMemBytes = 0;
        std::string outDir;

        auto parseMemSize = [](const std::string& s) -> uint64_t {
            std::string v = s;
            size_t pos = 0;
            while (pos < v.size() && (v[pos] == '.' || (v[pos] >= '0' && v[pos] <= '9'))) pos++;
            if (pos == 0) throw std::invalid_argument("no digits");
            double num = std::stod(v.substr(0, pos));
            std::string suf;
            for (size_t i = pos; i < v.size(); i++) suf += static_cast<char>(std::toupper(v[i]));
            if (suf == "G" || suf == "GB" || suf == "GIB") return static_cast<uint64_t>(num * 1024ULL * 1024ULL * 1024ULL);
            if (suf == "M" || suf == "MB" || suf == "MIB") return static_cast<uint64_t>(num * 1024ULL * 1024ULL);
            if (suf == "K" || suf == "KB" || suf == "KIB") return static_cast<uint64_t>(num * 1024ULL);
            if (suf == "B") return static_cast<uint64_t>(num);
            return static_cast<uint64_t>(num);
        };

        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-o" && i + 1 < argc) { outDir = argv[++i]; }
            else if (a == "-f" || a == "--force") { force = true; }
            else if (a == "--debug") { debug = true; }
            else if (a == "--timing") { showTiming = true; }
            else if (a == "-j" || a == "--jobs") {
                if (i + 1 < argc) {
                    try {
                        numJobs = static_cast<uint32_t>(std::stoul(argv[++i]));
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid job count '" << argv[i] << "' (" << e.what() << ")\n";
                        return 1;
                    }
                } else { std::cerr << "Error: --jobs requires a value\n"; return 1; }
                if (numJobs > 128) { std::cerr << "Error: Job count too large (max 128)\n"; return 1; }
                if (numJobs == 0) { std::cerr << "Error: Job count must be at least 1\n"; return 1; }
            }
            else if (a == "--max-mem") {
                if (i + 1 < argc) {
                    try {
                        maxMemBytes = parseMemSize(argv[++i]);
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid --max-mem value '" << argv[i] << "' (" << e.what() << ")\n";
                        return 1;
                    }
                    if (maxMemBytes < 256ULL * 1024ULL) {
                        std::cerr << "Error: --max-mem must be at least 256 KB\n";
                        return 1;
                    }
                } else { std::cerr << "Error: --max-mem requires a value\n"; return 1; }
            }
            else posArgs.push_back(a);
        }

        if (posArgs.size() != 1) {
            std::cerr << "Error: repair mode now requires exactly one archive path.\n";
            std::cerr << "Usage: whpar -r <archive.whpar> [-o <outdir_or_file>] [-f] [--debug] [--timing]\n";
            return 1;
        }

        std::string archivePath = posArgs[0];
        std::string targetOut = outDir.empty() ? std::string(".") : outDir;
        RepairDataset(std::string(""), archivePath, targetOut, force, debug, showTiming, numJobs, maxMemBytes);
    }
    else if (mode == "-a") {
        std::vector<std::string> posArgs;
        bool debug = false;
        bool force = false;
        uint32_t numJobs = 0;
        uint64_t maxMemBytes = 0;

        auto parseMemSize = [](const std::string& s) -> uint64_t {
            std::string v = s;
            size_t pos = 0;
            while (pos < v.size() && (v[pos] == '.' || (v[pos] >= '0' && v[pos] <= '9'))) pos++;
            if (pos == 0) throw std::invalid_argument("no digits");
            double num = std::stod(v.substr(0, pos));
            std::string suf;
            for (size_t i = pos; i < v.size(); i++) suf += static_cast<char>(std::toupper(v[i]));
            if (suf == "G" || suf == "GB" || suf == "GIB") return static_cast<uint64_t>(num * 1024ULL * 1024ULL * 1024ULL);
            if (suf == "M" || suf == "MB" || suf == "MIB") return static_cast<uint64_t>(num * 1024ULL * 1024ULL);
            if (suf == "K" || suf == "KB" || suf == "KIB") return static_cast<uint64_t>(num * 1024ULL);
            if (suf == "B") return static_cast<uint64_t>(num);
            return static_cast<uint64_t>(num);
        };

        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--debug") debug = true;
            else if (a == "-f" || a == "--force") force = true;
            else if (a == "-j" || a == "--jobs") {
                if (i + 1 < argc) {
                    try {
                        numJobs = static_cast<uint32_t>(std::stoul(argv[++i]));
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid job count '" << argv[i] << "' (" << e.what() << ")\n";
                        return 1;
                    }
                } else { std::cerr << "Error: --jobs requires a value\n"; return 1; }
                if (numJobs > 128) { std::cerr << "Error: Job count too large (max 128)\n"; return 1; }
                if (numJobs == 0) { std::cerr << "Error: Job count must be at least 1\n"; return 1; }
            }
            else if (a == "--max-mem") {
                if (i + 1 < argc) {
                    try {
                        maxMemBytes = parseMemSize(argv[++i]);
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid --max-mem value '" << argv[i] << "' (" << e.what() << ")\n";
                        return 1;
                    }
                    if (maxMemBytes < 256ULL * 1024ULL) {
                        std::cerr << "Error: --max-mem must be at least 256 KB\n";
                        return 1;
                    }
                } else { std::cerr << "Error: --max-mem requires a value\n"; return 1; }
            }
            else if (a == "-o") {
                if (i + 1 < argc) { std::cerr << "Note: -o is not used in add mode; output is auto-named.\n"; ++i; }
                else ++i;
            }
            else posArgs.push_back(a);
        }

        if (posArgs.size() < 2) {
            std::cerr << "Error: Missing arguments for add mode.\n";
            std::cerr << "Usage: whpar -a <archive.whpar> <overhead> [-j <numJobs>] [-f] [--debug]\n";
            std::cerr << "Creates a supplemental archive (keeps original intact).\n";
            return 1;
        }

        if (posArgs.size() > 2) {
            std::cerr << "Error: Add mode takes exactly 2 positional arguments.\n";
            return 1;
        }

        std::string archivePath = posArgs[0];
        float additionalOverhead = 0;
        try {
            additionalOverhead = std::stof(posArgs[1]);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid overhead value '" << posArgs[1] << "' (" << e.what() << ")\n";
            return 1;
        }
        if (additionalOverhead <= 0.0f || additionalOverhead > 1.0f) {
            if (additionalOverhead >= 1.0f)
                std::cerr << "Error: Overhead value " << posArgs[1] << " looks like a percentage.\n"
                          << "       whpar uses decimal fractions: 0.10 = 10%, 0.05 = 5%, etc.\n";
            else
                std::cerr << "Error: Overhead must be greater than 0.\n";
            return 1;
        }

        AddParity(archivePath, additionalOverhead, debug, force, numJobs, false, maxMemBytes);
    }
    else if (mode == "-l" || mode == "--list") {
        std::string archivePath;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (archivePath.empty()) archivePath = a;
            else { std::cerr << "Error: Unexpected argument: " << a << "\n"; return 1; }
        }
        if (archivePath.empty()) {
            std::cerr << "Error: Missing archive path for list mode.\n";
            std::cerr << "Usage: whpar -l <archive.whpar>\n";
            return 1;
        }
        ListManifest(archivePath);
    }
    else if (mode == "-i" || mode == "--info") {
        std::string archivePath;
        std::string sourceDir;
        bool dbg = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--debug") dbg = true;
            else if (a == "-o" && i + 1 < argc) { sourceDir = argv[++i]; }
            else if (archivePath.empty()) archivePath = a;
            else { std::cerr << "Error: Unexpected argument: " << a << "\n"; return 1; }
        }
        if (archivePath.empty()) {
            std::cerr << "Error: Missing archive path for info mode.\n";
            std::cerr << "Usage: whpar -i <archive.whpar> [-o <dir>] [--debug]\n";
            return 1;
        }
        InfoCheck(archivePath, dbg, sourceDir);
    }
    else {
        std::cerr << "Unknown mode: " << mode << "\n";
        return 1;
    }

    return 0;
}
