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

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdlib>

#define WHPAR_MAGIC 0x32324857
#define WHPAR_PKT_MAGIC 0x4B504B54
#define WHPAR_VERSION "0.0.4"

// On-disk format is little-endian.
// On x86 (LE) these are no-ops; on big-endian they swap via compiler intrinsics.
#ifdef WHPAR_BIG_ENDIAN
  #ifdef _MSC_VER
    inline uint16_t le_to_cpu16(uint16_t v) { return _byteswap_ushort(v); }
    inline uint32_t le_to_cpu32(uint32_t v) { return _byteswap_ulong(v); }
    inline uint64_t le_to_cpu64(uint64_t v) { return _byteswap_uint64(v); }
    inline uint16_t cpu_to_le16(uint16_t v) { return _byteswap_ushort(v); }
    inline uint32_t cpu_to_le32(uint32_t v) { return _byteswap_ulong(v); }
    inline uint64_t cpu_to_le64(uint64_t v) { return _byteswap_uint64(v); }
  #else
    inline uint16_t le_to_cpu16(uint16_t v) { return __builtin_bswap16(v); }
    inline uint32_t le_to_cpu32(uint32_t v) { return __builtin_bswap32(v); }
    inline uint64_t le_to_cpu64(uint64_t v) { return __builtin_bswap64(v); }
    inline uint16_t cpu_to_le16(uint16_t v) { return __builtin_bswap16(v); }
    inline uint32_t cpu_to_le32(uint32_t v) { return __builtin_bswap32(v); }
    inline uint64_t cpu_to_le64(uint64_t v) { return __builtin_bswap64(v); }
  #endif
#else
    inline uint16_t le_to_cpu16(uint16_t v) { return v; }
    inline uint32_t le_to_cpu32(uint32_t v) { return v; }
    inline uint64_t le_to_cpu64(uint64_t v) { return v; }
    inline uint16_t cpu_to_le16(uint16_t v) { return v; }
    inline uint32_t cpu_to_le32(uint32_t v) { return v; }
    inline uint64_t cpu_to_le64(uint64_t v) { return v; }
#endif

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint64_t originalFileSize;
    uint32_t blockSize;
    uint16_t matrixTrack;
    uint32_t fountainId;
    uint64_t payloadHash;
    uint32_t blockSequence;
    uint64_t expectedBlockHash;
};
#pragma pack(pop)

// Endian-aware PacketHeader I/O (on-disk format: little-endian)
static inline void writePacketHeader(std::ofstream& os, const PacketHeader& hdr) {
    PacketHeader le = hdr;
    le.magic = cpu_to_le32(le.magic);
    le.originalFileSize = cpu_to_le64(le.originalFileSize);
    le.blockSize = cpu_to_le32(le.blockSize);
    le.matrixTrack = cpu_to_le16(le.matrixTrack);
    le.fountainId = cpu_to_le32(le.fountainId);
    le.payloadHash = cpu_to_le64(le.payloadHash);
    le.blockSequence = cpu_to_le32(le.blockSequence);
    le.expectedBlockHash = cpu_to_le64(le.expectedBlockHash);
    os.write(reinterpret_cast<const char*>(&le), sizeof(le));
}

static inline void readPacketHeader(std::ifstream& is, PacketHeader& hdr) {
    is.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    hdr.magic = le_to_cpu32(hdr.magic);
    hdr.originalFileSize = le_to_cpu64(hdr.originalFileSize);
    hdr.blockSize = le_to_cpu32(hdr.blockSize);
    hdr.matrixTrack = le_to_cpu16(hdr.matrixTrack);
    hdr.fountainId = le_to_cpu32(hdr.fountainId);
    hdr.payloadHash = le_to_cpu64(hdr.payloadHash);
    hdr.blockSequence = le_to_cpu32(hdr.blockSequence);
    hdr.expectedBlockHash = le_to_cpu64(hdr.expectedBlockHash);
}

// Endian-aware scalar I/O helpers for manifest serialization
static inline void writeU64LE(std::ofstream& os, uint64_t v) { uint64_t le = cpu_to_le64(v); os.write(reinterpret_cast<const char*>(&le), sizeof(le)); }
static inline void writeU32LE(std::ofstream& os, uint32_t v) { uint32_t le = cpu_to_le32(v); os.write(reinterpret_cast<const char*>(&le), sizeof(le)); }
static inline void writeU16LE(std::ofstream& os, uint16_t v) { uint16_t le = cpu_to_le16(v); os.write(reinterpret_cast<const char*>(&le), sizeof(le)); }
static inline void readU64LE(std::ifstream& is, uint64_t& v) { is.read(reinterpret_cast<char*>(&v), sizeof(v)); v = le_to_cpu64(v); }
static inline void readU32LE(std::ifstream& is, uint32_t& v) { is.read(reinterpret_cast<char*>(&v), sizeof(v)); v = le_to_cpu32(v); }
static inline void readU16LE(std::ifstream& is, uint16_t& v) { is.read(reinterpret_cast<char*>(&v), sizeof(v)); v = le_to_cpu16(v); }

struct ManifestEntry {
    std::string relPath;
    uint64_t fileSize;
    uint64_t mtime;
    uint32_t attributes;
};

struct Progress {
    std::atomic<uint64_t> count{0};
    uint64_t total;
    std::string label;
    std::atomic<int> lastPct{-1};

    Progress(uint64_t t, std::string l) : total(t), label(std::move(l)) {
        std::cout << label << " [";
        for (int i = 0; i < 40; ++i) std::cout << " ";
        std::cout << "] 0%" << std::flush;
        lastPct.store(0, std::memory_order_relaxed);
    }

    void display(int pct) {
        std::cout << "\r" << label << " [";
        int bar = 40 * pct / 100;
        for (int i = 0; i < bar; ++i) std::cout << "=";
        if (bar < 40) std::cout << ">";
        for (int i = bar + 1; i < 40; ++i) std::cout << " ";
        std::cout << "] " << pct << "%" << std::flush;
        if (pct == 100) std::cout << "\n";
    }

    void tick() {
        uint64_t c = count.fetch_add(1, std::memory_order_relaxed) + 1;
        int pct = static_cast<int>(c * 100 / total);
        int expected = lastPct.load(std::memory_order_relaxed);
        if (pct > expected && lastPct.compare_exchange_strong(expected, pct, std::memory_order_relaxed)) {
            display(pct);
        }
    }

    void done() {
        int expected = lastPct.load(std::memory_order_relaxed);
        if (expected != 100) {
            lastPct.store(100, std::memory_order_relaxed);
            display(100);
        }
    }
};

inline uint32_t roundUpPow2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

inline double elapsedSecsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
