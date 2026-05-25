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

#include "create.h"
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
#include <filesystem>
#include <chrono>
#include <future>
#include <atomic>
#include <thread>
#include <mutex>
#include "portability.h"

static void writeArchiveHeader(
    std::ofstream& out,
    uint64_t fileSize,
    uint32_t blockSize,
    uint16_t totalTracks,
    bool useXxh64,
    const std::vector<uint64_t>& originalBlockHashes,
    const std::vector<ManifestEntry>& manifest,
    uint32_t stripeCount,
    uint64_t blocksPerStripe
) {
    std::vector<uint8_t> buf;
    auto appendBytes = [&](const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };

    PacketHeader mainHeader;
    mainHeader.magic = WHPAR_MAGIC;
    mainHeader.originalFileSize = fileSize;
    mainHeader.blockSize = blockSize;
    mainHeader.matrixTrack = useXxh64 ? 1 : 0;
    mainHeader.fountainId = totalTracks;
    mainHeader.payloadHash = 0;
    mainHeader.blockSequence = stripeCount;
    mainHeader.expectedBlockHash = blocksPerStripe;

    PacketHeader hdrLE = mainHeader;
    hdrLE.magic = cpu_to_le32(hdrLE.magic);
    hdrLE.originalFileSize = cpu_to_le64(hdrLE.originalFileSize);
    hdrLE.blockSize = cpu_to_le32(hdrLE.blockSize);
    hdrLE.matrixTrack = cpu_to_le16(hdrLE.matrixTrack);
    hdrLE.fountainId = cpu_to_le32(hdrLE.fountainId);
    hdrLE.payloadHash = cpu_to_le64(hdrLE.payloadHash);
    hdrLE.blockSequence = cpu_to_le32(hdrLE.blockSequence);
    hdrLE.expectedBlockHash = cpu_to_le64(hdrLE.expectedBlockHash);
    appendBytes(&hdrLE, sizeof(hdrLE));

    uint32_t hashCount = cpu_to_le32(static_cast<uint32_t>(originalBlockHashes.size()));
    appendBytes(&hashCount, sizeof(hashCount));
    for (auto h : originalBlockHashes) {
        uint64_t hLE = cpu_to_le64(h);
        appendBytes(&hLE, sizeof(hLE));
    }

    uint8_t hasManifest = static_cast<uint8_t>(!manifest.empty());
    buf.push_back(hasManifest);
    if (hasManifest) {
        uint32_t fileCount = cpu_to_le32(static_cast<uint32_t>(manifest.size()));
        appendBytes(&fileCount, sizeof(fileCount));
        for (auto& e : manifest) {
            uint32_t pathLen = cpu_to_le32(static_cast<uint32_t>(e.relPath.size()));
            appendBytes(&pathLen, sizeof(pathLen));
            appendBytes(e.relPath.data(), e.relPath.size());
            uint64_t fsLE = cpu_to_le64(e.fileSize);
            appendBytes(&fsLE, sizeof(fsLE));
            uint64_t mtLE = cpu_to_le64(e.mtime);
            appendBytes(&mtLE, sizeof(mtLE));
            uint32_t attrLE = cpu_to_le32(e.attributes);
            appendBytes(&attrLE, sizeof(attrLE));
        }
    }

    uint64_t eccHash = XXH3_64bits(buf.data(), buf.size());
    out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    writeU64LE(out, eccHash);
}

// Encodes parity for one stripe (or the full dataset with stripeIndex=0) and writes to output.
// If encoderPool is non-empty, encoders are reused across calls (avoiding create/destroy overhead).
// Caller must wirehair_free() each non-null encoderPool entry after the last stripe.
static bool encodeTrackParityToFile(
    std::ofstream& finalOut,
    uint64_t fileSize,
    uint32_t blockSize,
    uint16_t totalTracks,
    bool useXxh64,
    const std::vector<uint64_t>& originalBlockHashes,
    const std::vector<std::vector<uint8_t>>& trackBufs,
    const std::vector<uint32_t>& trackBlockCounts,
    float overhead,
    uint32_t stripeIndex,
    uint64_t stripeStartBlock,
    Progress& prog,
    std::mutex& progMutex,
    std::atomic<bool>& encodeFailed,
    std::vector<WirehairCodec>& encoderPool
) {
    std::vector<uint32_t> trackParityCount(totalTracks, 0);
    double carry = 0.0;
    for (uint16_t t = 0; t < totalTracks; ++t) {
        double exact = trackBlockCounts[t] * overhead + carry;
        uint32_t blocks = static_cast<uint32_t>(exact);
        carry = exact - blocks;
        if (blocks == 0 && trackBlockCounts[t] > 0) blocks = 1;
        trackParityCount[t] = blocks;
    }

    unsigned int BATCH_SIZE = 2;
    uint64_t totalMB = os::TotalMemoryMB();
    if (totalMB >= 40960) BATCH_SIZE = 3;
    if (BATCH_SIZE > totalTracks) BATCH_SIZE = totalTracks;

    // Pooled mode: keep encoders alive across calls (striped path).
    // Non-pooled mode (encoderPool empty): create/destroy per call.
    bool pooled = !encoderPool.empty();
    if (!pooled)
        encoderPool.assign(totalTracks, nullptr);

    std::vector<std::vector<uint8_t>> trackParityData(totalTracks);

    for (uint16_t batchStart = 0; batchStart < totalTracks && !encodeFailed; batchStart += BATCH_SIZE) {
        uint16_t batchEnd = (batchStart + BATCH_SIZE < totalTracks) ? (batchStart + BATCH_SIZE) : totalTracks;
        std::vector<std::future<void>> futures;

        for (uint16_t t = batchStart; t < batchEnd; ++t) {
            if (trackBlockCounts[t] == 0) continue;

            futures.push_back(std::async(std::launch::async, [&, t]() {
                WirehairCodec enc = encoderPool[t];
                try {
                    const std::vector<uint8_t>& trackBuf = trackBufs[t];

                    enc = wirehair_encoder_create(enc, trackBuf.data(), trackBuf.size(), blockSize);
                    encoderPool[t] = enc;
                    if (!enc) {
                        std::lock_guard<std::mutex> lock(progMutex);
                        std::cerr << "Error creating encoder for track " << t << "\n";
                        encodeFailed = true;
                        return;
                    }

                    uint32_t trackBlocks = static_cast<uint32_t>((trackBuf.size() + blockSize - 1) / blockSize);
                    uint32_t parityBlocksToCreate = trackParityCount[t];

                    std::vector<uint8_t> localEncodeBuf(blockSize);
                    std::vector<uint8_t> localBuf;
                    localBuf.reserve(parityBlocksToCreate * (sizeof(PacketHeader) + blockSize));

                    for (uint32_t fid = 0; fid < parityBlocksToCreate; ++fid) {
                        uint32_t bytesWritten = 0;
                        uint32_t blockId = trackBlocks + fid;
                        WirehairResult res = wirehair_encode(enc, blockId, localEncodeBuf.data(), blockSize, &bytesWritten);

                        {
                            std::lock_guard<std::mutex> lock(progMutex);
                            prog.tick();
                        }

                        if (res == Wirehair_Success) {
                            PacketHeader header;
                            header.magic = WHPAR_PKT_MAGIC;
                            header.originalFileSize = fileSize;
                            header.blockSize = bytesWritten;
                            header.matrixTrack = t;
                            header.fountainId = blockId;
                            header.payloadHash = useXxh64 ? XXH3_64bits(localEncodeBuf.data(), bytesWritten) : XXH32(localEncodeBuf.data(), bytesWritten, 0);

                            uint32_t targetLocalIndex = fid % trackBlocks;
                            uint32_t targetGlobalIndex = static_cast<uint32_t>(stripeStartBlock) + targetLocalIndex * totalTracks + t;
                            if (targetGlobalIndex >= originalBlockHashes.size())
                                targetGlobalIndex = static_cast<uint32_t>(originalBlockHashes.size() - 1);
                            header.blockSequence = stripeIndex;
                            header.expectedBlockHash = originalBlockHashes[targetGlobalIndex];

                            auto oldSize = localBuf.size();
                            localBuf.resize(oldSize + sizeof(header) + bytesWritten);

                            PacketHeader headerLE = header;
                            headerLE.magic = cpu_to_le32(headerLE.magic);
                            headerLE.originalFileSize = cpu_to_le64(headerLE.originalFileSize);
                            headerLE.blockSize = cpu_to_le32(headerLE.blockSize);
                            headerLE.matrixTrack = cpu_to_le16(headerLE.matrixTrack);
                            headerLE.fountainId = cpu_to_le32(headerLE.fountainId);
                            headerLE.payloadHash = cpu_to_le64(headerLE.payloadHash);
                            headerLE.blockSequence = cpu_to_le32(headerLE.blockSequence);
                            headerLE.expectedBlockHash = cpu_to_le64(headerLE.expectedBlockHash);

                            memcpy(localBuf.data() + oldSize, &headerLE, sizeof(headerLE));
                            memcpy(localBuf.data() + oldSize + sizeof(header), localEncodeBuf.data(), bytesWritten);
                        }
                    }

                    if (!pooled) {
                        wirehair_free(enc);
                        encoderPool[t] = nullptr;
                    }
                    trackParityData[t] = std::move(localBuf);
                } catch (...) {
                    if (enc) {
                        wirehair_free(enc);
                        encoderPool[t] = nullptr;
                    }
                    throw;
                }
            }));
        }

        for (auto& f : futures) f.get();

        if (encodeFailed) break;

        for (uint16_t t = batchStart; t < batchEnd; ++t) {
            if (!trackParityData[t].empty()) {
                finalOut.write(reinterpret_cast<const char*>(trackParityData[t].data()), trackParityData[t].size());
                std::vector<uint8_t>().swap(trackParityData[t]);
            }
        }
    }

    return !encodeFailed;
}

static bool writeParityOutput(
    const std::string& parityPath,
    uint64_t fileSize,
    uint32_t blockSize,
    uint16_t totalTracks,
    bool useXxh64,
    const std::vector<uint64_t>& originalBlockHashes,
    const std::vector<ManifestEntry>& manifest,
    const std::vector<std::vector<uint8_t>>& trackBufs,
    const std::vector<uint32_t>& trackBlockCounts,
    float overhead,
    const std::string& tempFilePath,
    bool debug
) {
    auto startTime = std::chrono::steady_clock::now();

    std::vector<uint32_t> trackParityCount(totalTracks, 0);
    double carry = 0.0;
    for (uint16_t t = 0; t < totalTracks; ++t) {
        double exact = trackBlockCounts[t] * overhead + carry;
        uint32_t blocks = static_cast<uint32_t>(exact);
        carry = exact - blocks;
        if (blocks == 0 && trackBlockCounts[t] > 0) blocks = 1;
        trackParityCount[t] = blocks;
    }

    uint32_t totalParityBlocks = 0;
    for (uint16_t t = 0; t < totalTracks; ++t) totalParityBlocks += trackParityCount[t];

    std::ofstream finalOut(parityPath, std::ios::binary);
    if (!finalOut) {
        std::cerr << "Error: Cannot create parity file '" << parityPath << "'.\n";
        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        return false;
    }
    // stripeCount=0, blocksPerStripe=0 for original non-striped archives
    writeArchiveHeader(finalOut, fileSize, blockSize, totalTracks, useXxh64,
                       originalBlockHashes, manifest, 0, 0);
    std::streampos headerEndPos = finalOut.tellp();
    if (headerEndPos < 0) return false;

    Progress prog(totalParityBlocks, "Parity");
    std::mutex progMutex;
    std::atomic<bool> encodeFailed{false};

    std::vector<WirehairCodec> emptyPool;
    encodeTrackParityToFile(finalOut, fileSize, blockSize, totalTracks, useXxh64,
                            originalBlockHashes, trackBufs, trackBlockCounts,
                            overhead, 0, 0, prog, progMutex, encodeFailed, emptyPool);

    prog.done();

    if (encodeFailed) {
        finalOut.close();
        os::RemoveFile(parityPath.c_str());
        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        return false;
    }

    writeArchiveHeader(finalOut, fileSize, blockSize, totalTracks, useXxh64,
                       originalBlockHashes, manifest, 0, 0);
    uint64_t hbs = static_cast<uint64_t>(static_cast<std::streamoff>(headerEndPos));
    writeU64LE(finalOut, hbs);
    finalOut.close();

    double elapsed = elapsedSecsSince(startTime);
    std::cout << "Done! Parity generation completed successfully.";
    if (elapsed >= 60.0) {
        int mins = static_cast<int>(elapsed / 60);
        double secs = elapsed - mins * 60;
        std::cout << " (" << mins << "m " << secs << "s)";
    } else {
        std::cout << " (" << elapsed << "s)";
    }
    std::cout << std::endl;

    return true;
}

struct BatchStripe {
    uint64_t startBlock, endBlock;
    std::vector<uint32_t> trackBlockCounts;
    std::vector<uint32_t> trackParityCount;
    std::vector<uint8_t> rawData;
};

static void processAllStripeBatches(
    std::ofstream& finalOut,
    uint64_t fileSize, uint32_t blockSize, uint16_t totalTracks, bool useXxh64,
    const std::vector<uint64_t>& blockHashes,
    uint64_t stripeCount, uint64_t blocksPerStripe, uint64_t totalBlocks,
    float overhead,
    os::FileHandle hReadFile,
    const std::vector<uint8_t>& fallbackData,
    unsigned int STRIPE_BATCH,
    Progress& prog, std::mutex& progMutex,
    std::atomic<bool>& encodeFailed
) {
    for (uint64_t batchStart = 0; batchStart < stripeCount && !encodeFailed; batchStart += STRIPE_BATCH) {
        uint64_t batchEnd = std::min(batchStart + STRIPE_BATCH, stripeCount);

        // Phase 1: Pre-read stripe data for this batch (sequential I/O)
        std::vector<BatchStripe> batchData(batchEnd - batchStart);
        for (uint64_t s = batchStart; s < batchEnd; ++s) {
            auto& bd = batchData[s - batchStart];
            bd.startBlock = s * blocksPerStripe;
            bd.endBlock = std::min(bd.startBlock + blocksPerStripe, totalBlocks);
            bd.trackBlockCounts.assign(totalTracks, 0);
            for (uint64_t i = bd.startBlock; i < bd.endBlock; ++i)
                bd.trackBlockCounts[i % totalTracks]++;
            {
                double carry = 0.0;
                bd.trackParityCount.resize(totalTracks);
                for (uint16_t t = 0; t < totalTracks; ++t) {
                    double exact = bd.trackBlockCounts[t] * overhead + carry;
                    uint32_t blocks = static_cast<uint32_t>(exact);
                    carry = exact - blocks;
                    if (blocks == 0 && bd.trackBlockCounts[t] > 0) blocks = 1;
                    bd.trackParityCount[t] = blocks;
                }
            }

            if (hReadFile != os::InvalidHandle()) {
                uint64_t stripeByteStart = bd.startBlock * blockSize;
                uint64_t stripeByteEnd = std::min(bd.endBlock * blockSize, fileSize);
                size_t stripeBytes = static_cast<size_t>(stripeByteEnd - stripeByteStart);
                bd.rawData.resize(stripeBytes);
                if (!os::Seek(hReadFile, static_cast<int64_t>(stripeByteStart), 0)) {
                    std::cerr << "Error: Seek failed for stripe " << s << "\n";
                    encodeFailed = true; break;
                }
                uint32_t bytesRead = 0;
                if (!os::Read(hReadFile, bd.rawData.data(), static_cast<uint32_t>(stripeBytes), bytesRead)) {
                    std::cerr << "Error: Read failed for stripe " << s << "\n";
                    encodeFailed = true; break;
                }
            } else if (!fallbackData.empty()) {
                uint64_t stripeBytes = (bd.endBlock - bd.startBlock) * blockSize;
                bd.rawData.resize(stripeBytes);
                for (uint64_t i = bd.startBlock; i < bd.endBlock; ++i) {
                    uint64_t offset = i * blockSize;
                    uint64_t localOff = (i - bd.startBlock) * blockSize;
                    size_t sz = (offset + blockSize <= fileSize) ? blockSize : static_cast<size_t>(fileSize - offset);
                    memcpy(bd.rawData.data() + localOff, fallbackData.data() + offset, sz);
                }
            }
        }
        if (encodeFailed) break;

        // Phase 2: Parallel encode
        std::vector<std::future<std::vector<std::vector<uint8_t>>>> futures;
        for (uint64_t s = batchStart; s < batchEnd; ++s) {
            auto encodeFn = std::function<std::vector<std::vector<uint8_t>>()>([&batchData, s, batchStart, &encodeFailed, &progMutex, &prog, fileSize, blockSize, totalTracks, useXxh64, &blockHashes]() -> std::vector<std::vector<uint8_t>> {
                auto& bd = batchData[s - batchStart];
                std::vector<WirehairCodec> localPool(totalTracks, nullptr);
                std::vector<std::vector<uint8_t>> result(totalTracks);

                try {
                    // Deinterleave
                    std::vector<std::vector<uint8_t>> trackBufs(totalTracks);
                    for (uint16_t t = 0; t < totalTracks; ++t)
                        trackBufs[t].reserve(bd.trackBlockCounts[t] * blockSize);
                    for (uint64_t i = bd.startBlock; i < bd.endBlock; ++i) {
                        uint64_t localOff = (i - bd.startBlock) * blockSize;
                        size_t sz = (i * blockSize + blockSize <= fileSize)
                            ? blockSize : static_cast<size_t>(fileSize - i * blockSize);
                        uint16_t t = static_cast<uint16_t>(i % totalTracks);
                        trackBufs[t].insert(trackBufs[t].end(),
                            bd.rawData.data() + localOff,
                            bd.rawData.data() + localOff + sz);
                    }

                    // Encode each track
                    for (uint16_t t = 0; t < totalTracks; ++t) {
                        if (bd.trackBlockCounts[t] == 0) continue;
                        WirehairCodec enc = wirehair_encoder_create(nullptr,
                            trackBufs[t].data(), trackBufs[t].size(), blockSize);
                        localPool[t] = enc;
                        if (!enc) {
                            std::lock_guard<std::mutex> lock(progMutex);
                            std::cerr << "Error creating encoder for track " << t << " stripe " << s << "\n";
                            encodeFailed = true;
                            return result;
                        }

                        uint32_t trackBlocks = static_cast<uint32_t>((trackBufs[t].size() + blockSize - 1) / blockSize);
                        uint32_t parityBlocksToCreate = bd.trackParityCount[t];

                        std::vector<uint8_t> encodeBuf(blockSize);
                        std::vector<uint8_t> localBuf;
                        localBuf.reserve(parityBlocksToCreate * (sizeof(PacketHeader) + blockSize));

                        for (uint32_t fid = 0; fid < parityBlocksToCreate; ++fid) {
                            uint32_t bytesWritten = 0;
                            uint32_t blockId = trackBlocks + fid;
                            WirehairResult res = wirehair_encode(enc, blockId, encodeBuf.data(), blockSize, &bytesWritten);

                            {
                                std::lock_guard<std::mutex> lock(progMutex);
                                prog.tick();
                            }

                            if (res == Wirehair_Success) {
                                PacketHeader header;
                                header.magic = WHPAR_PKT_MAGIC;
                                header.originalFileSize = fileSize;
                                header.blockSize = bytesWritten;
                                header.matrixTrack = t;
                                header.fountainId = blockId;
                                header.payloadHash = useXxh64 ? XXH3_64bits(encodeBuf.data(), bytesWritten) : XXH32(encodeBuf.data(), bytesWritten, 0);

                                uint32_t targetLocalIndex = fid % trackBlocks;
                                uint32_t targetGlobalIndex = static_cast<uint32_t>(bd.startBlock) + targetLocalIndex * totalTracks + t;
                                if (targetGlobalIndex >= blockHashes.size())
                                    targetGlobalIndex = static_cast<uint32_t>(blockHashes.size() - 1);
                                header.blockSequence = static_cast<uint32_t>(s);
                                header.expectedBlockHash = blockHashes[targetGlobalIndex];

                                auto oldSize = localBuf.size();
                                localBuf.resize(oldSize + sizeof(header) + bytesWritten);

                                PacketHeader headerLE = header;
                                headerLE.magic = cpu_to_le32(headerLE.magic);
                                headerLE.originalFileSize = cpu_to_le64(headerLE.originalFileSize);
                                headerLE.blockSize = cpu_to_le32(headerLE.blockSize);
                                headerLE.matrixTrack = cpu_to_le16(headerLE.matrixTrack);
                                headerLE.fountainId = cpu_to_le32(headerLE.fountainId);
                                headerLE.payloadHash = cpu_to_le64(headerLE.payloadHash);
                                headerLE.blockSequence = cpu_to_le32(headerLE.blockSequence);
                                headerLE.expectedBlockHash = cpu_to_le64(headerLE.expectedBlockHash);

                                memcpy(localBuf.data() + oldSize, &headerLE, sizeof(headerLE));
                                memcpy(localBuf.data() + oldSize + sizeof(header), encodeBuf.data(), bytesWritten);
                            }
                        }

                        result[t] = std::move(localBuf);
                    }
                } catch (...) {
                    encodeFailed = true;
                }

                for (auto enc : localPool) {
                    if (enc) wirehair_free(enc);
                }
                return result;
            });
            futures.push_back(std::async(std::launch::async, encodeFn));
        }

        // Phase 3: Collect and write results in stripe order
        for (uint64_t s = batchStart; s < batchEnd; ++s) {
            auto&& result = futures[s - batchStart].get();
            for (uint16_t t = 0; t < totalTracks; ++t) {
                if (!result[t].empty()) {
                    finalOut.write(reinterpret_cast<const char*>(result[t].data()), result[t].size());
                }
            }
        }
    }
}

// ────────────────────────────────────────────────────────────

void CreateParity(const std::vector<std::string>& sourcePaths, const std::string& parityPath, float overhead, bool debug, uint32_t blockSizeKB, bool useXxh64, uint32_t numJobs, bool noRecursive, uint64_t maxMemBytes) {
    auto startTime = std::chrono::steady_clock::now();

    std::vector<ManifestEntry> manifest;
    std::vector<uint8_t> allDataFallback;
    std::vector<std::filesystem::path> srcFiles;

    auto normalizeSlashes = [](std::string p) -> std::string {
        for (auto& c : p) if (c == '\\') c = '/';
        return p;
    };

    bool multiSource = sourcePaths.size() > 1;

    for (auto& sourcePath : sourcePaths) {
        std::filesystem::path src(sourcePath);
        bool isDir = std::filesystem::is_directory(src);

        if (isDir) {
            std::vector<std::filesystem::path> dirFiles;
            if (noRecursive) {
                for (auto& p : std::filesystem::directory_iterator(src)) {
                    if (std::filesystem::is_regular_file(p.path())) dirFiles.push_back(p.path());
                }
            } else {
                for (auto& p : std::filesystem::recursive_directory_iterator(src)) {
                    if (std::filesystem::is_regular_file(p.path())) dirFiles.push_back(p.path());
                }
            }
            std::sort(dirFiles.begin(), dirFiles.end());

            std::string prefix = multiSource ? (src.filename().u8string() + "/") : "";

            for (auto& f : dirFiles) {
                ManifestEntry e;
                e.relPath = normalizeSlashes(prefix + std::filesystem::relative(f, src).u8string());
                e.fileSize = std::filesystem::file_size(f);
                auto ftime = std::filesystem::last_write_time(f);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
                e.mtime = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(sctp));
                e.attributes = os::GetAttributes(f.string().c_str());
                manifest.push_back(e);
                srcFiles.push_back(f);
            }
        } else if (std::filesystem::is_regular_file(src)) {
            ManifestEntry e;
            e.relPath = normalizeSlashes(src.filename().u8string());
            e.fileSize = std::filesystem::file_size(src);
            auto ftime = std::filesystem::last_write_time(src);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
            e.mtime = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(sctp));
            e.attributes = os::GetAttributes(sourcePath.c_str());
            manifest.push_back(e);
            srcFiles.push_back(src);
        } else {
            std::cerr << "Error: source path is not a file or directory: " << sourcePath << "\n";
            return;
        }
    }

    if (manifest.empty()) {
        std::cerr << "Error: No valid source files found.\n";
        return;
    }

    uint64_t fileSize = 0;
    for (auto& me : manifest) fileSize += me.fileSize;

    uint64_t sysMemMB = os::TotalMemoryMB();
    if (sysMemMB > 0 && maxMemBytes == 0 && fileSize > sysMemMB * 1024ULL * 1024ULL) {
        std::cout << "Warning: File size (" << (fileSize / (1024ULL * 1024 * 1024))
                  << " GB) exceeds total system RAM (" << sysMemMB / 1024
                  << " GB) and --max-mem is not set.\n"
                  << "         Without --max-mem, the entire file will be loaded into memory.\n"
                  << "         Use --max-mem <size> to limit memory usage (e.g. --max-mem 4GB).\n";
    }

    if (numJobs == 0) numJobs = std::thread::hardware_concurrency();
    if (numJobs < 1) numJobs = 1;
    uint16_t totalTracks = static_cast<uint16_t>(numJobs);

    uint32_t blockSize;
    if (blockSizeKB > 0) {
        blockSize = blockSizeKB * 1024;
    } else {
        uint64_t totalTargetBlocks = static_cast<uint64_t>(totalTracks) * 2000;
        uint64_t desired = fileSize / (totalTargetBlocks > 0 ? totalTargetBlocks : 1);
        if (desired < 65536) desired = 65536;
        if (desired > 4194304) desired = 4194304;
        blockSize = static_cast<uint32_t>(desired);
        blockSize = roundUpPow2(blockSize);
        blockSize /= 2;
        if (blockSize < 1024) blockSize = 1024;
    }

    uint64_t totalBlocks = (fileSize + blockSize - 1) / blockSize;

    if (totalBlocks > UINT32_MAX) {
        std::cerr << "Error: Too many blocks (" << totalBlocks << "); maximum supported is " << UINT32_MAX << ".\n";
        return;
    }

    if (totalTracks * 2 > totalBlocks) totalTracks = static_cast<uint16_t>(totalBlocks / 2);
    if (totalTracks > totalBlocks) totalTracks = static_cast<uint16_t>(totalBlocks);
    if (totalTracks < 1) totalTracks = 1;

    std::cout << "File Size: " << (fileSize / (1024 * 1024.0)) << " MB\n";
    std::cout << "Block Size: " << (blockSize / 1024) << " KB\n";
    std::cout << "Interleaving Architecture: " << totalTracks << " parallel track(s)\n";
    std::cout << "Total Blocks: " << totalBlocks << "\n";

    // Compute stripe count
    uint64_t stripeCount = 1;
    uint64_t blocksPerStripe = totalBlocks;
    if (maxMemBytes > 0 && fileSize > maxMemBytes) {
        stripeCount = (fileSize + maxMemBytes - 1) / maxMemBytes;
        // Ensure at least 1 block per stripe
        if (stripeCount > totalBlocks) stripeCount = totalBlocks;
        if (stripeCount < 1) stripeCount = 1;
        blocksPerStripe = (totalBlocks + stripeCount - 1) / stripeCount;
        if (debug) {
            std::cout << "Stripe processing: " << stripeCount << " stripes, "
                      << blocksPerStripe << " blocks/stripe, "
                      << (blocksPerStripe * blockSize / (1024 * 1024)) << " MB/stripe\n";
        }
    }

    // Open source file(s)
    std::string tempFilePath;
    os::FileHandle hReadFile = os::InvalidHandle();
    if (srcFiles.size() == 1) {
        hReadFile = os::OpenRead(srcFiles[0].string().c_str(), true);
    } else if (!srcFiles.empty()) {
        std::string tempDir = os::TempDir();
        os::FileHandle hTempWrite = os::InvalidHandle();
        for (int attempt = 0; attempt < 100; ++attempt) {
            tempFilePath = tempDir + "whp_" + std::to_string(os::ProcessId()) + "_" + std::to_string(os::TickCount() + attempt) + ".tmp";
            hTempWrite = os::CreateNew(tempFilePath.c_str(), true);
            if (hTempWrite != os::InvalidHandle()) break;
            if (attempt == 99) { tempFilePath.clear(); break; }
        }
        if (hTempWrite == os::InvalidHandle()) {
            std::cerr << "Error: Cannot create temp file.\n";
            return;
        }
        if (hTempWrite != os::InvalidHandle()) {
            std::vector<uint8_t> copyBuf(64ULL * 1024 * 1024);
            bool writeFailed = false;
            for (auto& f : srcFiles) {
                os::FileHandle hSrc = os::OpenRead(f.string().c_str(), false);
                if (hSrc == os::InvalidHandle()) { writeFailed = true; break; }
                uint32_t br = 0;
                while (os::Read(hSrc, copyBuf.data(), static_cast<uint32_t>(copyBuf.size()), br) && br > 0) {
                    if (!os::Write(hTempWrite, copyBuf.data(), br)) { writeFailed = true; break; }
                }
                os::Close(hSrc);
                if (writeFailed) break;
            }
            os::Close(hTempWrite);
            if (!writeFailed) {
                hReadFile = os::OpenRead(tempFilePath.c_str(), true);
            }
        }
        if (hReadFile == os::InvalidHandle()) {
            allDataFallback.clear();
            for (auto& f : srcFiles) {
                std::ifstream in(f.string(), std::ios::binary);
                if (!in) continue;
                size_t sz = static_cast<size_t>(std::filesystem::file_size(f));
                size_t oldSz = allDataFallback.size();
                allDataFallback.resize(oldSz + sz);
                in.read(reinterpret_cast<char*>(allDataFallback.data() + oldSz), static_cast<std::streamsize>(sz));
            }
        }
    }

    // ── 1-track fast path (no interleaving needed) ──
    if (totalTracks == 1) {
        std::vector<uint64_t> originalBlockHashes(totalBlocks);
        std::vector<uint8_t> allData;

        {
            Progress prog(totalBlocks, "Hash");
            uint32_t blocksPerRead = static_cast<uint32_t>((64ULL * 1024 * 1024) / blockSize);
            if (blocksPerRead < 1) blocksPerRead = 1;
            size_t readBufSize = static_cast<size_t>(blocksPerRead) * blockSize;
            std::vector<uint8_t> readBuf(readBufSize);
            uint64_t blockIdx = 0;

            if (hReadFile != os::InvalidHandle()) {
                uint32_t bytesRead = 0;
                while (blockIdx < totalBlocks && os::Read(hReadFile, readBuf.data(), static_cast<uint32_t>(readBufSize), bytesRead) && bytesRead > 0) {
                    uint64_t bufOffset = 0;
                    while (bufOffset + blockSize <= bytesRead && blockIdx < totalBlocks) {
                        originalBlockHashes[blockIdx] = useXxh64 ? XXH3_64bits(readBuf.data() + bufOffset, blockSize) : XXH32(readBuf.data() + bufOffset, blockSize, 0);
                        prog.tick();
                        blockIdx++;
                        bufOffset += blockSize;
                    }
                    allData.insert(allData.end(), readBuf.data(), readBuf.data() + bytesRead);
                }
                if (blockIdx < totalBlocks) {
                    uint64_t offset = blockIdx * blockSize;
                    size_t lastBlockSize = static_cast<size_t>(fileSize - offset);
                    std::vector<uint8_t> lastBuf(lastBlockSize);
                    if (!os::Seek(hReadFile, static_cast<int64_t>(offset), 0)) {
                        std::cerr << "Error: Failed to seek in source file.\n";
                        os::Close(hReadFile);
                        return;
                    }
                    uint32_t lastRead = 0;
                    if (!os::Read(hReadFile, lastBuf.data(), static_cast<uint32_t>(lastBlockSize), lastRead)) {
                        std::cerr << "Error: Failed to read last block from source file.\n";
                        os::Close(hReadFile);
                        return;
                    }
                    allData.insert(allData.end(), lastBuf.begin(), lastBuf.end());
                    originalBlockHashes[blockIdx] = useXxh64 ? XXH3_64bits(lastBuf.data(), lastBlockSize) : XXH32(lastBuf.data(), lastBlockSize, 0);
                    prog.tick();
                }
                os::Close(hReadFile);
                hReadFile = os::InvalidHandle();
            } else if (!allDataFallback.empty()) {
                allData = std::move(allDataFallback);
                for (uint64_t i = 0; i < totalBlocks; ++i) {
                    uint64_t offset = i * blockSize;
                    size_t currentBlockSize = (offset + blockSize <= fileSize) ? blockSize : static_cast<size_t>(fileSize - offset);
                    originalBlockHashes[i] = useXxh64 ? XXH3_64bits(allData.data() + offset, currentBlockSize) : XXH32(allData.data() + offset, currentBlockSize, 0);
                    prog.tick();
                }
            }
            prog.done();
        }

        std::vector<uint8_t>().swap(allDataFallback);

        std::vector<uint32_t> trackBlockCounts = {static_cast<uint32_t>(totalBlocks)};
        std::vector<std::vector<uint8_t>> trackBufs(1);
        trackBufs[0] = std::move(allData);

        bool ok = writeParityOutput(
            parityPath, fileSize, blockSize, 1, useXxh64,
            originalBlockHashes, manifest, trackBufs, trackBlockCounts,
            overhead, tempFilePath, debug
        );

        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        (void)ok;
        return;
    }

    // ── Non-stripe path (default, unchanged) ──
    if (stripeCount <= 1) {
        std::vector<uint64_t> originalBlockHashes(totalBlocks);
        std::vector<uint32_t> trackBlockCounts(totalTracks, 0);
        for (uint64_t i = 0; i < totalBlocks; ++i) {
            trackBlockCounts[i % totalTracks]++;
        }

        std::vector<std::vector<uint8_t>> trackBufs(totalTracks);
        for (uint16_t t = 0; t < totalTracks; ++t) {
            trackBufs[t].reserve(trackBlockCounts[t] * blockSize);
        }

        {
            Progress prog(totalBlocks, "Hash");
            uint32_t blocksPerRead = static_cast<uint32_t>((64ULL * 1024 * 1024) / blockSize);
            if (blocksPerRead < 1) blocksPerRead = 1;
            size_t readBufSize = static_cast<size_t>(blocksPerRead) * blockSize;
            std::vector<uint8_t> readBuf(readBufSize);
            uint64_t blockIdx = 0;

            if (hReadFile != os::InvalidHandle()) {
                uint32_t bytesRead = 0;
                while (blockIdx < totalBlocks && os::Read(hReadFile, readBuf.data(), static_cast<uint32_t>(readBufSize), bytesRead) && bytesRead > 0) {
                    uint64_t bufOffset = 0;
                    while (bufOffset + blockSize <= bytesRead && blockIdx < totalBlocks) {
                        uint16_t t = static_cast<uint16_t>(blockIdx % totalTracks);
                        trackBufs[t].insert(trackBufs[t].end(), readBuf.data() + bufOffset, readBuf.data() + bufOffset + blockSize);
                        originalBlockHashes[blockIdx] = useXxh64 ? XXH3_64bits(readBuf.data() + bufOffset, blockSize) : XXH32(readBuf.data() + bufOffset, blockSize, 0);
                        prog.tick();
                        blockIdx++;
                        bufOffset += blockSize;
                    }
                }
                if (blockIdx < totalBlocks) {
                    uint64_t offset = blockIdx * blockSize;
                    size_t lastBlockSize = static_cast<size_t>(fileSize - offset);
                    std::vector<uint8_t> lastBuf(lastBlockSize);
                    if (!os::Seek(hReadFile, static_cast<int64_t>(offset), 0)) {
                        std::cerr << "Error: Failed to seek in source file.\n";
                        os::Close(hReadFile);
                        return;
                    }
                    uint32_t lastRead = 0;
                    if (!os::Read(hReadFile, lastBuf.data(), static_cast<uint32_t>(lastBlockSize), lastRead)) {
                        std::cerr << "Error: Failed to read last block from source file.\n";
                        os::Close(hReadFile);
                        return;
                    }
                    uint16_t t = static_cast<uint16_t>(blockIdx % totalTracks);
                    trackBufs[t].insert(trackBufs[t].end(), lastBuf.data(), lastBuf.data() + lastBlockSize);
                    originalBlockHashes[blockIdx] = useXxh64 ? XXH3_64bits(lastBuf.data(), lastBlockSize) : XXH32(lastBuf.data(), lastBlockSize, 0);
                    prog.tick();
                }
                os::Close(hReadFile);
                hReadFile = os::InvalidHandle();
            } else if (!allDataFallback.empty()) {
                for (uint64_t i = 0; i < totalBlocks; ++i) {
                    uint64_t offset = i * blockSize;
                    size_t currentBlockSize = (offset + blockSize <= fileSize) ? blockSize : static_cast<size_t>(fileSize - offset);
                    uint16_t t = static_cast<uint16_t>(i % totalTracks);
                    trackBufs[t].insert(trackBufs[t].end(), allDataFallback.data() + offset, allDataFallback.data() + offset + currentBlockSize);
                    originalBlockHashes[i] = useXxh64 ? XXH3_64bits(allDataFallback.data() + offset, currentBlockSize) : XXH32(allDataFallback.data() + offset, currentBlockSize, 0);
                    prog.tick();
                }
            }
            prog.done();
        }
        std::vector<uint8_t>().swap(allDataFallback);

        bool ok = writeParityOutput(
            parityPath, fileSize, blockSize, totalTracks, useXxh64,
            originalBlockHashes, manifest, trackBufs, trackBlockCounts,
            overhead, tempFilePath, debug
        );

        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        (void)ok;
        return;
    }

    // ── Stripe path (--max-mem) ──

    // Phase 1: Hash pass — read entire source, compute all block hashes (no track buffers)
    std::vector<uint64_t> originalBlockHashes(totalBlocks);
    {
        Progress prog(totalBlocks, "Hash");
        uint32_t blocksPerRead = static_cast<uint32_t>((64ULL * 1024 * 1024) / blockSize);
        if (blocksPerRead < 1) blocksPerRead = 1;
        size_t readBufSize = static_cast<size_t>(blocksPerRead) * blockSize;
        std::vector<uint8_t> readBuf(readBufSize);
        uint64_t blockIdx = 0;

        if (hReadFile != os::InvalidHandle()) {
            os::Seek(hReadFile, 0, 0);
            uint32_t bytesRead = 0;
            while (blockIdx < totalBlocks && os::Read(hReadFile, readBuf.data(), static_cast<uint32_t>(readBufSize), bytesRead) && bytesRead > 0) {
                uint64_t bufOffset = 0;
                while (bufOffset + blockSize <= bytesRead && blockIdx < totalBlocks) {
                    originalBlockHashes[blockIdx] = useXxh64 ? XXH3_64bits(readBuf.data() + bufOffset, blockSize) : XXH32(readBuf.data() + bufOffset, blockSize, 0);
                    prog.tick();
                    blockIdx++;
                    bufOffset += blockSize;
                }
            }
            if (blockIdx < totalBlocks) {
                uint64_t offset = blockIdx * blockSize;
                size_t lastBlockSize = static_cast<size_t>(fileSize - offset);
                std::vector<uint8_t> lastBuf(lastBlockSize);
                os::Seek(hReadFile, static_cast<int64_t>(offset), 0);
                uint32_t lastRead = 0;
                os::Read(hReadFile, lastBuf.data(), static_cast<uint32_t>(lastBlockSize), lastRead);
                originalBlockHashes[blockIdx] = useXxh64 ? XXH3_64bits(lastBuf.data(), lastBlockSize) : XXH32(lastBuf.data(), lastBlockSize, 0);
                prog.tick();
            }
            // Do NOT close hReadFile — we'll re-use it for stripe reads
        } else if (!allDataFallback.empty()) {
            for (uint64_t i = 0; i < totalBlocks; ++i) {
                uint64_t offset = i * blockSize;
                size_t currentBlockSize = (offset + blockSize <= fileSize) ? blockSize : static_cast<size_t>(fileSize - offset);
                originalBlockHashes[i] = useXxh64 ? XXH3_64bits(allDataFallback.data() + offset, currentBlockSize) : XXH32(allDataFallback.data() + offset, currentBlockSize, 0);
                prog.tick();
            }
        }
        prog.done();
    }
    std::vector<uint8_t>().swap(allDataFallback);

    // Open output file
    std::ofstream finalOut(parityPath, std::ios::binary);
    if (!finalOut) {
        std::cerr << "Error: Cannot create parity file '" << parityPath << "'.\n";
        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        if (hReadFile != os::InvalidHandle()) os::Close(hReadFile);
        return;
    }
    writeArchiveHeader(finalOut, fileSize, blockSize, totalTracks, useXxh64,
                       originalBlockHashes, manifest,
                       static_cast<uint32_t>(stripeCount), blocksPerStripe);
    std::streampos headerEndPos = finalOut.tellp();
    if (headerEndPos < 0) {
        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        os::RemoveFile(parityPath.c_str());
        if (hReadFile != os::InvalidHandle()) os::Close(hReadFile);
        return;
    }

    // Pre-compute total parity blocks for progress bar
    uint64_t totalParityBlocks = 0;
    for (uint64_t s = 0; s < stripeCount; ++s) {
        uint64_t sStart = s * blocksPerStripe;
        uint64_t sEnd = std::min(sStart + blocksPerStripe, totalBlocks);
        std::vector<uint32_t> counts(totalTracks, 0);
        for (uint64_t i = sStart; i < sEnd; ++i) counts[i % totalTracks]++;
        double carry = 0.0;
        for (uint16_t t = 0; t < totalTracks; ++t) {
            double exact = counts[t] * overhead + carry;
            uint32_t blocks = static_cast<uint32_t>(exact);
            carry = exact - blocks;
            if (blocks == 0 && counts[t] > 0) blocks = 1;
            totalParityBlocks += blocks;
        }
    }

    Progress prog(totalParityBlocks, "Parity");
    std::mutex progMutex;
    std::atomic<bool> encodeFailed{false};

    // Memory-aware parallel stripe processing.
    uint64_t estPerStripeTrackBufs = static_cast<uint64_t>(blocksPerStripe * blockSize / (1024 * 1024));
    uint64_t estPerStripeEncoders = totalTracks * static_cast<uint64_t>(blocksPerStripe * blockSize * 2 / totalTracks / (1024 * 1024));
    uint64_t estPerStripeMB = estPerStripeTrackBufs + estPerStripeEncoders;
    if (estPerStripeMB < 1) estPerStripeMB = 1;
    uint64_t availMB = os::TotalMemoryMB();
    if (availMB == 0) availMB = 4096;
    unsigned int maxStripesInFlight = static_cast<unsigned int>(std::max<uint64_t>(1, availMB / estPerStripeMB / 3));
    unsigned int STRIPE_BATCH = (numJobs > 0 && static_cast<unsigned int>(numJobs) < maxStripesInFlight)
        ? static_cast<unsigned int>(numJobs) : maxStripesInFlight;
    if (STRIPE_BATCH < 1) STRIPE_BATCH = 1;
    if (STRIPE_BATCH > stripeCount) STRIPE_BATCH = static_cast<unsigned int>(stripeCount);
    if (debug) {
        std::cout << "Parallel stripe batch: " << STRIPE_BATCH
                  << " (est " << estPerStripeMB << " MB/stripe, "
                  << availMB << " MB available)\n";
    }

    processAllStripeBatches(finalOut, fileSize, blockSize, totalTracks, useXxh64,
        originalBlockHashes, stripeCount, blocksPerStripe, totalBlocks,
        overhead, hReadFile, allDataFallback, STRIPE_BATCH,
        prog, progMutex, encodeFailed);

    prog.done();

    if (hReadFile != os::InvalidHandle()) os::Close(hReadFile);

    if (encodeFailed) {
        finalOut.close();
        os::RemoveFile(parityPath.c_str());
        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        return;
    }

    // Write mirror header and footer
    writeArchiveHeader(finalOut, fileSize, blockSize, totalTracks, useXxh64,
                       originalBlockHashes, manifest,
                       static_cast<uint32_t>(stripeCount), blocksPerStripe);
    uint64_t hbs = static_cast<uint64_t>(static_cast<std::streamoff>(headerEndPos));
    writeU64LE(finalOut, hbs);
    finalOut.close();

    if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());

    double elapsed = elapsedSecsSince(startTime);
    std::cout << "Done! Parity generation completed successfully.";
    if (elapsed >= 60.0) {
        int mins = static_cast<int>(elapsed / 60);
        double secs = elapsed - mins * 60;
        std::cout << " (" << mins << "m " << secs << "s)";
    } else {
        std::cout << " (" << elapsed << "s)";
    }
    std::cout << std::endl;
}

// ────────────────────────────────────────────────────────────

static int parseExistingOverheadPct(const std::string& archivePath) {
    std::string fn = std::filesystem::path(archivePath).filename().string();
    size_t ppos = fn.find(".p");
    if (ppos == std::string::npos || ppos + 2 >= fn.size()) return 0;
    size_t dot2 = fn.find('.', ppos + 2);
    if (dot2 == std::string::npos) return 0;
    try {
        int pct = std::stoi(fn.substr(ppos + 2, dot2 - ppos - 2));
        if (pct >= 1 && pct <= 99) return pct;
    } catch (...) {}
    return 0;
}

static int parseCumulativeBasePct(const std::string& archivePath) {
    // Returns the total cumulative overhead represented by the archive.
    // e.g., "base.p10+05.whpar" → 15, "base.p10.whpar" → 10
    std::string fn = std::filesystem::path(archivePath).filename().string();
    size_t ppos = fn.find(".p");
    if (ppos == std::string::npos || ppos + 2 >= fn.size()) return 0;
    size_t end = ppos + 2;
    while (end < fn.size() && fn[end] >= '0' && fn[end] <= '9') end++;
    int total = 0;
    try {
        total = std::stoi(fn.substr(ppos + 2, end - ppos - 2));
    } catch (...) { return 0; }
    // Sum any +NN suffixes
    size_t i = end;
    while (i < fn.size()) {
        if (fn[i] == '+') {
            i++;
            size_t start = i;
            while (i < fn.size() && fn[i] >= '0' && fn[i] <= '9') i++;
            if (i > start) {
                try { total += std::stoi(fn.substr(start, i - start)); }
                catch (...) {}
            }
        } else {
            i++;
        }
    }
    return total;
}

// ────────────────────────────────────────────────────────────

void AddParity(const std::string& archivePath, float additionalOverhead, bool debug, bool force, uint32_t numJobs, bool noRecursive, uint64_t maxMemBytes) {
    auto startTime = std::chrono::steady_clock::now();
    std::filesystem::path archPath(archivePath);

    // ── 1. Read archive metadata ──────────────────────────
    std::ifstream parityIn(archivePath, std::ios::binary);
    if (!parityIn) {
        std::cerr << "Error: Cannot open archive: " << archivePath << "\n";
        return;
    }

    parityIn.seekg(0, std::ios::end);
    std::streampos fileSz = parityIn.tellg();
    if (fileSz < static_cast<std::streampos>(sizeof(uint64_t) + sizeof(PacketHeader))) {
        std::cerr << "Error: Archive too small or corrupt.\n"; return;
    }
    parityIn.seekg(-static_cast<std::streamoff>(sizeof(uint64_t)), std::ios::end);
    uint64_t headerBlockSize = 0;
    readU64LE(parityIn, headerBlockSize);
    if (headerBlockSize < sizeof(PacketHeader) || headerBlockSize > static_cast<uint64_t>(fileSz) - sizeof(uint64_t)) {
        std::cerr << "Error: Invalid header size in archive footer.\n"; return;
    }
    std::streampos headerEndOffset = static_cast<std::streampos>(static_cast<uint64_t>(fileSz) - sizeof(uint64_t) - headerBlockSize);
    if (headerEndOffset <= 0 || headerEndOffset >= fileSz) {
        std::cerr << "Error: Corrupt archive offset.\n"; return;
    }

    auto readHeaderBlock = [&](std::streampos pos, PacketHeader& hdr, std::vector<uint64_t>& hashes, std::vector<ManifestEntry>& manifest) -> bool {
        parityIn.seekg(pos);
        readPacketHeader(parityIn, hdr);
        if (!parityIn || hdr.magic != WHPAR_MAGIC) return false;
        uint32_t hashCount = 0;
        readU32LE(parityIn, hashCount);
        if (hashCount > (static_cast<uint64_t>(fileSz) - static_cast<uint64_t>(static_cast<std::streamoff>(pos))
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

    auto verifyHeaderEcc = [&](std::streampos pos) -> bool {
        std::vector<uint8_t> body(static_cast<size_t>(headerBlockSize));
        parityIn.seekg(pos);
        parityIn.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(headerBlockSize - 8));
        if (!parityIn) return false;
        uint64_t stored = 0;
        readU64LE(parityIn, stored);
        return XXH3_64bits(body.data(), headerBlockSize - 8) == stored;
    };

    PacketHeader headerA, headerEnd;
    std::vector<uint64_t> hashesA, hashesEnd;
    std::vector<ManifestEntry> manifestA, manifestEnd;

    bool mainOk = readHeaderBlock(0, headerA, hashesA, manifestA);
    if (mainOk) mainOk = verifyHeaderEcc(0);
    bool mirrorOk = readHeaderBlock(headerEndOffset, headerEnd, hashesEnd, manifestEnd);
    if (mirrorOk) mirrorOk = verifyHeaderEcc(headerEndOffset);

    PacketHeader hdr;
    std::vector<uint64_t> blockHashes;
    std::vector<ManifestEntry> manifest;

    if (mainOk && mirrorOk) {
        if (hashesA == hashesEnd) { hdr = headerEnd; blockHashes = std::move(hashesEnd); manifest = std::move(manifestEnd); }
        else { hdr = headerA; blockHashes = std::move(hashesA); manifest = std::move(manifestA); }
    } else if (mirrorOk) {
        hdr = headerEnd; blockHashes = std::move(hashesEnd); manifest = std::move(manifestEnd);
    } else if (mainOk) {
        hdr = headerA; blockHashes = std::move(hashesA); manifest = std::move(manifestA);
    } else {
        std::cerr << "Error: Cannot read archive headers.\n"; return;
    }
    parityIn.close();

    // ── 2. Derive metadata from archive ───────────────────
    int existingPct = parseExistingOverheadPct(archivePath);
    int cumulativeBase = parseCumulativeBasePct(archivePath);
    int addPct = static_cast<int>(additionalOverhead * 100.0f + 0.5f);
    if (addPct < 1) addPct = 1;
    if (addPct > 99) addPct = 99;

    // ── 3. Generate supplemental filename ─────────────────
    // Format: basename.p{cumulative}+{add}.whpar
    // e.g., "data.p10.whpar" + 5% → "data.p10+05.whpar"
    //       "data.p10+05.whpar" + 3% → "data.p15+03.whpar"
    std::string fn = archPath.filename().string();
    size_t ppos = fn.find(".p");
    std::string base = (ppos != std::string::npos) ? fn.substr(0, ppos) : fn;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = base.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".whpar") base = base.substr(0, dot);
    }
    std::string sCum = (cumulativeBase < 10 ? "0" : "") + std::to_string(cumulativeBase);
    std::string sAdd = (addPct < 10 ? "0" : "") + std::to_string(addPct);
    std::string outputPath = (archPath.parent_path().empty() ? "" : archPath.parent_path().u8string() + "/")
        + base + ".p" + sCum + "+" + sAdd + ".whpar";

    std::cout << "Source archive: " << archivePath << " (" << existingPct << "% base overhead";
    if (cumulativeBase != existingPct) std::cout << ", cumulative " << cumulativeBase << "%";
    std::cout << ")\n";
    std::cout << "Generating supplemental parity: +" << addPct << "%\n";
    std::cout << "Supplement: " << outputPath << "\n";

    if (!force) {
        std::ifstream test(outputPath);
        if (test.good()) {
            test.close();
            std::cout << "Warning: '" << outputPath << "' already exists. Overwrite? (y/N): ";
            std::string answer;
            std::getline(std::cin, answer);
            if (answer != "y" && answer != "Y") { std::cout << "Aborted.\n"; return; }
        }
    }

    // ── 4. Resolve source files from manifest ─────────────
    if (manifest.empty()) {
        std::cerr << "Error: Archive has no file manifest; cannot locate source files.\n";
        return;
    }

    std::string archiveDir = archPath.parent_path().u8string();
    if (archiveDir.empty()) archiveDir = ".";
    std::vector<std::string> sourcePaths;
    sourcePaths.reserve(manifest.size());

    for (auto& me : manifest) {
        std::filesystem::path candidate1 = std::filesystem::path(archiveDir) / me.relPath;
        std::filesystem::path candidate2 = std::filesystem::path(".") / me.relPath;
        if (std::filesystem::exists(candidate1)) {
            sourcePaths.push_back(candidate1.u8string());
        } else if (std::filesystem::exists(candidate2)) {
            sourcePaths.push_back(candidate2.u8string());
        } else {
            std::cerr << "Error: Source file not found: " << me.relPath << "\n"
                      << "       Looked in: " << std::filesystem::absolute(candidate1) << "\n"
                      << "       and: " << std::filesystem::absolute(candidate2) << "\n"
                      << "       -a requires the original source files to generate new parity.\n";
            return;
        }
    }

    for (size_t i = 0; i < manifest.size(); ++i) {
        uint64_t actualSize = std::filesystem::file_size(sourcePaths[i]);
        if (actualSize != manifest[i].fileSize) {
            std::cerr << "Error: Source file '" << sourcePaths[i] << "' size mismatch:\n"
                      << "       expected " << manifest[i].fileSize << " bytes, got " << actualSize << " bytes.\n";
            return;
        }
    }

    // ── 5. Open source data ───────────────────────────────
    bool useXxh64 = (hdr.matrixTrack == 1);
    uint64_t fileSize = hdr.originalFileSize;
    uint32_t blockSize = hdr.blockSize;
    uint16_t totalTracks = static_cast<uint16_t>(hdr.fountainId);
    if (totalTracks == 0) totalTracks = 1;
    uint64_t totalBlocks = (fileSize + blockSize - 1) / blockSize;

    std::cout << "File Size: " << (fileSize / (1024 * 1024.0)) << " MB\n";
    std::cout << "Block Size: " << (blockSize / 1024) << " KB\n";
    std::cout << "Interleaving Architecture: " << totalTracks << " parallel track(s)\n";
    std::cout << "Total Blocks: " << totalBlocks << "\n";

    std::vector<uint8_t> allData;
    std::string tempFilePath;
    os::FileHandle hConcat = os::InvalidHandle();

    if (sourcePaths.size() > 1) {
        std::string tempDir = os::TempDir();
        os::FileHandle hTempWrite = os::InvalidHandle();
        for (int attempt = 0; attempt < 100; ++attempt) {
            tempFilePath = tempDir + "whp_" + std::to_string(os::ProcessId()) + "_" + std::to_string(os::TickCount() + attempt) + ".tmp";
            hTempWrite = os::CreateNew(tempFilePath.c_str(), true);
            if (hTempWrite != os::InvalidHandle()) break;
            if (attempt == 99) { tempFilePath.clear(); break; }
        }
        if (hTempWrite != os::InvalidHandle()) {
            std::vector<uint8_t> copyBuf(64ULL * 1024 * 1024);
            bool writeFailed = false;
            for (auto& f : sourcePaths) {
                os::FileHandle hSrc = os::OpenRead(f.c_str(), false);
                if (hSrc == os::InvalidHandle()) { writeFailed = true; break; }
                uint32_t br = 0;
                while (os::Read(hSrc, copyBuf.data(), static_cast<uint32_t>(copyBuf.size()), br) && br > 0) {
                    if (!os::Write(hTempWrite, copyBuf.data(), br)) { writeFailed = true; break; }
                }
                os::Close(hSrc);
                if (writeFailed) break;
            }
            os::Close(hTempWrite);
            if (!writeFailed) hConcat = os::OpenRead(tempFilePath.c_str(), true);
        }
    }

    if (hConcat == os::InvalidHandle()) {
        allData.reserve(static_cast<size_t>(fileSize));
        for (auto& f : sourcePaths) {
            std::ifstream in(f, std::ios::binary);
            if (!in) continue;
            size_t sz = static_cast<size_t>(std::filesystem::file_size(f));
            size_t oldSz = allData.size();
            allData.resize(oldSz + sz);
            in.read(reinterpret_cast<char*>(allData.data() + oldSz), static_cast<std::streamsize>(sz));
        }
    }

    // ── 6. Compute stripe count ───────────────────────────
    uint64_t stripeCount = 1;
    uint64_t blocksPerStripe = totalBlocks;
    if (maxMemBytes > 0 && fileSize > maxMemBytes) {
        stripeCount = (fileSize + maxMemBytes - 1) / maxMemBytes;
        if (stripeCount > totalBlocks) stripeCount = totalBlocks;
        if (stripeCount < 1) stripeCount = 1;
        blocksPerStripe = (totalBlocks + stripeCount - 1) / stripeCount;
        if (debug) {
            std::cout << "Stripe processing: " << stripeCount << " stripes, "
                      << blocksPerStripe << " blocks/stripe, "
                      << (blocksPerStripe * blockSize / (1024 * 1024)) << " MB/stripe\n";
        }
    }

    // ── 7. Generate parity (non-stripe or stripe path) ──
    float additionalOverheadFrac = addPct / 100.0f;
    std::cout << "Encoding " << addPct << "% additional parity...\n";

    // ── 1-track fast path (no interleaving needed) ──
    if (totalTracks == 1) {
        std::vector<uint8_t> flatData;

        {
            Progress prog(totalBlocks, "Hash");
            uint32_t blocksPerRead = static_cast<uint32_t>((64ULL * 1024 * 1024) / blockSize);
            if (blocksPerRead < 1) blocksPerRead = 1;
            size_t readBufSize = static_cast<size_t>(blocksPerRead) * blockSize;
            std::vector<uint8_t> readBuf(readBufSize);
            uint64_t blockIdx = 0;

            if (hConcat != os::InvalidHandle()) {
                uint32_t bytesRead = 0;
                while (blockIdx < totalBlocks && os::Read(hConcat, readBuf.data(), static_cast<uint32_t>(readBufSize), bytesRead) && bytesRead > 0) {
                    uint64_t bufOffset = 0;
                    while (bufOffset + blockSize <= bytesRead && blockIdx < totalBlocks) {
                        prog.tick();
                        blockIdx++;
                        bufOffset += blockSize;
                    }
                    flatData.insert(flatData.end(), readBuf.data(), readBuf.data() + bytesRead);
                }
                if (blockIdx < totalBlocks) {
                    uint64_t offset = blockIdx * blockSize;
                    size_t lastBlockSize = static_cast<size_t>(fileSize - offset);
                    std::vector<uint8_t> lastBuf(lastBlockSize);
                    if (!os::Seek(hConcat, static_cast<int64_t>(offset), 0)) {
                        std::cerr << "Error: Seek failed.\n"; os::Close(hConcat); return;
                    }
                    uint32_t lastRead = 0;
                    if (!os::Read(hConcat, lastBuf.data(), static_cast<uint32_t>(lastBlockSize), lastRead)) {
                        std::cerr << "Error: Read failed.\n"; os::Close(hConcat); return;
                    }
                    flatData.insert(flatData.end(), lastBuf.begin(), lastBuf.end());
                    prog.tick();
                }
                os::Close(hConcat);
            } else if (!allData.empty()) {
                flatData = std::move(allData);
                for (uint64_t i = 0; i < totalBlocks; ++i) {
                    prog.tick();
                }
            }
            prog.done();
        }

        std::vector<uint32_t> trackBlockCounts = {static_cast<uint32_t>(totalBlocks)};
        std::vector<std::vector<uint8_t>> trackBufs(1);
        trackBufs[0] = std::move(flatData);

        bool ok = writeParityOutput(
            outputPath, fileSize, blockSize, 1, useXxh64,
            blockHashes, manifest, trackBufs, trackBlockCounts,
            additionalOverheadFrac, tempFilePath, debug
        );

        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        if (ok) {
            double elapsed = elapsedSecsSince(startTime);
            std::cout << "Supplemental archive created: " << outputPath;
            if (elapsed >= 60.0) {
                int mins = static_cast<int>(elapsed / 60);
                double secs = elapsed - mins * 60;
                std::cout << " (" << mins << "m " << secs << "s)";
            } else {
                std::cout << " (" << elapsed << "s)";
            }
            std::cout << "\n";
        }
        return;
    }

    if (stripeCount <= 1) {
        // ── Non-stripe path (load all, encode once) ──
        std::vector<uint32_t> trackBlockCounts(totalTracks, 0);
        for (uint64_t i = 0; i < totalBlocks; ++i) trackBlockCounts[i % totalTracks]++;

        std::vector<std::vector<uint8_t>> trackBufs(totalTracks);
        for (uint16_t t = 0; t < totalTracks; ++t)
            trackBufs[t].reserve(trackBlockCounts[t] * blockSize);

        {
            Progress prog(totalBlocks, "Hash");
            if (hConcat != os::InvalidHandle()) {
                uint32_t blocksPerRead = static_cast<uint32_t>((64ULL * 1024 * 1024) / blockSize);
                if (blocksPerRead < 1) blocksPerRead = 1;
                size_t readBufSize = static_cast<size_t>(blocksPerRead) * blockSize;
                std::vector<uint8_t> readBuf(readBufSize);
                uint64_t blockIdx = 0;
                uint32_t bytesRead = 0;
                while (blockIdx < totalBlocks && os::Read(hConcat, readBuf.data(), static_cast<uint32_t>(readBufSize), bytesRead) && bytesRead > 0) {
                    uint64_t bufOffset = 0;
                    while (bufOffset + blockSize <= bytesRead && blockIdx < totalBlocks) {
                        uint16_t t = static_cast<uint16_t>(blockIdx % totalTracks);
                        trackBufs[t].insert(trackBufs[t].end(), readBuf.data() + bufOffset, readBuf.data() + bufOffset + blockSize);
                        prog.tick();
                        blockIdx++;
                        bufOffset += blockSize;
                    }
                }
                if (blockIdx < totalBlocks) {
                    uint64_t offset = blockIdx * blockSize;
                    size_t lastBlockSize = static_cast<size_t>(fileSize - offset);
                    std::vector<uint8_t> lastBuf(lastBlockSize);
                    if (!os::Seek(hConcat, static_cast<int64_t>(offset), 0)) {
                        std::cerr << "Error: Seek failed.\n"; os::Close(hConcat); return;
                    }
                    uint32_t lastRead = 0;
                    if (!os::Read(hConcat, lastBuf.data(), static_cast<uint32_t>(lastBlockSize), lastRead)) {
                        std::cerr << "Error: Read failed.\n"; os::Close(hConcat); return;
                    }
                    uint16_t t = static_cast<uint16_t>(blockIdx % totalTracks);
                    trackBufs[t].insert(trackBufs[t].end(), lastBuf.data(), lastBuf.data() + lastBlockSize);
                    prog.tick();
                }
                os::Close(hConcat);
            } else if (!allData.empty()) {
                for (uint64_t i = 0; i < totalBlocks; ++i) {
                    uint64_t offset = i * blockSize;
                    size_t currentBlockSize = (offset + blockSize <= fileSize) ? blockSize : static_cast<size_t>(fileSize - offset);
                    uint16_t t = static_cast<uint16_t>(i % totalTracks);
                    trackBufs[t].insert(trackBufs[t].end(), allData.data() + offset, allData.data() + offset + currentBlockSize);
                    prog.tick();
                }
            }
            prog.done();
        }

        bool ok = writeParityOutput(
            outputPath, fileSize, blockSize, totalTracks, useXxh64,
            blockHashes, manifest, trackBufs, trackBlockCounts,
            additionalOverheadFrac, tempFilePath, debug
        );

        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
        if (ok) {
            double elapsed = elapsedSecsSince(startTime);
            std::cout << "Supplemental archive created: " << outputPath;
            if (elapsed >= 60.0) {
                int mins = static_cast<int>(elapsed / 60);
                double secs = elapsed - mins * 60;
                std::cout << " (" << mins << "m " << secs << "s)";
            } else {
                std::cout << " (" << elapsed << "s)";
            }
            std::cout << "\n";
            std::cout << "Keep all archive files together for repair.\n";
            std::cout << "Repair auto-discovers all matching .whpar files.\n";
        }
    } else {
        // ── Stripe path ──
        std::ofstream finalOut(outputPath, std::ios::binary);
        if (!finalOut) {
            std::cerr << "Error: Cannot create supplemental archive '" << outputPath << "'.\n";
            if (hConcat != os::InvalidHandle()) os::Close(hConcat);
            if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
            return;
        }

        writeArchiveHeader(finalOut, fileSize, blockSize, totalTracks, useXxh64,
                          blockHashes, manifest,
                          static_cast<uint32_t>(stripeCount), blocksPerStripe);
        std::streampos headerEndPos = finalOut.tellp();
        if (headerEndPos < 0) {
            if (hConcat != os::InvalidHandle()) os::Close(hConcat);
            if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
            os::RemoveFile(outputPath.c_str());
            return;
        }

        uint64_t totalParityBlocks = 0;
        for (uint64_t s = 0; s < stripeCount; ++s) {
            uint64_t sStart = s * blocksPerStripe;
            uint64_t sEnd = std::min(sStart + blocksPerStripe, totalBlocks);
            std::vector<uint32_t> counts(totalTracks, 0);
            for (uint64_t i = sStart; i < sEnd; ++i) counts[i % totalTracks]++;
            double carry = 0.0;
            for (uint16_t t = 0; t < totalTracks; ++t) {
                double exact = counts[t] * additionalOverheadFrac + carry;
                uint32_t blocks = static_cast<uint32_t>(exact);
                carry = exact - blocks;
                if (blocks == 0 && counts[t] > 0) blocks = 1;
                totalParityBlocks += blocks;
            }
        }

        Progress prog(totalParityBlocks, "Parity");
        std::mutex progMutex;
        std::atomic<bool> encodeFailed{false};

        uint64_t estPerStripeTrackBufs = static_cast<uint64_t>(blocksPerStripe * blockSize / (1024 * 1024));
        uint64_t estPerStripeEncoders = totalTracks * static_cast<uint64_t>(blocksPerStripe * blockSize * 2 / totalTracks / (1024 * 1024));
        uint64_t estPerStripeMB = estPerStripeTrackBufs + estPerStripeEncoders;
        if (estPerStripeMB < 1) estPerStripeMB = 1;
        uint64_t availMB = os::TotalMemoryMB();
        if (availMB == 0) availMB = 4096;
        unsigned int maxStripesInFlight = static_cast<unsigned int>(std::max<uint64_t>(1, availMB / estPerStripeMB / 3));
        unsigned int STRIPE_BATCH = (numJobs > 0 && static_cast<unsigned int>(numJobs) < maxStripesInFlight)
            ? static_cast<unsigned int>(numJobs) : maxStripesInFlight;
        if (STRIPE_BATCH < 1) STRIPE_BATCH = 1;
        if (STRIPE_BATCH > stripeCount) STRIPE_BATCH = static_cast<unsigned int>(stripeCount);

        processAllStripeBatches(finalOut, fileSize, blockSize, totalTracks, useXxh64,
            blockHashes, stripeCount, blocksPerStripe, totalBlocks,
            additionalOverheadFrac, hConcat, allData, STRIPE_BATCH,
            prog, progMutex, encodeFailed);

        prog.done();

        if (hConcat != os::InvalidHandle()) os::Close(hConcat);

        if (encodeFailed) {
            finalOut.close();
            os::RemoveFile(outputPath.c_str());
            if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());
            return;
        }

        writeArchiveHeader(finalOut, fileSize, blockSize, totalTracks, useXxh64,
                          blockHashes, manifest,
                          static_cast<uint32_t>(stripeCount), blocksPerStripe);
        uint64_t hbs = static_cast<uint64_t>(static_cast<std::streamoff>(headerEndPos));
        writeU64LE(finalOut, hbs);
        finalOut.close();

        if (!tempFilePath.empty()) os::RemoveFile(tempFilePath.c_str());

        double elapsed = elapsedSecsSince(startTime);
        std::cout << "Supplemental archive created: " << outputPath;
        if (elapsed >= 60.0) {
            int mins = static_cast<int>(elapsed / 60);
            double secs = elapsed - mins * 60;
            std::cout << " (" << mins << "m " << secs << "s)";
        } else {
            std::cout << " (" << elapsed << "s)";
        }
        std::cout << "\n";
        std::cout << "Keep all archive files together for repair.\n";
        std::cout << "Repair auto-discovers all matching .whpar files.\n";
    }
}
