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

#include "repair.h"
#include "parity.h"
#include "wirehair.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <iostream>
#include <vector>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include <future>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <mutex>
#include <filesystem>
#include "portability.h"

// ── Shared helpers ──────────────────────────────

struct ArchiveMapping {
    os::Mapping mapping;
    std::string path;
};

static void showElapsed(std::chrono::steady_clock::time_point start) {
    double e = elapsedSecsSince(start);
    if (e >= 60.0) {
        int m = static_cast<int>(e / 60);
        std::cout << " (" << m << "m " << (e - m * 60) << "s)";
    } else {
        std::cout << " (" << e << "s)";
    }
}

struct ParsedArchive {
    bool valid = false;
    PacketHeader globalMeta;
    std::vector<uint64_t> referenceHashes;
    std::vector<ManifestEntry> canonicalManifest;
    bool useXxh64 = false;
    uint64_t totalBlocks = 0;
    uint16_t totalTracks = 0;
    uint64_t stripeCount = 1;
    uint64_t blocksPerStripe = 0;
    std::string archiveDir;
    std::function<uint64_t(const uint8_t*, size_t)> hashBlock;
    std::vector<uint64_t> trackSizes;
};

static ParsedArchive parseArchive(const std::string& parityPath) {
    ParsedArchive result;

    std::ifstream parityIn(parityPath, std::ios::binary);
    if (!parityIn) {
        std::cerr << "Error: Cannot open parity file: " << parityPath << "\n";
        return result;
    }

    parityIn.seekg(0, std::ios::end);
    std::streampos fileSize = parityIn.tellg();
    if (fileSize < static_cast<std::streampos>(sizeof(uint64_t))) {
        std::cerr << "Error: Archive too small.\n";
        return result;
    }
    parityIn.seekg(-static_cast<std::streamoff>(sizeof(uint64_t)), std::ios::end);
    uint64_t headerBlockSize = 0;
    readU64LE(parityIn, headerBlockSize);

    if (headerBlockSize < sizeof(PacketHeader) || headerBlockSize > static_cast<uint64_t>(fileSize) - sizeof(uint64_t)) {
        std::cerr << "Error: Invalid header size in archive footer.\n";
        return result;
    }
    std::streampos headerEndOffset = fileSize - static_cast<std::streampos>(sizeof(uint64_t)) - static_cast<std::streampos>(headerBlockSize);
    if (headerEndOffset <= 0 || headerEndOffset >= fileSize - static_cast<std::streampos>(sizeof(uint64_t))) {
        std::cerr << "Error: Corrupt archive header offset.\n";
        return result;
    }

    auto readHeaderBlock = [&](std::streampos pos, PacketHeader& hdr, std::vector<uint64_t>& hashes, std::vector<ManifestEntry>& manifest) -> bool {
        parityIn.seekg(pos);
        readPacketHeader(parityIn, hdr);
        if (!parityIn || hdr.magic != WHPAR_MAGIC) return false;
        uint32_t hashCount = 0;
        readU32LE(parityIn, hashCount);
        if (hashCount > (static_cast<uint64_t>(fileSize) - static_cast<uint64_t>(static_cast<std::streamoff>(pos))
            - sizeof(PacketHeader) - sizeof(uint32_t) - 1) / sizeof(uint64_t)) return false;
        hashes.resize(hashCount);
        for (uint32_t i = 0; i < hashCount; ++i) readU64LE(parityIn, hashes[i]);
        uint8_t hasManifest = 0;
        parityIn.read(reinterpret_cast<char*>(&hasManifest), sizeof(hasManifest));
        if (hasManifest) {
            uint32_t fileCount = 0;
            readU32LE(parityIn, fileCount);
            for (uint32_t i = 0; i < fileCount; ++i) {
                uint32_t pathLen = 0;
                readU32LE(parityIn, pathLen);
                std::string path; path.resize(pathLen);
                parityIn.read(&path[0], pathLen);
                ManifestEntry me;
                me.relPath = path;
                readU64LE(parityIn, me.fileSize);
                readU64LE(parityIn, me.mtime);
                readU32LE(parityIn, me.attributes);
                manifest.push_back(me);
            }
        }
        return true;
    };

    auto verifyHeaderEcc = [&](std::streampos pos, uint64_t totalSize) -> bool {
        if (totalSize <= 8) return false;
        uint64_t bodySize = totalSize - 8;
        std::vector<uint8_t> body(static_cast<size_t>(bodySize));
        parityIn.seekg(pos);
        parityIn.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(bodySize));
        if (!parityIn) return false;
        uint64_t stored = 0;
        readU64LE(parityIn, stored);
        uint64_t computed = XXH3_64bits(body.data(), bodySize);
        return computed == stored;
    };

    PacketHeader headerA, headerEnd;
    std::vector<uint64_t> hashesA, hashesEnd;
    std::vector<ManifestEntry> manifestA, manifestEnd;

    bool mainOk = readHeaderBlock(0, headerA, hashesA, manifestA);
    if (mainOk) mainOk = verifyHeaderEcc(0, headerBlockSize);
    bool mirrorOk = (headerEndOffset > 0 && headerEndOffset < fileSize - static_cast<std::streampos>(sizeof(uint64_t)))
        ? readHeaderBlock(headerEndOffset, headerEnd, hashesEnd, manifestEnd)
        : false;
    if (mirrorOk) mirrorOk = verifyHeaderEcc(headerEndOffset, headerBlockSize);

    if (mainOk && mirrorOk) {
        if (hashesA == hashesEnd) {
            result.globalMeta = headerEnd;
            result.referenceHashes = std::move(hashesEnd);
            result.canonicalManifest = !manifestEnd.empty() ? std::move(manifestEnd) : std::move(manifestA);
        } else {
            std::cerr << "Warning: mirrored header mismatch; using primary.\n";
            result.globalMeta = headerA;
            result.referenceHashes = std::move(hashesA);
            result.canonicalManifest = std::move(manifestA);
        }
    } else if (mirrorOk) {
        result.globalMeta = headerEnd;
        result.referenceHashes = std::move(hashesEnd);
        result.canonicalManifest = std::move(manifestEnd);
        std::cerr << "Warning: primary header corrupt; recovered from mirrored header.\n";
    } else if (mainOk) {
        result.globalMeta = headerA;
        result.referenceHashes = std::move(hashesA);
        result.canonicalManifest = std::move(manifestA);
        std::cerr << "Warning: mirrored header missing or corrupt; using primary.\n";
    } else {
        std::cerr << "Error: Invalid or corrupt .whpar archive (both headers unusable)\n";
        return result;
    }

    result.useXxh64 = (result.globalMeta.matrixTrack == 1);
    result.hashBlock = [useXxh64 = result.useXxh64](const uint8_t* data, size_t len) -> uint64_t {
        return useXxh64 ? XXH3_64bits(data, len) : XXH32(data, len, 0);
    };

    std::cout << "Valid archive metadata found!\n";
    std::cout << "Target File Size: " << (result.globalMeta.originalFileSize / (1024 * 1024.0)) << " MiB\n";
    std::cout << "Expected Blocks: " << result.referenceHashes.size() << " (" << (result.globalMeta.blockSize / 1024) << " KB each)\n";

    result.totalBlocks = (result.globalMeta.originalFileSize + result.globalMeta.blockSize - 1) / result.globalMeta.blockSize;
    result.totalTracks = static_cast<uint16_t>(result.globalMeta.fountainId);
    if (result.totalTracks == 0) result.totalTracks = 1;

    std::cout << "Interleaving Architecture: " << result.totalTracks << " parallel matrix track(s).\n";

    result.stripeCount = result.globalMeta.blockSequence;
    result.blocksPerStripe = result.globalMeta.expectedBlockHash;
    if (result.stripeCount == 0) result.stripeCount = 1;
    if (result.blocksPerStripe == 0) result.blocksPerStripe = result.totalBlocks;
    if (result.stripeCount > 1)
        std::cout << "Stripe count: " << result.stripeCount << " (" << result.blocksPerStripe << " blocks/stripe)\n";

    result.trackSizes.resize(result.totalTracks, 0);
    for (uint64_t i = 0; i < result.totalBlocks; ++i) {
        uint16_t trackId = i % result.totalTracks;
        uint64_t offset = i * result.globalMeta.blockSize;
        size_t currentBlockSize = (offset + result.globalMeta.blockSize <= result.globalMeta.originalFileSize) ? result.globalMeta.blockSize : (result.globalMeta.originalFileSize - offset);
        result.trackSizes[trackId] += currentBlockSize;
    }

    result.archiveDir = std::filesystem::path(parityPath).parent_path().u8string();
    if (result.archiveDir.empty()) result.archiveDir = ".";

    result.valid = true;
    return result;
}

static std::string resolveSourceFile(const ManifestEntry& entry, const std::string& searchDir, const std::string& archiveDir) {
    auto rel = std::filesystem::path(entry.relPath);
    std::filesystem::path cand1 = std::filesystem::path(searchDir) / rel;
    if (std::filesystem::exists(cand1)) return cand1.string();
    std::filesystem::path cand2 = std::filesystem::path(archiveDir) / rel;
    if (std::filesystem::exists(cand2)) return cand2.string();
    std::filesystem::path cand3 = std::filesystem::current_path() / rel;
    if (std::filesystem::exists(cand3)) return cand3.string();
    return "";
}

static std::vector<std::string> resolveAllSources(const std::vector<ManifestEntry>& manifest, const std::string& searchDir, const std::string& archiveDir) {
    std::vector<std::string> paths;
    for (auto& me : manifest) {
        paths.push_back(resolveSourceFile(me, searchDir, archiveDir));
    }
    return paths;
}

template<typename OnPacket>
static bool walkArchivePackets(const std::string& archPath, const ParsedArchive& archive, bool isPrimary,
                               std::vector<ArchiveMapping>& archiveMappings, size_t& parityPacketsSeen,
                               OnPacket&& onPacket) {
    os::Mapping mapping;
    if (!os::MapRead(archPath.c_str(), mapping)) {
        if (isPrimary) std::cerr << "Error: Cannot memory-map parity file: " << archPath << "\n";
        else std::cerr << "Warning: Cannot memory-map supplement: " << archPath << "\n";
        return false;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapping.data);
    uint64_t fileSz = mapping.size;

    uint64_t hdrBlockSz = 0;
    memcpy(&hdrBlockSz, base + fileSz - sizeof(uint64_t), sizeof(uint64_t));
    hdrBlockSz = le_to_cpu64(hdrBlockSz);
    if (hdrBlockSz < sizeof(PacketHeader)) {
        if (!isPrimary) std::cerr << "Warning: Invalid supplement header size: " << archPath << "\n";
        os::Unmap(mapping);
        return false;
    }
    uint64_t dataStart = hdrBlockSz;
    uint64_t mirrorOffset = fileSz - sizeof(uint64_t) - hdrBlockSz;
    if (dataStart >= mirrorOffset) {
        if (!isPrimary) std::cerr << "Warning: Corrupt supplement: " << archPath << "\n";
        os::Unmap(mapping);
        return false;
    }
    uint64_t dataSize = mirrorOffset - dataStart;
    const uint8_t* dataPtr = base + dataStart;
    size_t dataSz = static_cast<size_t>(dataSize);

    size_t bufOff = 0;
    while (bufOff + sizeof(PacketHeader) <= dataSz) {
        PacketHeader pkt;
        memcpy(&pkt, dataPtr + bufOff, sizeof(pkt));
        pkt.magic = le_to_cpu32(pkt.magic);
        pkt.originalFileSize = le_to_cpu64(pkt.originalFileSize);
        pkt.blockSize = le_to_cpu32(pkt.blockSize);
        pkt.matrixTrack = le_to_cpu16(pkt.matrixTrack);
        pkt.fountainId = le_to_cpu32(pkt.fountainId);
        pkt.payloadHash = le_to_cpu64(pkt.payloadHash);
        pkt.blockSequence = le_to_cpu32(pkt.blockSequence);
        pkt.expectedBlockHash = le_to_cpu64(pkt.expectedBlockHash);
        bufOff += sizeof(PacketHeader);

        if (pkt.magic == WHPAR_MAGIC) {
            if (bufOff + sizeof(uint32_t) > dataSz) break;
            uint32_t hashCount = le_to_cpu32(*reinterpret_cast<const uint32_t*>(dataPtr + bufOff));
            bufOff += sizeof(uint32_t) + static_cast<size_t>(hashCount) * sizeof(uint64_t);
            if (bufOff >= dataSz) break;
            uint8_t hm = dataPtr[bufOff]; bufOff++;
            if (hm) {
                if (bufOff + sizeof(uint32_t) > dataSz) break;
                uint32_t fc = le_to_cpu32(*reinterpret_cast<const uint32_t*>(dataPtr + bufOff));
                bufOff += sizeof(uint32_t);
                for (uint32_t i = 0; i < fc; ++i) {
                    if (bufOff + sizeof(uint32_t) > dataSz) break;
                    uint32_t plen = le_to_cpu32(*reinterpret_cast<const uint32_t*>(dataPtr + bufOff));
                    bufOff += sizeof(uint32_t) + plen + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t);
                }
            }
            continue;
        }

        if (pkt.magic != WHPAR_PKT_MAGIC) {
            bufOff += (pkt.blockSize > 0) ? static_cast<size_t>(pkt.blockSize) : 0;
            parityPacketsSeen++;
            continue;
        }

        if (pkt.matrixTrack >= archive.totalTracks || pkt.blockSize > 2 * 1024 * 1024) {
            bufOff += static_cast<size_t>(pkt.blockSize);
            continue;
        }

        if (bufOff + pkt.blockSize > dataSz) break;

        uint32_t stripeIndex = (archive.stripeCount > 1) ? pkt.blockSequence : 0;
        onPacket(pkt.matrixTrack, dataPtr + bufOff, pkt.blockSize, pkt.fountainId, pkt.blockSequence, pkt.payloadHash, pkt.expectedBlockHash, stripeIndex);
        bufOff += pkt.blockSize;
        parityPacketsSeen++;
    }

    archiveMappings.push_back({std::move(mapping), archPath});
    return true;
}

template<typename OnPacket>
static void scanSupplementArchives(const std::string& parityPath, const ParsedArchive& archive,
                                   std::vector<ArchiveMapping>& archiveMappings, size_t& parityPacketsSeen,
                                   OnPacket&& onPacket) {
    auto archDir = std::filesystem::path(parityPath).parent_path();
    if (archDir.empty()) archDir = ".";
    std::string primaryFn = std::filesystem::path(parityPath).filename().string();
    std::string baseName = primaryFn;
    {
        size_t extDot = baseName.rfind('.');
        if (extDot != std::string::npos) baseName = baseName.substr(0, extDot);
        size_t plusPos = baseName.find('+');
        if (plusPos != std::string::npos) baseName = baseName.substr(0, plusPos);
        size_t ppos = baseName.rfind(".p");
        if (ppos != std::string::npos && ppos > 0) baseName = baseName.substr(0, ppos);
    }
    if (std::filesystem::is_directory(archDir)) {
        for (auto& entry : std::filesystem::directory_iterator(archDir)) {
            if (!std::filesystem::is_regular_file(entry.path())) continue;
            std::string name = entry.path().filename().string();
            if (name.size() < 7) continue;
            std::string ext = name.substr(name.size() - 6);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".whpar") continue;
            if (name.find(baseName) != 0) continue;
            std::string absEntry = std::filesystem::absolute(entry.path()).u8string();
            std::string absPrimary = std::filesystem::absolute(parityPath).u8string();
            if (absEntry == absPrimary) continue;
            size_t plusPos = name.find('+');
            if (plusPos == std::string::npos) continue;
            if (plusPos >= name.size() - 1) continue;
            std::cout << "Found supplemental archive: " << name << "\n";
            if (!walkArchivePackets(entry.path().u8string(), archive, false, archiveMappings, parityPacketsSeen, onPacket))
                std::cout << "  Warning: Failed to parse supplement '" << name << "' - skipping.\n";
        }
    }
}

struct FileOffsets {
    std::vector<uint64_t> fileStartOffsets;
    std::vector<uint64_t> fileEndOffsets;
};

static FileOffsets computeFileOffsets(const std::vector<ManifestEntry>& manifest) {
    FileOffsets fo;
    uint64_t off = 0;
    for (auto& me : manifest) {
        fo.fileStartOffsets.push_back(off);
        off += me.fileSize;
        fo.fileEndOffsets.push_back(off);
    }
    return fo;
}

// ── RepairDataset ───────────────────────────────

void RepairDataset(const std::string& damagedPath, const std::string& parityPath, const std::string& outputPath, bool force, bool debug, bool showTiming, uint32_t numJobs, uint64_t maxMemBytes) {
    auto startTime = std::chrono::steady_clock::now();

    ParsedArchive archive = parseArchive(parityPath);
    if (!archive.valid) {
        showElapsed(startTime); std::cerr << "\n";
        return;
    }

    {
        uint64_t sysMemMB = os::TotalMemoryMB();
        if (sysMemMB > 0 && maxMemBytes == 0 && archive.globalMeta.originalFileSize > sysMemMB * 1024ULL * 1024ULL) {
            std::cout << "Warning: File size (" << (archive.globalMeta.originalFileSize / (1024ULL * 1024 * 1024))
                      << " GB) exceeds total system RAM (" << sysMemMB / 1024
                      << " GB) and --max-mem is not set.\n"
                      << "         Repair will need to hold the full recovered data in memory.\n"
                      << "         Use --max-mem <size> to stream output and -j 1 to limit concurrency.\n";
        }
    }

    std::string resolvedDamagedPath = damagedPath;
    if (resolvedDamagedPath.empty() && archive.canonicalManifest.size() == 1) {
        auto found = resolveSourceFile(archive.canonicalManifest[0], outputPath, archive.archiveDir);
        if (!found.empty()) {
            resolvedDamagedPath = found;
            std::cout << "Found file matching manifest: " << resolvedDamagedPath << "\n";
        }
    }

    FileOffsets fo = computeFileOffsets(archive.canonicalManifest);

    struct HealthyBlockRef {
        uint32_t internalTrackBlockId;
        uint64_t fileOffset;
        uint32_t blockSize;
        uint16_t fileIndex;
        uint64_t fileLocalOffset;
    };

    auto readBlockFromFiles = [&](uint64_t offset, uint32_t size, std::vector<uint8_t>& buf, int& cachedFileIdx, os::FileHandle& cachedHandle) -> bool {
        int fileIdx = 0;
        for (size_t f = 0; f < fo.fileStartOffsets.size(); ++f) {
            if (offset >= fo.fileStartOffsets[f] && offset < fo.fileEndOffsets[f]) { fileIdx = static_cast<int>(f); break; }
        }
        if (fileIdx != cachedFileIdx) {
            if (cachedHandle != os::InvalidHandle()) os::Close(cachedHandle);
            std::string fpath = resolveSourceFile(archive.canonicalManifest[fileIdx], outputPath, archive.archiveDir);
            cachedHandle = fpath.empty() ? os::InvalidHandle() : os::OpenRead(fpath.c_str(), false);
            cachedFileIdx = fileIdx;
        }
        std::fill(buf.begin(), buf.end(), 0);
        if (cachedHandle == os::InvalidHandle()) return false;
        uint64_t localOff = offset - fo.fileStartOffsets[fileIdx];
        uint64_t avail = fo.fileEndOffsets[fileIdx] - offset;
        size_t firstPart = static_cast<size_t>(std::min<uint64_t>(avail, size));
        os::Seek(cachedHandle, static_cast<int64_t>(localOff), 0);
        uint32_t br = 0;
        os::Read(cachedHandle, buf.data(), static_cast<uint32_t>(firstPart), br);
        if (firstPart < size) {
            size_t secondPart = size - firstPart;
            int fileIdx2 = fileIdx + 1;
            if (fileIdx2 < static_cast<int>(archive.canonicalManifest.size())) {
                std::string fpath2 = resolveSourceFile(archive.canonicalManifest[fileIdx2], outputPath, archive.archiveDir);
                os::FileHandle h2 = fpath2.empty() ? os::InvalidHandle() : os::OpenRead(fpath2.c_str(), false);
                if (h2 != os::InvalidHandle()) {
                    uint32_t br2 = 0;
                    os::Read(h2, buf.data() + firstPart, static_cast<uint32_t>(secondPart), br2);
                    os::Close(h2);
                }
            }
        }
        return true;
    };

    uint64_t corruptedBlocksCount = 0;
    std::vector<uint64_t> damagedBlockHashes;
    std::vector<uint8_t> blockCorrupted;
    std::vector<std::vector<HealthyBlockRef>> trackHealthyBlocks(archive.totalTracks);

    double phase1HashTime = 0, injectHashTime = 0, decodeFeedTime = 0;
    uint64_t decodeFeedCalls = 0;

    const uint8_t* mappedData = nullptr;
    os::Mapping mappingFeed;
    bool hasMapping = false;
    std::ifstream fallbackFile;
    std::mutex fallbackMutex;

    if (!resolvedDamagedPath.empty()) {
        Progress prog(archive.totalBlocks, "Analysis");

        bool mappingUsed = false;
        os::Mapping mapping;
        const uint8_t* fileData = nullptr;

        if (os::MapRead(resolvedDamagedPath.c_str(), mapping)) {
            fileData = static_cast<const uint8_t*>(mapping.data);
            if (fileData) {
                mappingUsed = true;
                damagedBlockHashes.resize(archive.totalBlocks);
                blockCorrupted.assign(archive.totalBlocks, 0);

                auto tHashPhase1 = std::chrono::steady_clock::now();
                {
                    unsigned int numThreads = std::thread::hardware_concurrency();
                    if (numThreads == 0) numThreads = 4;
                    uint64_t chunkSize = (archive.totalBlocks + numThreads - 1) / numThreads;
                    std::vector<std::future<void>> hashFutures;
                    for (uint64_t b = 0; b < archive.totalBlocks; b += chunkSize) {
                        uint64_t end = (b + chunkSize > archive.totalBlocks) ? archive.totalBlocks : b + chunkSize;
                        hashFutures.push_back(std::async(std::launch::async, [&, b, end]() {
                            for (uint64_t i = b; i < end; ++i) {
                                uint64_t offset = i * archive.globalMeta.blockSize;
                                size_t currentBlockSize = (offset + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                                    ? archive.globalMeta.blockSize
                                    : (archive.globalMeta.originalFileSize - offset);
                                damagedBlockHashes[i] = archive.hashBlock(fileData + offset, currentBlockSize);
                                prog.tick();
                            }
                        }));
                    }
                    for (auto& f : hashFutures) f.get();
                }
                phase1HashTime = elapsedSecsSince(tHashPhase1);

                for (uint64_t i = 0; i < archive.totalBlocks; ++i) {
                    uint16_t trackId = i % archive.totalTracks;
                    uint64_t offset = i * archive.globalMeta.blockSize;
                    size_t currentBlockSize = (offset + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                        ? archive.globalMeta.blockSize : (archive.globalMeta.originalFileSize - offset);
                    uint64_t expectedHash = (i < archive.referenceHashes.size()) ? archive.referenceHashes[i] : 0;
                    if (damagedBlockHashes[i] == expectedHash) {
                        trackHealthyBlocks[trackId].push_back({static_cast<uint32_t>(i / archive.totalTracks), offset, static_cast<uint32_t>(currentBlockSize), 0, offset});
                    } else {
                        corruptedBlocksCount++;
                        blockCorrupted[i] = 1;
                    }
                }

                os::Unmap(mapping);
            }
        }

        if (!mappingUsed) {
            std::ifstream damagedFile(resolvedDamagedPath, std::ios::binary);
            if (damagedFile.is_open()) {
                std::vector<uint8_t> blockBuffer(archive.globalMeta.blockSize);
                blockCorrupted.assign(archive.totalBlocks, 0);
                for (uint64_t i = 0; i < archive.totalBlocks; ++i) {
                    damagedFile.read(reinterpret_cast<char*>(blockBuffer.data()), archive.globalMeta.blockSize);
                    size_t bytesRead = damagedFile.gcount();
                    uint64_t calculatedHash = archive.hashBlock(blockBuffer.data(), bytesRead);
                    prog.tick();
                    uint16_t trackId = i % archive.totalTracks;
                    uint64_t expectedHash = (i < archive.referenceHashes.size()) ? archive.referenceHashes[i] : 0;
                    if (calculatedHash == expectedHash) {
                        trackHealthyBlocks[trackId].push_back({static_cast<uint32_t>(i / archive.totalTracks), i * archive.globalMeta.blockSize, static_cast<uint32_t>(bytesRead), 0, i * archive.globalMeta.blockSize});
                    } else {
                        corruptedBlocksCount++;
                        blockCorrupted[i] = 1;
                    }
                }
                damagedFile.close();
            }
        }

        prog.done();
        std::cout << "Found and isolated " << corruptedBlocksCount << " corrupted block(s).\n";
    } else if (!archive.canonicalManifest.empty()) {
        blockCorrupted.assign(archive.totalBlocks, 0);
        std::cout << "Analyzing local files for healthy blocks...\n";
        corruptedBlocksCount = 0;

        Progress prog(archive.totalBlocks, "Analysis");
        auto tHashPhase1 = std::chrono::steady_clock::now();
        std::vector<uint8_t> composeBuf(archive.globalMeta.blockSize);
        int curFileIdx = -1;
        os::FileHandle hCurFile = os::InvalidHandle();

        for (uint64_t i = 0; i < archive.totalBlocks; ++i) {
            uint64_t offset = i * archive.globalMeta.blockSize;
            size_t currentBlockSize = (offset + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                ? archive.globalMeta.blockSize : (archive.globalMeta.originalFileSize - offset);

            readBlockFromFiles(offset, static_cast<uint32_t>(currentBlockSize), composeBuf, curFileIdx, hCurFile);

            uint64_t h = archive.hashBlock(composeBuf.data(), currentBlockSize);
            prog.tick();
            uint16_t trackId = i % archive.totalTracks;
            uint64_t expectedHash = (i < archive.referenceHashes.size()) ? archive.referenceHashes[i] : 0;
            if (h == expectedHash) {
                trackHealthyBlocks[trackId].push_back({static_cast<uint32_t>(i / archive.totalTracks), offset, static_cast<uint32_t>(currentBlockSize), 0, offset});
            } else {
                corruptedBlocksCount++;
                blockCorrupted[i] = 1;
            }
        }

        if (hCurFile != os::InvalidHandle()) os::Close(hCurFile);
        phase1HashTime = elapsedSecsSince(tHashPhase1);
        prog.done();
        std::cout << "Found " << corruptedBlocksCount << " corrupted block(s).\n";
    }

    if (corruptedBlocksCount == 0) {
        if (!resolvedDamagedPath.empty()) {
            std::cout << "No corruption detected; file is intact.\n";
        } else {
            std::cout << "No corruption detected.\n";
        }
        std::cout << "File is already intact.";
        showElapsed(startTime); std::cout << "\n";
        return;
    }

    std::cout << "Injecting parity packets...\n";

    // ── Parse parity packets from primary + supplemental archives ──
    struct ParsedPacket {
        uint32_t fountainId;
        const uint8_t* payload;
        uint32_t payloadSize;
        uint32_t blockSequence;
        uint64_t payloadHash;
        uint64_t expectedBlockHash;
        uint32_t stripeIndex;
    };
    std::vector<std::vector<ParsedPacket>> trackPackets(archive.totalTracks);
    size_t parityPacketsSeen = 0;

    std::vector<ArchiveMapping> archiveMappings;

    auto onPacket = [&](uint16_t track, const uint8_t* payload, uint32_t payloadSize,
                        uint32_t fountainId, uint32_t blockSequence,
                        uint64_t payloadHash, uint64_t expectedBlockHash, uint32_t stripeIndex) {
        trackPackets[track].push_back({fountainId, payload, payloadSize, blockSequence, payloadHash, expectedBlockHash, stripeIndex});
    };

    walkArchivePackets(parityPath, archive, true, archiveMappings, parityPacketsSeen, onPacket);
    scanSupplementArchives(parityPath, archive, archiveMappings, parityPacketsSeen, onPacket);

    auto tInjectHash = std::chrono::steady_clock::now();
    std::vector<std::vector<uint8_t>> trackPacketValid(archive.totalTracks);
    {
        size_t totalParityPackets = 0;
        for (uint16_t t = 0; t < archive.totalTracks; ++t) {
            trackPacketValid[t].resize(trackPackets[t].size(), false);
            totalParityPackets += trackPackets[t].size();
        }

        unsigned int numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) numThreads = 4;

        struct PacketRef { uint16_t track; size_t idx; const uint8_t* data; uint32_t size; uint64_t expectedHash; };
        std::vector<PacketRef> allRefs;
        allRefs.reserve(totalParityPackets);
        for (uint16_t t = 0; t < archive.totalTracks; ++t) {
            for (size_t i = 0; i < trackPackets[t].size(); ++i) {
                auto& pp = trackPackets[t][i];
                allRefs.push_back({t, i, pp.payload, pp.payloadSize, pp.payloadHash});
            }
        }

        uint64_t chunkSize = (totalParityPackets + numThreads - 1) / numThreads;
        std::vector<std::future<void>> hashFutures;
        for (uint64_t b = 0; b < totalParityPackets; b += chunkSize) {
            uint64_t end = (b + chunkSize > totalParityPackets) ? totalParityPackets : b + chunkSize;
            hashFutures.push_back(std::async(std::launch::async, [&, b, end]() {
                for (uint64_t i = b; i < end; ++i) {
                    auto& ref = allRefs[i];
                    trackPacketValid[ref.track][ref.idx] = (archive.hashBlock(ref.data, ref.size) == ref.expectedHash);
                }
            }));
        }
        for (auto& f : hashFutures) f.get();
    }
    injectHashTime = elapsedSecsSince(tInjectHash);

    std::cout << "Parity packets: " << parityPacketsSeen << " seen\n";

    auto tDecodeFeed = std::chrono::steady_clock::now();

    std::vector<std::vector<uint8_t>> recoveredTracks(archive.totalTracks);
    std::atomic<bool> executionSuccess{true};
    std::atomic<uint64_t> atomicDecodeCalls{0};

    if (!resolvedDamagedPath.empty()) {
        os::Mapping feedMapping;
        if (os::MapRead(resolvedDamagedPath.c_str(), feedMapping)) {
            mappedData = static_cast<const uint8_t*>(feedMapping.data);
            mappingFeed = std::move(feedMapping);
            hasMapping = true;
        } else {
            fallbackFile.open(resolvedDamagedPath, std::ios::binary);
        }
    }

    auto rejectPathTraversal = [&](const std::filesystem::path& base, const std::string& rel) -> bool {
        auto resolved = std::filesystem::absolute(base / rel).lexically_normal();
        auto baseNorm = std::filesystem::absolute(base).lexically_normal();
        auto rs = resolved.u8string();
        auto bs = baseNorm.u8string();
        while (bs.size() > 1 && (bs.back() == '/' || bs.back() == '\\'))
            bs.pop_back();
        if (rs.size() < bs.size()) return false;
        if (rs.compare(0, bs.size(), bs) != 0) return false;
        if (rs.size() > bs.size() && rs[bs.size()] != '/' && rs[bs.size()] != '\\') return false;
        return true;
    };

    std::string outFile;
    bool inPlace = false;
    if (!archive.canonicalManifest.empty() && archive.canonicalManifest.size() == 1) {
        auto& me = archive.canonicalManifest[0];
        std::filesystem::path outP(outputPath);
        if (std::filesystem::is_directory(outP) && !rejectPathTraversal(outP, me.relPath)) {
            std::cerr << "Error: manifest path '" << me.relPath << "' escapes output directory.\n";
            return;
        }
        outFile = std::filesystem::is_directory(outP) ? (outP / me.relPath).string() : outputPath;
        auto normPath = [](const std::string& p) -> std::string {
            return std::filesystem::absolute(std::filesystem::path(p)).lexically_normal().u8string();
        };
        inPlace = !resolvedDamagedPath.empty() && normPath(outFile) == normPath(resolvedDamagedPath);
    }

    unsigned int BATCH_SIZE = 2;
    if (numJobs > 0) {
        BATCH_SIZE = numJobs;
    } else if (maxMemBytes > 0 && archive.totalTracks > 0) {
        uint64_t perDecoder = archive.globalMeta.originalFileSize / archive.totalTracks + archive.globalMeta.blockSize;
        perDecoder = std::max(perDecoder * 2, uint64_t(1024 * 1024));
        if (archive.stripeCount > 1) {
            uint64_t bpt = (archive.blocksPerStripe + archive.totalTracks - 1) / archive.totalTracks;
            perDecoder = std::max(bpt * archive.globalMeta.blockSize * 3, uint64_t(1024 * 1024));
        }
        unsigned int m = static_cast<unsigned int>(maxMemBytes / perDecoder);
        if (m > 0) BATCH_SIZE = m;
        if (BATCH_SIZE < 1) BATCH_SIZE = 1;
    } else {
        uint64_t totalMemMB = os::TotalMemoryMB();
        if (totalMemMB > 0 && totalMemMB >= 40ULL * 1024) BATCH_SIZE = 3;
    }
    if (BATCH_SIZE > archive.totalTracks) BATCH_SIZE = archive.totalTracks;

    std::mutex writeMutex;

    if (archive.stripeCount <= 1) {
        // ── Non-stripe decode (original behavior, unchanged) ──
        for (uint16_t batchStart = 0; batchStart < archive.totalTracks && executionSuccess; batchStart += BATCH_SIZE) {
            uint16_t batchEnd = (batchStart + BATCH_SIZE < archive.totalTracks) ? (batchStart + BATCH_SIZE) : archive.totalTracks;
            std::vector<std::future<void>> futures;

            for (uint16_t t = batchStart; t < batchEnd; ++t) {
                std::cout << "Track " << t << ": " << trackHealthyBlocks[t].size() << " healthy blocks, "
                          << trackPackets[t].size() << " parity packets\n";

                futures.push_back(std::async(std::launch::async, [&, t]() {
                    WirehairCodec dec = wirehair_decoder_create(nullptr, archive.trackSizes[t], archive.globalMeta.blockSize);
                    if (!dec) {
                        std::cerr << "Failed to create decoder for track " << t << "\n";
                        executionSuccess = false;
                        return;
                    }

                    bool solved = false;
                    uint64_t localCalls = 0;
                    std::vector<uint8_t> readBuf(archive.globalMeta.blockSize);
                    int curFileIdx = -1;
                    os::FileHandle hMultiIn = os::InvalidHandle();

                    for (auto& ref : trackHealthyBlocks[t]) {
                        if (solved) break;
                        const uint8_t* srcData = mappedData + ref.fileOffset;
                        if (!hasMapping) {
                            bool multiFile = resolvedDamagedPath.empty() || ref.fileIndex > 0;
                            if (multiFile) {
                                if (!readBlockFromFiles(ref.fileOffset, ref.blockSize, readBuf, curFileIdx, hMultiIn)) continue;
                                srcData = readBuf.data();
                            } else if (fallbackFile.is_open()) {
                                std::lock_guard<std::mutex> lock(fallbackMutex);
                                fallbackFile.seekg(ref.fileOffset);
                                fallbackFile.read(reinterpret_cast<char*>(readBuf.data()), ref.blockSize);
                                srcData = readBuf.data();
                            } else {
                                continue;
                            }
                        }
                        localCalls++;
                        WirehairResult res = wirehair_decode(dec, ref.internalTrackBlockId, srcData, ref.blockSize);
                        if (res == Wirehair_Success) {
                            solved = true;
                        } else if (res != Wirehair_NeedMore) {
                            std::cerr << "Decoder error " << static_cast<int>(res) << " for track " << t << " at block " << ref.internalTrackBlockId << "\n";
                        }
                    }
                    if (hMultiIn != os::InvalidHandle()) os::Close(hMultiIn);

                    if (!solved) {
                        for (size_t i = 0; i < trackPackets[t].size(); ++i) {
                            if (solved) break;
                            if (trackPacketValid[t][i]) {
                                auto& pp = trackPackets[t][i];
                                localCalls++;
                                WirehairResult res = wirehair_decode(dec, pp.fountainId, pp.payload, pp.payloadSize);
                                if (res == Wirehair_Success) {
                                    solved = true;
                                } else if (res != Wirehair_NeedMore) {
                                    std::cerr << "Decoder error " << static_cast<int>(res) << " for track " << t << " (fountainId=" << pp.fountainId << ")\n";
                                }
                            }
                        }
                    }

                    atomicDecodeCalls += localCalls;

                    if (!solved) {
                        std::cerr << "CRITICAL: Insufficient data to solve track " << t << "!\n";
                        executionSuccess = false;
                        wirehair_free(dec);
                        return;
                    }

                    recoveredTracks[t].resize(archive.trackSizes[t]);
                    WirehairResult rr = wirehair_recover(dec, recoveredTracks[t].data(), archive.trackSizes[t]);
                    wirehair_free(dec);

                    if (rr != Wirehair_Success) {
                        std::cerr << "Recovery failed on track " << t << " (result=" << static_cast<int>(rr) << ")\n";
                        executionSuccess = false;
                    } else if (inPlace) {
                        std::lock_guard<std::mutex> lock(writeMutex);
                        os::FileHandle hTrackOut = os::OpenReadWrite(resolvedDamagedPath.c_str());
                        if (hTrackOut != os::InvalidHandle()) {
                            size_t trackByteOffset = 0;
                            for (uint64_t g = t; g < archive.totalBlocks; g += archive.totalTracks) {
                                uint64_t fileOff = g * archive.globalMeta.blockSize;
                                size_t blockSz = (fileOff + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                                    ? archive.globalMeta.blockSize
                                    : (archive.globalMeta.originalFileSize - fileOff);
                                if (g < blockCorrupted.size() && blockCorrupted[g]) {
                                    if (!os::Seek(hTrackOut, static_cast<int64_t>(fileOff), 0)) {
                                        std::cerr << "Error: Seek failed during in-place write.\n";
                                        executionSuccess = false;
                                        break;
                                    }
                                    if (!os::Write(hTrackOut, recoveredTracks[t].data() + trackByteOffset, static_cast<uint32_t>(blockSz))) {
                                        std::cerr << "Error: Write failed during in-place repair (disk full?).\n";
                                        executionSuccess = false;
                                        break;
                                    }
                                }
                                trackByteOffset += blockSz;
                            }
                            os::Close(hTrackOut);
                        }
                        recoveredTracks[t].clear();
                        recoveredTracks[t].shrink_to_fit();
                    }
                    {
                        std::lock_guard<std::mutex> lock(writeMutex);
                        std::cout << "Track " << t << " decoded.\n";
                    }
                }));
            }

            for (auto& f : futures) f.get();
        }
    } else {
        // ── Stripe decode ──
        for (uint16_t t = 0; t < archive.totalTracks; ++t)
            recoveredTracks[t].resize(archive.trackSizes[t]);
        std::vector<size_t> trackWriteOffsets(archive.totalTracks, 0);
        std::vector<std::vector<uint64_t>> trackFailedStripes(archive.totalTracks);

        for (uint64_t s = 0; s < archive.stripeCount && executionSuccess; ++s) {
            uint64_t stripeStartBlock = s * archive.blocksPerStripe;
            uint64_t stripeEndBlock = std::min(stripeStartBlock + archive.blocksPerStripe, archive.totalBlocks);

            // Per-stripe per-track sizes
            std::vector<uint64_t> stripeTrackSizes(archive.totalTracks, 0);
            for (uint64_t i = stripeStartBlock; i < stripeEndBlock; ++i) {
                uint16_t tid = static_cast<uint16_t>(i % archive.totalTracks);
                uint64_t off = i * archive.globalMeta.blockSize;
                size_t sz = (off + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                    ? archive.globalMeta.blockSize : (archive.globalMeta.originalFileSize - off);
                stripeTrackSizes[tid] += sz;
            }

            for (uint16_t batchStart = 0; batchStart < archive.totalTracks && executionSuccess; batchStart += BATCH_SIZE) {
                uint16_t batchEnd = (batchStart + BATCH_SIZE < archive.totalTracks) ? (batchStart + BATCH_SIZE) : archive.totalTracks;
                std::vector<std::future<void>> futures;

                for (uint16_t t = batchStart; t < batchEnd; ++t) {
                    if (stripeTrackSizes[t] == 0) continue;
                    uint64_t hCount = 0;
                    for (auto& ref : trackHealthyBlocks[t]) {
                        uint64_t g = ref.internalTrackBlockId * archive.totalTracks + t;
                        if (g >= stripeStartBlock && g < stripeEndBlock) hCount++;
                    }
                    std::cout << "Stripe " << s << " Track " << t << ": " << hCount << " healthy blocks, "
                              << trackPackets[t].size() << " total parity packets\n";

                    futures.push_back(std::async(std::launch::async, [&, t, s, stripeStartBlock, stripeEndBlock]() {
                        uint64_t stripeSize = stripeTrackSizes[t];
                        WirehairCodec dec = wirehair_decoder_create(nullptr, stripeSize, archive.globalMeta.blockSize);
                        if (!dec) {
                            std::cerr << "Failed to create decoder for stripe " << s << " track " << t << "\n";
                            executionSuccess = false;
                            return;
                        }

                        bool solved = false;
                        uint64_t localCalls = 0;
                        std::vector<uint8_t> readBuf(archive.globalMeta.blockSize);
                        int curFileIdx = -1;
                        os::FileHandle hMultiIn = os::InvalidHandle();

                        // Feed healthy blocks in this stripe
                        // Stripe-relative block ID = position within stripe for this track
                        for (auto& ref : trackHealthyBlocks[t]) {
                            if (solved) break;
                            uint64_t globalIdx = ref.internalTrackBlockId * archive.totalTracks + t;
                            if (globalIdx < stripeStartBlock || globalIdx >= stripeEndBlock) continue;

                            const uint8_t* srcData = mappedData + ref.fileOffset;
                            if (!hasMapping) {
                                bool multiFile = resolvedDamagedPath.empty() || ref.fileIndex > 0;
                                if (multiFile) {
                                    if (!readBlockFromFiles(ref.fileOffset, ref.blockSize, readBuf, curFileIdx, hMultiIn)) continue;
                                    srcData = readBuf.data();
                                } else if (fallbackFile.is_open()) {
                                    std::lock_guard<std::mutex> lock(fallbackMutex);
                                    fallbackFile.seekg(ref.fileOffset);
                                    fallbackFile.read(reinterpret_cast<char*>(readBuf.data()), ref.blockSize);
                                    srcData = readBuf.data();
                                } else {
                                    continue;
                                }
                            }
                            localCalls++;
                            uint32_t relId = static_cast<uint32_t>((globalIdx - stripeStartBlock) / archive.totalTracks);
                            WirehairResult res = wirehair_decode(dec, relId, srcData, ref.blockSize);
                            if (res == Wirehair_Success) {
                                solved = true;
                            } else if (res != Wirehair_NeedMore) {
                                std::cerr << "Decoder error at stripe " << s << " track " << t << " block " << relId << "\n";
                            }
                        }
                        if (hMultiIn != os::InvalidHandle()) os::Close(hMultiIn);

                        // Feed parity packets for this stripe
                        if (!solved) {
                            for (size_t i = 0; i < trackPackets[t].size(); ++i) {
                                if (solved) break;
                                if (!trackPacketValid[t][i]) continue;
                                auto& pp = trackPackets[t][i];
                                if (pp.stripeIndex != s) continue;
                                localCalls++;
                                WirehairResult res = wirehair_decode(dec, pp.fountainId, pp.payload, pp.payloadSize);
                                if (res == Wirehair_Success) {
                                    solved = true;
                                } else if (res != Wirehair_NeedMore) {
                                    std::cerr << "Decoder error at stripe " << s << " track " << t << " (fountainId=" << pp.fountainId << ")\n";
                                }
                            }
                        }

                        atomicDecodeCalls += localCalls;

                        if (!solved) {
                            std::cout << "  Warning: Stripe " << s << " track " << t << " insufficient parity.\n";
                            wirehair_free(dec);
                            trackFailedStripes[t].push_back(s);
                            return;
                        }

                        std::vector<uint8_t> stripeBuf(static_cast<size_t>(stripeSize));
                        WirehairResult rr = wirehair_recover(dec, stripeBuf.data(), stripeSize);
                        wirehair_free(dec);

                        if (rr != Wirehair_Success) {
                            std::cerr << "Recovery failed on stripe " << s << " track " << t << " (result=" << static_cast<int>(rr) << ")\n";
                            executionSuccess = false;
                            return;
                        }


                        // Copy to recoveredTracks[t] at accumulated offset
                        size_t writeOff = trackWriteOffsets[t];
                        memcpy(recoveredTracks[t].data() + writeOff, stripeBuf.data(), static_cast<size_t>(stripeSize));

                        // In-place: write only corrupted blocks within this stripe
                        if (inPlace) {
                            std::lock_guard<std::mutex> lock(writeMutex);
                            os::FileHandle hTrackOut = os::OpenReadWrite(resolvedDamagedPath.c_str());
                            if (hTrackOut != os::InvalidHandle()) {
                                size_t stripeByteOffset = 0;
                                for (uint64_t g = stripeStartBlock; g < stripeEndBlock; ++g) {
                                    if (g % archive.totalTracks != t) continue;
                                    uint64_t fileOff = g * archive.globalMeta.blockSize;
                                    size_t blockSz = (fileOff + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                                        ? archive.globalMeta.blockSize : (archive.globalMeta.originalFileSize - fileOff);
                                    if (g < blockCorrupted.size() && blockCorrupted[g]) {
                                        os::Seek(hTrackOut, static_cast<int64_t>(fileOff), 0);
                                        os::Write(hTrackOut, stripeBuf.data() + stripeByteOffset, static_cast<uint32_t>(blockSz));
                                    }
                                    stripeByteOffset += blockSz;
                                }
                                os::Close(hTrackOut);
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(writeMutex);
                            std::cout << "Stripe " << s << " track " << t << " decoded.\n";
                        }
                    }));
                }
                for (auto& f : futures) f.get();

                // Update track write offsets after all tracks in this batch complete
                for (uint16_t t = batchStart; t < batchEnd; ++t) {
                    bool failed = false;
                    for (auto fs : trackFailedStripes[t]) {
                        if (fs == s) { failed = true; break; }
                    }
                    if (!failed)
                        trackWriteOffsets[t] += stripeTrackSizes[t];
                }
            }
        }

        // Check for stripe-level failures (some tracks could not be fully decoded per-stripe)
        uint64_t totalFailedTracks = 0;
        for (uint16_t t = 0; t < archive.totalTracks; ++t) {
            if (!trackFailedStripes[t].empty()) {
                std::cout << "  Warning: Track " << t << " has " << trackFailedStripes[t].size() << " unrecoverable stripe(s):";
                for (auto s : trackFailedStripes[t]) std::cout << " " << s;
                std::cout << "\n";
                totalFailedTracks++;
            }
        }
        if (totalFailedTracks > 0) {
            std::cout << "  Parity packets from striped archives are stripe-specific and cannot be used\n"
                      << "  for whole-track recovery. Retry without --max-mem to use global parity.\n";
            executionSuccess = false;
        }

        // Free per-track recovered data if in-place (already written)
        if (inPlace) {
            for (uint16_t t = 0; t < archive.totalTracks; ++t) {
                recoveredTracks[t].clear();
                recoveredTracks[t].shrink_to_fit();
            }
        }
    }

    decodeFeedCalls = atomicDecodeCalls.load();

    std::cout << "All tracks completed.\n";

    if (hasMapping) {
        os::Unmap(mappingFeed);
    }
    if (fallbackFile.is_open()) fallbackFile.close();

    for (auto& am : archiveMappings) {
        if (am.mapping.data) os::Unmap(am.mapping);
    }

    decodeFeedTime = elapsedSecsSince(tDecodeFeed);

    if (executionSuccess) {
        if (inPlace) {
            std::cout << "Restored in-place: " << outFile << "\n";
        } else if (maxMemBytes > 0) {
            // ── Streaming output path (no fullMessage buffer) ──
            // Iterate all blocks once, routing each directly to the correct
            // output file. For existing files, only overwrite corrupted blocks.
            std::vector<size_t> trackOffsets(archive.totalTracks, 0);

            // Resolve per-file target info
            struct FileTarget {
                std::string path;
                uint64_t fileStart, fileEnd;
                bool exists;
            };
            std::vector<FileTarget> targets;
            uint64_t cumOff = 0;
            for (auto& me : archive.canonicalManifest) {
                std::filesystem::path outPath(outputPath);
                if (!rejectPathTraversal(outPath, me.relPath)) {
                    std::cerr << "Error: manifest path '" << me.relPath << "' escapes output directory.\n";
                    return;
                }
                auto parent = outPath / std::filesystem::path(me.relPath).parent_path();
                std::filesystem::create_directories(parent);
                std::string filePath = (outPath / me.relPath).string();
                bool exists = std::filesystem::exists(filePath)
                    && std::filesystem::is_regular_file(filePath)
                    && std::filesystem::file_size(filePath) == me.fileSize;
                targets.push_back({filePath, cumOff, cumOff + me.fileSize, exists});
                cumOff += me.fileSize;
            }

            int curTarget = -1;
            os::FileHandle hCurFile = os::InvalidHandle();
            os::Mapping curMapping = {};
            for (uint64_t g = 0; g < archive.totalBlocks; ++g) {
                uint64_t blockByteStart = g * archive.globalMeta.blockSize;

                if (curTarget < 0 || blockByteStart >= targets[curTarget].fileEnd) {
                    if (hCurFile != os::InvalidHandle()) {
                        os::Unmap(curMapping);
                        os::Close(hCurFile); hCurFile = os::InvalidHandle();
                    }
                    curTarget++;
                    while (curTarget < (int)targets.size() && blockByteStart >= targets[curTarget].fileEnd)
                        curTarget++;
                    if (curTarget >= (int)targets.size()) break;
                    auto& tgt = targets[curTarget];
                    if (tgt.exists) {
                        hCurFile = os::OpenReadWrite(tgt.path.c_str());
                    } else {
                        os::FileHandle hNew = os::CreateNew(tgt.path.c_str(), false);
                        if (hNew != os::InvalidHandle()) os::Close(hNew);
                        hCurFile = os::OpenReadWrite(tgt.path.c_str());
                    }
                    if (hCurFile == os::InvalidHandle()) {
                        std::cerr << "Error: Cannot open output file: " << tgt.path << "\n";
                        return;
                    }
                    uint64_t fileSz = tgt.fileEnd - tgt.fileStart;
                    if (!tgt.exists && !os::SetFileSize(hCurFile, fileSz)) {
                        std::cerr << "Error: Cannot set output file size: " << tgt.path << "\n";
                        os::Close(hCurFile);
                        return;
                    }
                    if (!os::MapWrite(hCurFile, fileSz, curMapping)) {
                        std::cerr << "Error: Cannot memory-map output file: " << tgt.path << "\n";
                        os::Close(hCurFile);
                        return;
                    }
                }

                auto& tgt = targets[curTarget];
                uint16_t tid = g % archive.totalTracks;
                size_t currentBlockSize = (blockByteStart + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                    ? archive.globalMeta.blockSize : (archive.globalMeta.originalFileSize - blockByteStart);

                bool shouldWrite = !tgt.exists || (g < blockCorrupted.size() && blockCorrupted[g]);
                if (shouldWrite) {
                    uint64_t writeStart = (blockByteStart < tgt.fileStart) ? tgt.fileStart : blockByteStart;
                    uint64_t blockByteEnd = blockByteStart + currentBlockSize;
                    if (blockByteEnd > archive.globalMeta.originalFileSize) blockByteEnd = archive.globalMeta.originalFileSize;
                    uint64_t writeEnd = (blockByteEnd < tgt.fileEnd) ? blockByteEnd : tgt.fileEnd;
                    if (writeStart < writeEnd) {
                        uint64_t localOff = writeStart - tgt.fileStart;
                        uint32_t writeSz = static_cast<uint32_t>(writeEnd - writeStart);
                        memcpy(static_cast<uint8_t*>(curMapping.data) + localOff,
                               recoveredTracks[tid].data() + trackOffsets[tid] + (writeStart - blockByteStart),
                               writeSz);
                    }
                }
                trackOffsets[tid] += currentBlockSize;
            }
            if (hCurFile != os::InvalidHandle()) {
                os::Unmap(curMapping);
                os::Close(hCurFile);
            }

            // Restore attributes/mtime for all targets
            cumOff = 0;
            for (size_t fi = 0; fi < targets.size(); ++fi) {
                auto& me = archive.canonicalManifest[fi];
                if (!os::SetAttributes(targets[fi].path.c_str(), me.attributes))
                    std::cout << "  Warning: Failed to set attributes on '" << targets[fi].path << "'\n";
                if (me.mtime > 0 && !os::SetModificationTime(targets[fi].path.c_str(), me.mtime))
                    std::cout << "  Warning: Failed to set modification time on '" << targets[fi].path << "'\n";
            }

            if (archive.canonicalManifest.size() == 1)
                std::cout << "Restored to: " << targets[0].path << "\n";
            else
                std::cout << "Restored " << archive.canonicalManifest.size() << " file(s) to: " << outputPath << "\n";
        } else {
            // ── Original fullMessage output path (backward compat) ──
            if (archive.canonicalManifest.size() == 1) {
                std::vector<uint8_t> fullMessage(static_cast<size_t>(archive.globalMeta.originalFileSize));
                std::vector<size_t> trackOffsets(archive.totalTracks, 0);
                for (uint64_t i = 0; i < archive.totalBlocks; ++i) {
                    uint16_t trackId = i % archive.totalTracks;
                    uint64_t offset = i * archive.globalMeta.blockSize;
                    size_t currentBlockSize = (offset + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                        ? archive.globalMeta.blockSize
                        : (archive.globalMeta.originalFileSize - offset);
                    memcpy(fullMessage.data() + offset, recoveredTracks[trackId].data() + trackOffsets[trackId], currentBlockSize);
                    trackOffsets[trackId] += currentBlockSize;
                }
                std::ofstream out(outFile, std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(fullMessage.data()), fullMessage.size());
                    out.close();
                }
                std::cout << "Restored to: " << outFile << "\n";
            } else {
                std::vector<uint8_t> fullMessage(static_cast<size_t>(archive.globalMeta.originalFileSize));
                std::vector<size_t> trackOffsets(archive.totalTracks, 0);
                for (uint64_t i = 0; i < archive.totalBlocks; ++i) {
                    uint16_t trackId = i % archive.totalTracks;
                    uint64_t offset = i * archive.globalMeta.blockSize;
                    size_t currentBlockSize = (offset + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                        ? archive.globalMeta.blockSize
                        : (archive.globalMeta.originalFileSize - offset);
                    memcpy(fullMessage.data() + offset, recoveredTracks[trackId].data() + trackOffsets[trackId], currentBlockSize);
                    trackOffsets[trackId] += currentBlockSize;
                }
                uint64_t cumulativeOffset = 0;
                for (auto& me : archive.canonicalManifest) {
                    std::filesystem::path outPath(outputPath);
                    if (!rejectPathTraversal(outPath, me.relPath)) {
                        std::cerr << "Error: manifest path '" << me.relPath << "' escapes output directory.\n";
                        return;
                    }
                    auto parent = outPath / std::filesystem::path(me.relPath).parent_path();
                    std::filesystem::create_directories(parent);
                    std::string filePath = (outPath / me.relPath).string();
                    uint64_t fileStart = cumulativeOffset;
                    uint64_t fileEnd = cumulativeOffset + me.fileSize;
                    bool exists = std::filesystem::exists(filePath)
                        && std::filesystem::is_regular_file(filePath)
                        && std::filesystem::file_size(filePath) == me.fileSize;
                    if (exists) {
                        uint64_t firstBlock = fileStart / archive.globalMeta.blockSize;
                        uint64_t lastBlock = (fileEnd - 1) / archive.globalMeta.blockSize;
                        os::FileHandle hFile = os::OpenReadWrite(filePath.c_str());
                        if (hFile != os::InvalidHandle()) {
                            for (uint64_t g = firstBlock; g <= lastBlock && g < archive.totalBlocks; ++g) {
                                if (g < blockCorrupted.size() && blockCorrupted[g]) {
                                    uint64_t blockByteStart = g * archive.globalMeta.blockSize;
                                    uint64_t writeStart = (blockByteStart < fileStart) ? fileStart : blockByteStart;
                                    uint64_t blockByteEnd = blockByteStart + archive.globalMeta.blockSize;
                                    if (blockByteEnd > archive.globalMeta.originalFileSize) blockByteEnd = archive.globalMeta.originalFileSize;
                                    uint64_t writeEnd = (blockByteEnd < fileEnd) ? blockByteEnd : fileEnd;
                                    if (writeStart >= writeEnd) continue;
                                    uint64_t localOff = writeStart - fileStart;
                                    uint32_t writeSz = static_cast<uint32_t>(writeEnd - writeStart);
                                    os::Seek(hFile, static_cast<int64_t>(localOff), 0);
                                    os::Write(hFile, fullMessage.data() + writeStart, writeSz);
                                }
                            }
                            os::Close(hFile);
                        }
                        if (!os::SetAttributes(filePath.c_str(), me.attributes))
                            std::cout << "  Warning: Failed to set attributes on '" << filePath << "'\n";
                        if (me.mtime > 0 && !os::SetModificationTime(filePath.c_str(), me.mtime))
                            std::cout << "  Warning: Failed to set modification time on '" << filePath << "'\n";
                    } else {
                        std::ofstream outFile(filePath, std::ios::binary);
                        if (outFile) {
                            outFile.write(reinterpret_cast<const char*>(fullMessage.data() + cumulativeOffset), static_cast<std::streamsize>(me.fileSize));
                            if (!outFile.good()) {
                                std::cerr << "Error: Failed to write restored file (disk full?).\n";
                                return;
                            }
                            outFile.close();
                            if (!os::SetAttributes(filePath.c_str(), me.attributes))
                                std::cout << "  Warning: Failed to set attributes on '" << filePath << "'\n";
                            if (me.mtime > 0 && !os::SetModificationTime(filePath.c_str(), me.mtime))
                                std::cout << "  Warning: Failed to set modification time on '" << filePath << "'\n";
                        }
                    }
                    cumulativeOffset += me.fileSize;
                }
                std::cout << "Restored " << archive.canonicalManifest.size() << " file(s) to: " << outputPath << "\n";
            }
        }
    }

    if (showTiming || debug) {
        std::cout << "\n-- Timing breakdown --\n"
                  << "  Analysis (hash):            " << phase1HashTime << "s\n"
                  << "  Decode + Recovery:          " << decodeFeedTime << "s (" << decodeFeedCalls << " calls, "
                  << (decodeFeedCalls > 0 ? std::to_string(decodeFeedTime / decodeFeedCalls * 1e6) : "?") << " us/call)\n"
                  << "  Inject (parity hash):       " << injectHashTime << "s\n";
    }

    if (executionSuccess) {
        std::cout << "SUCCESS: Data fully recovered.";
    } else {
        uint64_t totalParity = parityPacketsSeen;
        uint64_t needEstimate = 0;
        for (uint16_t t = 0; t < archive.totalTracks; ++t) {
            uint64_t h = trackHealthyBlocks[t].size();
            uint64_t p = trackPackets[t].size();
            if (p < h + 1) needEstimate += (h + 1) - p;
        }
        int extraPct10 = static_cast<int>((needEstimate * 1000.0 / archive.totalBlocks) + 0.5);
        std::cout << "FAILURE: Repair incomplete. Have " << archive.totalBlocks << " data blocks, "
                  << totalParity << " parity packets.\n"
                  << "         Need ~" << needEstimate << " more packets (≈"
                  << (extraPct10 / 10) << "." << (extraPct10 % 10) << "% additional overhead).";
    }
    showElapsed(startTime); std::cout << "\n";
}

void ListManifest(const std::string& parityPath) {
    ParsedArchive archive = parseArchive(parityPath);
    if (!archive.valid) return;

    std::cout << "\nSource files:\n";
    for (size_t i = 0; i < archive.canonicalManifest.size(); ++i) {
        auto& me = archive.canonicalManifest[i];
        std::cout << "  " << me.relPath
                  << " (" << (me.fileSize / (1024 * 1024.0)) << " MiB)";
        if (me.mtime > 0) {
            std::time_t t = static_cast<std::time_t>(me.mtime);
            char buf[64];
            struct tm gmt;
#ifdef _WIN32
            gmtime_s(&gmt, &t);
#else
            gmtime_r(&t, &gmt);
#endif
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &gmt);
            std::cout << " - " << buf << " UTC";
        }
        if (me.attributes != 0) {
            std::cout << " [attrs:0x" << std::hex << me.attributes << std::dec << "]";
        }
        std::cout << "\n";
    }
}

void InfoCheck(const std::string& parityPath, bool debug, const std::string& sourceDir) {
    auto startTime = std::chrono::steady_clock::now();

    std::cout << "Archive: " << parityPath << "\n";
    ParsedArchive archive = parseArchive(parityPath);
    if (!archive.valid) return;

    // ── 2. Scan and parse parity packets ─────────────────
    std::vector<std::vector<const uint8_t*>> trackParityPayloads(archive.totalTracks);
    std::vector<std::vector<uint64_t>> trackParityHashes(archive.totalTracks);
    size_t parityPacketsSeen = 0;

    std::vector<ArchiveMapping> archiveMappings;

    auto onPacket = [&](uint16_t track, const uint8_t* payload, uint32_t payloadSize,
                        uint32_t fountainId, uint32_t blockSequence,
                        uint64_t payloadHash, uint64_t expectedBlockHash, uint32_t stripeIndex) {
        (void)fountainId; (void)blockSequence; (void)expectedBlockHash; (void)stripeIndex;
        trackParityPayloads[track].push_back(payload);
        trackParityHashes[track].push_back(payloadHash);
    };

    walkArchivePackets(parityPath, archive, true, archiveMappings, parityPacketsSeen, onPacket);
    scanSupplementArchives(parityPath, archive, archiveMappings, parityPacketsSeen, onPacket);

    // Hash-verify parity packets
    std::vector<uint32_t> validParityPerTrack(archive.totalTracks, 0);
    for (uint16_t t = 0; t < archive.totalTracks; ++t) {
        for (size_t i = 0; i < trackParityPayloads[t].size(); ++i) {
            uint32_t sz = archive.globalMeta.blockSize;
            if (archive.hashBlock(trackParityPayloads[t][i], sz) == trackParityHashes[t][i])
                validParityPerTrack[t]++;
        }
    }

    // Unmap archives
    for (auto& am : archiveMappings) {
        if (am.mapping.data) os::Unmap(am.mapping);
    }
    archiveMappings.clear();

    std::cout << "Total parity packets: " << parityPacketsSeen << " ("
              << (parityPacketsSeen > 0 ? std::to_string(validParityPerTrack[0]) : "0") << " valid)\n";

    // ── 3. Resolve source files ──────────────────────────
    std::string searchDir = sourceDir.empty() ? archive.archiveDir : sourceDir;
    std::vector<std::string> sourcePaths = resolveAllSources(archive.canonicalManifest, searchDir, archive.archiveDir);

    // Report source files
    std::cout << "\nSource files:\n";
    for (size_t i = 0; i < archive.canonicalManifest.size(); ++i) {
        std::cout << "  " << archive.canonicalManifest[i].relPath
                  << " (" << (archive.canonicalManifest[i].fileSize / (1024 * 1024.0)) << " MiB)";
        if (sourcePaths[i].empty()) {
            std::cout << " - NOT FOUND\n";
        } else {
            uint64_t actualSize = std::filesystem::file_size(sourcePaths[i]);
            if (actualSize == archive.canonicalManifest[i].fileSize) {
                std::cout << " - found\n";
            } else {
                std::cout << " - found but size mismatch (" << actualSize << " vs expected " << archive.canonicalManifest[i].fileSize << ")\n";
            }
        }
    }

    // ── 4. Hash source files, compare to references ──────
    std::vector<uint8_t> blockCorrupted(archive.totalBlocks, 0);
    std::vector<std::vector<uint32_t>> trackHealthyBlocks(archive.totalTracks);
    uint64_t corruptedBlocksCount = 0;

    FileOffsets fo = computeFileOffsets(archive.canonicalManifest);

    {
        std::vector<uint8_t> composeBuf(archive.globalMeta.blockSize);
        Progress prog(archive.totalBlocks, "Analysis");
        int curFileIdx = -1;
        os::FileHandle hCurFile = os::InvalidHandle();
        uint64_t fileCorrupted = 0;
        int reportFileIdx = -1;

        for (uint64_t i = 0; i < archive.totalBlocks; ++i) {
            uint64_t offset = i * archive.globalMeta.blockSize;
            size_t currentBlockSize = (offset + archive.globalMeta.blockSize <= archive.globalMeta.originalFileSize)
                ? archive.globalMeta.blockSize : (archive.globalMeta.originalFileSize - offset);

            int fileIdx = -1;
            for (size_t f = 0; f < fo.fileStartOffsets.size(); ++f) {
                if (offset >= fo.fileStartOffsets[f] && offset < fo.fileEndOffsets[f]) { fileIdx = static_cast<int>(f); break; }
            }

            if (fileIdx != reportFileIdx) {
                if (reportFileIdx >= 0) {
                    auto& me = archive.canonicalManifest[reportFileIdx];
                    if (fileCorrupted == 0)
                        std::cout << "  " << me.relPath << " (" << (me.fileSize / (1024 * 1024.0)) << " MiB) - OK\n";
                    else
                        std::cout << "  " << me.relPath << " (" << (me.fileSize / (1024 * 1024.0)) << " MiB) - " << fileCorrupted << " block(s) DAMAGED\n";
                }
                reportFileIdx = fileIdx;
                fileCorrupted = 0;
            }

            bool haveData = false;
            if (fileIdx >= 0 && fileIdx < (int)sourcePaths.size() && !sourcePaths[fileIdx].empty()) {
                if (fileIdx != curFileIdx) {
                    if (hCurFile != os::InvalidHandle()) os::Close(hCurFile);
                    hCurFile = os::OpenRead(sourcePaths[fileIdx].c_str(), false);
                    curFileIdx = fileIdx;
                }
                if (hCurFile != os::InvalidHandle()) {
                    uint64_t localOff = offset - fo.fileStartOffsets[fileIdx];
                    uint64_t avail = fo.fileEndOffsets[fileIdx] - offset;
                    size_t firstPart = static_cast<size_t>(std::min<uint64_t>(avail, currentBlockSize));
                    os::Seek(hCurFile, static_cast<int64_t>(localOff), 0);
                    uint32_t br = 0;
                    os::Read(hCurFile, composeBuf.data(), static_cast<uint32_t>(firstPart), br);
                    if (firstPart < currentBlockSize) {
                        size_t secondPart = currentBlockSize - firstPart;
                        int fileIdx2 = fileIdx + 1;
                        if (fileIdx2 < (int)archive.canonicalManifest.size() && fileIdx2 < (int)sourcePaths.size() && !sourcePaths[fileIdx2].empty()) {
                            os::FileHandle h2 = os::OpenRead(sourcePaths[fileIdx2].c_str(), false);
                            if (h2 != os::InvalidHandle()) {
                                uint32_t br2 = 0;
                                os::Read(h2, composeBuf.data() + firstPart, static_cast<uint32_t>(secondPart), br2);
                                os::Close(h2);
                            }
                        }
                    }
                    haveData = true;
                }
            }

            uint16_t trackId = i % archive.totalTracks;
            if (haveData) {
                uint64_t h = archive.hashBlock(composeBuf.data(), currentBlockSize);
                uint64_t expectedHash = (i < archive.referenceHashes.size()) ? archive.referenceHashes[i] : 0;
                if (h == expectedHash) {
                    trackHealthyBlocks[trackId].push_back(1);
                } else {
                    corruptedBlocksCount++; fileCorrupted++;
                    blockCorrupted[i] = 1;
                }
            } else {
                corruptedBlocksCount++; fileCorrupted++;
                blockCorrupted[i] = 1;
            }
            prog.tick();
        }

        // Report last file
        if (reportFileIdx >= 0) {
            auto& me = archive.canonicalManifest[reportFileIdx];
            if (fileCorrupted == 0)
                std::cout << "  " << me.relPath << " (" << (me.fileSize / (1024 * 1024.0)) << " MiB) - OK\n";
            else
                std::cout << "  " << me.relPath << " (" << (me.fileSize / (1024 * 1024.0)) << " MiB) - " << fileCorrupted << " block(s) DAMAGED\n";
        }

        if (hCurFile != os::InvalidHandle()) os::Close(hCurFile);
        prog.done();
    }

    // ── 5. Report ────────────────────────────────────────
    uint64_t healthyBlocks = archive.totalBlocks - corruptedBlocksCount;

    if (corruptedBlocksCount == 0) {
        std::cout << "Status: All files match hash - repair not needed.";
        showElapsed(startTime); std::cout << "\n";
        return;
    }

    std::cout << "Status: Corruption detected - " << corruptedBlocksCount << " block(s) damaged ("
              << healthyBlocks << " healthy).\n";

    // Per-track healthy count for need estimate
    uint64_t needEstimate = 0;
    uint64_t totalValidParity = 0;
    for (uint16_t t = 0; t < archive.totalTracks; ++t) {
        uint64_t h = trackHealthyBlocks[t].size();
        uint64_t p = validParityPerTrack[t];
        totalValidParity += p;
        if (p < h + 1) needEstimate += (h + 1) - p;
    }

    if (needEstimate > 0) {
        int extraPct10 = static_cast<int>((needEstimate * 1000.0 / archive.totalBlocks) + 0.5);
        std::cout << "FAILURE: Repair incomplete. Have " << archive.totalBlocks << " data blocks, "
                  << totalValidParity << " valid parity packets.\n"
                  << "         Need ~" << needEstimate << " more packets (≈"
                  << (extraPct10 / 10) << "." << (extraPct10 % 10) << "% additional overhead).";
        showElapsed(startTime); std::cout << "\n";
    } else {
        std::cout << "Sufficient parity available to repair.";
        showElapsed(startTime); std::cout << "\n";
    }

    // ── 6. Prompt for repair ─────────────────────────────
    std::cout << "\nWould you like to run repair now? (y/N): " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    if (answer == "y" || answer == "Y") {
        std::string outPath = sourceDir.empty() ? "." : sourceDir;
        RepairDataset(std::string(""), parityPath, outPath, false, debug, false, 0, 0);
    }
}
