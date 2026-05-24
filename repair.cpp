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
#include <thread>
#include <mutex>
#include <filesystem>
#include "portability.h"

void RepairDataset(const std::string& damagedPath, const std::string& parityPath, const std::string& outputPath, bool force, bool debug, bool showTiming) {
    auto startTime = std::chrono::steady_clock::now();
    auto showElapsed = [&]() {
        double e = elapsedSecsSince(startTime);
        if (e >= 60.0) {
            int m = static_cast<int>(e / 60);
            std::cout << " (" << m << "m " << (e - m * 60) << "s)";
        } else {
            std::cout << " (" << e << "s)";
        }
    };

    std::ifstream parityIn(parityPath, std::ios::binary);
    if (!parityIn) {
        std::cerr << "Error: Cannot open parity file: " << parityPath << "\n";
        showElapsed(); std::cerr << "\n";
        return;
    }

    parityIn.seekg(0, std::ios::end);
    std::streampos fileSize = parityIn.tellg();
    if (fileSize < static_cast<std::streampos>(sizeof(uint64_t))) {
        std::cerr << "Error: Archive too small.\n";
        showElapsed(); std::cerr << "\n";
        return;
    }
    parityIn.seekg(-static_cast<std::streamoff>(sizeof(uint64_t)), std::ios::end);
    uint64_t headerBlockSize = 0;
    readU64LE(parityIn, headerBlockSize);

    if (headerBlockSize < sizeof(PacketHeader) || headerBlockSize > static_cast<uint64_t>(fileSize) - sizeof(uint64_t)) {
        std::cerr << "Error: Invalid header size in archive footer.\n";
        showElapsed(); std::cerr << "\n";
        return;
    }
    std::streampos headerEndOffset = fileSize - static_cast<std::streampos>(sizeof(uint64_t)) - static_cast<std::streampos>(headerBlockSize);
    if (headerEndOffset <= 0 || headerEndOffset >= fileSize - static_cast<std::streampos>(sizeof(uint64_t))) {
        std::cerr << "Error: Corrupt archive header offset.\n";
        showElapsed(); std::cerr << "\n";
        return;
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

    PacketHeader globalMeta;
    std::vector<uint64_t> referenceHashes;
    std::vector<ManifestEntry> canonicalManifest;
    bool useXxh64;

    if (mainOk && mirrorOk) {
        if (hashesA == hashesEnd) {
            globalMeta = headerEnd;
            referenceHashes = std::move(hashesEnd);
            canonicalManifest = !manifestEnd.empty() ? std::move(manifestEnd) : std::move(manifestA);
        } else {
            std::cerr << "Warning: mirrored header mismatch; using primary.\n";
            globalMeta = headerA;
            referenceHashes = std::move(hashesA);
            canonicalManifest = std::move(manifestA);
        }
    } else if (mirrorOk) {
        globalMeta = headerEnd;
        referenceHashes = std::move(hashesEnd);
        canonicalManifest = std::move(manifestEnd);
        std::cerr << "Warning: primary header corrupt; recovered from mirrored header.\n";
    } else if (mainOk) {
        globalMeta = headerA;
        referenceHashes = std::move(hashesA);
        canonicalManifest = std::move(manifestA);
        std::cerr << "Warning: mirrored header missing or corrupt; using primary.\n";
    } else {
        std::cerr << "Error: Invalid or corrupt .whpar archive (both headers unusable)\n";
        showElapsed(); std::cerr << "\n";
        return;
    }

    useXxh64 = (globalMeta.matrixTrack == 1);
    auto hashBlock = [useXxh64](const uint8_t* data, size_t len) -> uint64_t {
        return useXxh64 ? XXH3_64bits(data, len) : XXH32(data, len, 0);
    };


    std::cout << "Valid archive metadata found!\n";
    std::cout << "Target File Size: " << (globalMeta.originalFileSize / (1024 * 1024.0)) << " MiB\n";
    std::cout << "Expected Blocks: " << referenceHashes.size() << " (" << (globalMeta.blockSize / 1024) << " KB each)\n";

    uint64_t totalBlocks = (globalMeta.originalFileSize + globalMeta.blockSize - 1) / globalMeta.blockSize;
    uint16_t totalTracks = static_cast<uint16_t>(globalMeta.fountainId);
    if (totalTracks == 0) totalTracks = 1;

    std::cout << "Interleaving Architecture: " << totalTracks << " parallel matrix track(s).\n";

    std::vector<uint64_t> trackSizes(totalTracks, 0);

    for (uint64_t i = 0; i < totalBlocks; ++i) {
        uint16_t trackId = i % totalTracks;
        uint64_t offset = i * globalMeta.blockSize;
        size_t currentBlockSize = (offset + globalMeta.blockSize <= globalMeta.originalFileSize) ? globalMeta.blockSize : (globalMeta.originalFileSize - offset);
        trackSizes[trackId] += currentBlockSize;
    }

    std::string resolvedDamagedPath = damagedPath;
    if (resolvedDamagedPath.empty() && !canonicalManifest.empty() && canonicalManifest.size() == 1) {
        std::filesystem::path candidate = std::filesystem::current_path() / std::filesystem::path(canonicalManifest[0].relPath);
        if (std::filesystem::exists(candidate)) {
            resolvedDamagedPath = candidate.string();
            std::cout << "Found local file matching manifest: " << resolvedDamagedPath << "\n";
        }
    }

    struct HealthyBlockRef {
        uint32_t internalTrackBlockId;
        uint64_t fileOffset;
        uint32_t blockSize;
        uint16_t fileIndex;
        uint64_t fileLocalOffset;
    };

    std::vector<uint64_t> fileStartOffsets;
    std::vector<uint64_t> fileEndOffsets;
    {
        uint64_t off = 0;
        for (auto& me : canonicalManifest) {
            fileStartOffsets.push_back(off);
            off += me.fileSize;
            fileEndOffsets.push_back(off);
        }
    }

    auto readBlockFromFiles = [&](uint64_t offset, uint32_t size, std::vector<uint8_t>& buf, int& cachedFileIdx, os::FileHandle& cachedHandle) -> bool {
        int fileIdx = 0;
        for (size_t f = 0; f < fileStartOffsets.size(); ++f) {
            if (offset >= fileStartOffsets[f] && offset < fileEndOffsets[f]) { fileIdx = static_cast<int>(f); break; }
        }
        if (fileIdx != cachedFileIdx) {
            if (cachedHandle != os::InvalidHandle()) os::Close(cachedHandle);
            std::string fpath = (std::filesystem::current_path() / canonicalManifest[fileIdx].relPath).string();
            cachedHandle = os::OpenRead(fpath.c_str(), false);
            cachedFileIdx = fileIdx;
        }
        std::fill(buf.begin(), buf.end(), 0);
        if (cachedHandle == os::InvalidHandle()) return false;
        uint64_t localOff = offset - fileStartOffsets[fileIdx];
        uint64_t avail = fileEndOffsets[fileIdx] - offset;
        size_t firstPart = static_cast<size_t>(std::min<uint64_t>(avail, size));
        os::Seek(cachedHandle, static_cast<int64_t>(localOff), 0);
        uint32_t br = 0;
        os::Read(cachedHandle, buf.data(), static_cast<uint32_t>(firstPart), br);
        if (firstPart < size) {
            size_t secondPart = size - firstPart;
            int fileIdx2 = fileIdx + 1;
            if (fileIdx2 < static_cast<int>(canonicalManifest.size())) {
                std::string fpath2 = (std::filesystem::current_path() / canonicalManifest[fileIdx2].relPath).string();
                os::FileHandle h2 = os::OpenRead(fpath2.c_str(), false);
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
    std::vector<std::vector<HealthyBlockRef>> trackHealthyBlocks(totalTracks);

    double phase1HashTime = 0, injectHashTime = 0, decodeFeedTime = 0, recoveryTime = 0;
    uint64_t decodeFeedCalls = 0;

    const uint8_t* mappedData = nullptr;
    os::Mapping mappingFeed;
    bool hasMapping = false;
    std::ifstream fallbackFile;
    std::mutex fallbackMutex;

    if (!resolvedDamagedPath.empty()) {
        Progress prog(totalBlocks, "Analysis");

        bool mappingUsed = false;
        os::Mapping mapping;
        const uint8_t* fileData = nullptr;

        if (os::MapRead(resolvedDamagedPath.c_str(), mapping)) {
            fileData = static_cast<const uint8_t*>(mapping.data);
            if (fileData) {
                mappingUsed = true;
                damagedBlockHashes.resize(totalBlocks);
                blockCorrupted.assign(totalBlocks, 0);

                auto tHashPhase1 = std::chrono::steady_clock::now();
                {
                    unsigned int numThreads = std::thread::hardware_concurrency();
                    if (numThreads == 0) numThreads = 4;
                    uint64_t chunkSize = (totalBlocks + numThreads - 1) / numThreads;
                    std::vector<std::future<void>> hashFutures;
                    for (uint64_t b = 0; b < totalBlocks; b += chunkSize) {
                        uint64_t end = (b + chunkSize > totalBlocks) ? totalBlocks : b + chunkSize;
                        hashFutures.push_back(std::async(std::launch::async, [&, b, end]() {
                            for (uint64_t i = b; i < end; ++i) {
                                uint64_t offset = i * globalMeta.blockSize;
                                size_t currentBlockSize = (offset + globalMeta.blockSize <= globalMeta.originalFileSize)
                                    ? globalMeta.blockSize
                                    : (globalMeta.originalFileSize - offset);
                                damagedBlockHashes[i] = hashBlock(fileData + offset, currentBlockSize);
                                prog.tick();
                            }
                        }));
                    }
                    for (auto& f : hashFutures) f.get();
                }
                phase1HashTime = elapsedSecsSince(tHashPhase1);

                for (uint64_t i = 0; i < totalBlocks; ++i) {
                    uint16_t trackId = i % totalTracks;
                    uint64_t offset = i * globalMeta.blockSize;
                    size_t currentBlockSize = (offset + globalMeta.blockSize <= globalMeta.originalFileSize)
                        ? globalMeta.blockSize : (globalMeta.originalFileSize - offset);
                    uint64_t expectedHash = (i < referenceHashes.size()) ? referenceHashes[i] : 0;
                    if (damagedBlockHashes[i] == expectedHash) {
                        trackHealthyBlocks[trackId].push_back({static_cast<uint32_t>(i / totalTracks), offset, static_cast<uint32_t>(currentBlockSize), 0, offset});
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
                std::vector<uint8_t> blockBuffer(globalMeta.blockSize);
                blockCorrupted.assign(totalBlocks, 0);
                for (uint64_t i = 0; i < totalBlocks; ++i) {
                    damagedFile.read(reinterpret_cast<char*>(blockBuffer.data()), globalMeta.blockSize);
                    size_t bytesRead = damagedFile.gcount();
                    uint64_t calculatedHash = hashBlock(blockBuffer.data(), bytesRead);
                    prog.tick();
                    uint16_t trackId = i % totalTracks;
                    uint64_t expectedHash = (i < referenceHashes.size()) ? referenceHashes[i] : 0;
                    if (calculatedHash == expectedHash) {
                        trackHealthyBlocks[trackId].push_back({static_cast<uint32_t>(i / totalTracks), i * globalMeta.blockSize, static_cast<uint32_t>(bytesRead), 0, i * globalMeta.blockSize});
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
    } else if (!canonicalManifest.empty()) {
        blockCorrupted.assign(totalBlocks, 0);
        std::cout << "Analyzing local files for healthy blocks...\n";
        corruptedBlocksCount = 0;

        Progress prog(totalBlocks, "Analysis");
        auto tHashPhase1 = std::chrono::steady_clock::now();
        std::vector<uint8_t> composeBuf(globalMeta.blockSize);
        int curFileIdx = -1;
        os::FileHandle hCurFile = os::InvalidHandle();

        for (uint64_t i = 0; i < totalBlocks; ++i) {
            uint64_t offset = i * globalMeta.blockSize;
            size_t currentBlockSize = (offset + globalMeta.blockSize <= globalMeta.originalFileSize)
                ? globalMeta.blockSize : (globalMeta.originalFileSize - offset);

            readBlockFromFiles(offset, static_cast<uint32_t>(currentBlockSize), composeBuf, curFileIdx, hCurFile);

            uint64_t h = hashBlock(composeBuf.data(), currentBlockSize);
            prog.tick();
            uint16_t trackId = i % totalTracks;
            uint64_t expectedHash = (i < referenceHashes.size()) ? referenceHashes[i] : 0;
            if (h == expectedHash) {
                trackHealthyBlocks[trackId].push_back({static_cast<uint32_t>(i / totalTracks), offset, static_cast<uint32_t>(currentBlockSize), 0, offset});
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
        parityIn.close();
        if (!resolvedDamagedPath.empty()) {
            std::cout << "No corruption detected; file is intact.\n";
        } else {
            std::cout << "No corruption detected.\n";
        }
        std::cout << "File is already intact.";
        showElapsed(); std::cout << "\n";
        return;
    }

    parityIn.close();
    std::cout << "Injecting parity packets...\n";

    // ── Parse parity packets from primary + supplemental archives ──
    struct ParsedPacket {
        uint32_t fountainId;
        const uint8_t* payload;
        uint32_t payloadSize;
        uint32_t blockSequence;
        uint64_t payloadHash;
        uint64_t expectedBlockHash;
    };
    std::vector<std::vector<ParsedPacket>> trackPackets(totalTracks);
    size_t parityPacketsSeen = 0;

    struct ArchiveMapping {
        os::Mapping mapping;
        std::string path;
    };
    std::vector<ArchiveMapping> archiveMappings;
    uint64_t primaryHeaderBlockSize = headerBlockSize;

    auto parsePacketsFromArchive = [&](const std::string& archPath, bool isPrimary) -> bool {
        os::Mapping mapping;
        if (!os::MapRead(archPath.c_str(), mapping)) {
            if (isPrimary) std::cerr << "Error: Cannot memory-map parity file: " << archPath << "\n";
            else std::cerr << "Warning: Cannot memory-map supplement: " << archPath << "\n";
            return false;
        }
        const uint8_t* base = static_cast<const uint8_t*>(mapping.data);
        uint64_t fileSz = mapping.size;

        // Find the data section (between the two headers)
        uint64_t hdrBlockSz = 0;
        // Read footer to get header block size
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

            if (pkt.matrixTrack >= totalTracks || pkt.blockSize > 2 * 1024 * 1024) {
                bufOff += static_cast<size_t>(pkt.blockSize);
                continue;
            }

            if (bufOff + pkt.blockSize > dataSz) break;

            trackPackets[pkt.matrixTrack].push_back({pkt.fountainId, dataPtr + bufOff, pkt.blockSize, pkt.blockSequence, pkt.payloadHash, pkt.expectedBlockHash});
            bufOff += pkt.blockSize;
            parityPacketsSeen++;
        }

        archiveMappings.push_back({std::move(mapping), archPath});
        return true;
    };

    // Parse primary archive
    parsePacketsFromArchive(parityPath, true);

    // Scan for supplemental archives (basename.pNN+NN.whpar)
    {
        auto archDir = std::filesystem::path(parityPath).parent_path();
        if (archDir.empty()) archDir = ".";
        std::string primaryFn = std::filesystem::path(parityPath).filename().string();
        // Derive base name: strip .pNN+NN.whpar or .pNN.whpar
        std::string baseName = primaryFn;
        {
            size_t extDot = baseName.rfind('.');
            if (extDot != std::string::npos) baseName = baseName.substr(0, extDot);
            // Remove supplemental suffix if present
            size_t plusPos = baseName.find('+');
            if (plusPos != std::string::npos) baseName = baseName.substr(0, plusPos);
            // Remove .pNN
            size_t ppos = baseName.rfind(".p");
            if (ppos != std::string::npos && ppos > 0) baseName = baseName.substr(0, ppos);
        }

        if (std::filesystem::is_directory(archDir)) {
            for (auto& entry : std::filesystem::directory_iterator(archDir)) {
                if (!std::filesystem::is_regular_file(entry.path())) continue;
                std::string name = entry.path().filename().string();
                // Must end with .whpar, start with baseName, and not be the primary
                if (name.size() < 7) continue;
                std::string ext = name.substr(name.size() - 6);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".whpar") continue;
                if (name.find(baseName) != 0) continue;
                // Canonicalize paths to compare
                std::string absEntry = std::filesystem::absolute(entry.path()).u8string();
                std::string absPrimary = std::filesystem::absolute(parityPath).u8string();
                if (absEntry == absPrimary) continue;

                size_t plusPos = name.find('+');
                if (plusPos == std::string::npos) continue; // only scan supplements (have + in name)
                if (plusPos >= name.size() - 1) continue;

                std::cout << "Found supplemental archive: " << name << "\n";
                parsePacketsFromArchive(entry.path().u8string(), false);
            }
        }
    }

    auto tInjectHash = std::chrono::steady_clock::now();
    std::vector<std::vector<uint8_t>> trackPacketValid(totalTracks);
    {
        size_t totalParityPackets = 0;
        for (uint16_t t = 0; t < totalTracks; ++t) {
            trackPacketValid[t].resize(trackPackets[t].size(), false);
            totalParityPackets += trackPackets[t].size();
        }

        unsigned int numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) numThreads = 4;

        struct PacketRef { uint16_t track; size_t idx; const uint8_t* data; uint32_t size; uint64_t expectedHash; };
        std::vector<PacketRef> allRefs;
        allRefs.reserve(totalParityPackets);
        for (uint16_t t = 0; t < totalTracks; ++t) {
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
                    trackPacketValid[ref.track][ref.idx] = (hashBlock(ref.data, ref.size) == ref.expectedHash);
                }
            }));
        }
        for (auto& f : hashFutures) f.get();
    }
    injectHashTime = elapsedSecsSince(tInjectHash);

    std::cout << "Parity packets: " << parityPacketsSeen << " seen\n";

    auto tDecodeFeed = std::chrono::steady_clock::now();

    std::vector<std::vector<uint8_t>> recoveredTracks(totalTracks);
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
        if (rs.size() < bs.size()) return false;
        if (rs.compare(0, bs.size(), bs) != 0) return false;
        if (rs.size() > bs.size() && rs[bs.size()] != '/' && rs[bs.size()] != '\\') return false;
        return true;
    };

    std::string outFile;
    bool inPlace = false;
    if (!canonicalManifest.empty() && canonicalManifest.size() == 1) {
        auto& me = canonicalManifest[0];
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
    uint64_t totalMemMB = os::TotalMemoryMB();
    if (totalMemMB > 0 && totalMemMB >= 40ULL * 1024) BATCH_SIZE = 3;
    if (BATCH_SIZE > totalTracks) BATCH_SIZE = totalTracks;

    for (uint16_t batchStart = 0; batchStart < totalTracks && executionSuccess; batchStart += BATCH_SIZE) {
        uint16_t batchEnd = (batchStart + BATCH_SIZE < totalTracks) ? (batchStart + BATCH_SIZE) : totalTracks;
        std::vector<std::future<void>> futures;

        for (uint16_t t = batchStart; t < batchEnd; ++t) {
            std::cout << "Track " << t << ": " << trackHealthyBlocks[t].size() << " healthy blocks, "
                      << trackPackets[t].size() << " parity packets\n";

            futures.push_back(std::async(std::launch::async, [&, t]() {
                WirehairCodec dec = wirehair_decoder_create(nullptr, trackSizes[t], globalMeta.blockSize);
                if (!dec) {
                    std::cerr << "Failed to create decoder for track " << t << "\n";
                    executionSuccess = false;
                    return;
                }

                bool solved = false;
                uint64_t localCalls = 0;
                std::vector<uint8_t> readBuf(globalMeta.blockSize);
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

                recoveredTracks[t].resize(trackSizes[t]);
                WirehairResult rr = wirehair_recover(dec, recoveredTracks[t].data(), trackSizes[t]);
                wirehair_free(dec);

                if (rr != Wirehair_Success) {
                    std::cerr << "Recovery failed on track " << t << " (result=" << static_cast<int>(rr) << ")\n";
                    executionSuccess = false;
                } else if (inPlace) {
                    os::FileHandle hTrackOut = os::OpenReadWrite(resolvedDamagedPath.c_str());
                    if (hTrackOut != os::InvalidHandle()) {
                        size_t trackByteOffset = 0;
                        for (uint64_t g = t; g < totalBlocks; g += totalTracks) {
                            uint64_t fileOff = g * globalMeta.blockSize;
                            size_t blockSz = (fileOff + globalMeta.blockSize <= globalMeta.originalFileSize)
                                ? globalMeta.blockSize
                                : (globalMeta.originalFileSize - fileOff);
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
            }));
        }

        for (auto& f : futures) f.get();
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
        if (!canonicalManifest.empty() && canonicalManifest.size() == 1) {
            if (inPlace) {
                std::cout << "Restored in-place: " << outFile << "\n";
            } else {
                std::vector<uint8_t> fullMessage(static_cast<size_t>(globalMeta.originalFileSize));
                std::vector<size_t> trackOffsets(totalTracks, 0);
                for (uint64_t i = 0; i < totalBlocks; ++i) {
                    uint16_t trackId = i % totalTracks;
                    uint64_t offset = i * globalMeta.blockSize;
                    size_t currentBlockSize = (offset + globalMeta.blockSize <= globalMeta.originalFileSize)
                        ? globalMeta.blockSize
                        : (globalMeta.originalFileSize - offset);
                    memcpy(fullMessage.data() + offset, recoveredTracks[trackId].data() + trackOffsets[trackId], currentBlockSize);
                    trackOffsets[trackId] += currentBlockSize;
                }

                std::ofstream out(outFile, std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(fullMessage.data()), fullMessage.size());
                    out.close();
                }
                std::cout << "Restored to: " << outFile << "\n";
            }
        } else if (!canonicalManifest.empty()) {
            std::vector<uint8_t> fullMessage(static_cast<size_t>(globalMeta.originalFileSize));
            std::vector<size_t> trackOffsets(totalTracks, 0);
            for (uint64_t i = 0; i < totalBlocks; ++i) {
                uint16_t trackId = i % totalTracks;
                uint64_t offset = i * globalMeta.blockSize;
                size_t currentBlockSize = (offset + globalMeta.blockSize <= globalMeta.originalFileSize)
                    ? globalMeta.blockSize
                    : (globalMeta.originalFileSize - offset);
                memcpy(fullMessage.data() + offset, recoveredTracks[trackId].data() + trackOffsets[trackId], currentBlockSize);
                trackOffsets[trackId] += currentBlockSize;
            }

            uint64_t cumulativeOffset = 0;
            for (auto& me : canonicalManifest) {
                std::filesystem::path outPath(outputPath);
                if (!rejectPathTraversal(outPath, me.relPath)) {
                    std::cerr << "Error: manifest path '" << me.relPath << "' escapes output directory.\n";
                    return;
                }
                auto parent = outPath / std::filesystem::path(me.relPath).parent_path();
                std::filesystem::create_directories(parent);
                std::string filePath = (outPath / me.relPath).string();

                std::ofstream outFile(filePath, std::ios::binary);
                if (outFile) {
                    outFile.write(reinterpret_cast<const char*>(fullMessage.data() + cumulativeOffset), static_cast<std::streamsize>(me.fileSize));
                    if (!outFile.good()) {
                        std::cerr << "Error: Failed to write restored file (disk full?).\n";
                        return;
                    }
                    outFile.close();
                    os::SetAttributes(filePath.c_str(), me.attributes);
                    if (me.mtime > 0) os::SetModificationTime(filePath.c_str(), me.mtime);
                }
                cumulativeOffset += me.fileSize;
            }
            std::cout << "Restored " << canonicalManifest.size() << " file(s) to: " << outputPath << "\n";
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
        uint64_t totalHealthy = totalBlocks - corruptedBlocksCount;
        uint64_t totalParity = parityPacketsSeen;
        uint64_t needEstimate = 0;
        for (uint16_t t = 0; t < totalTracks; ++t) {
            uint64_t h = trackHealthyBlocks[t].size();
            uint64_t p = trackPackets[t].size();
            if (p < h + 1) needEstimate += (h + 1) - p;
        }
        int extraPct10 = static_cast<int>((needEstimate * 1000.0 / totalBlocks) + 0.5);
        std::cout << "FAILURE: Repair incomplete. Have " << totalBlocks << " data blocks, "
                  << totalParity << " parity packets.\n"
                  << "         Need ~" << needEstimate << " more packets (≈"
                  << (extraPct10 / 10) << "." << (extraPct10 % 10) << "% additional overhead).";
    }
    showElapsed(); std::cout << "\n";
}
