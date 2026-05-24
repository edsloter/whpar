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
#include <windows.h>

void CreateParity(const std::string& sourcePath, const std::string& parityPath, float overhead, bool debug, uint32_t blockSizeKB, bool useXxh64, uint32_t numJobs, bool noRecursive) {
    auto startTime = std::chrono::steady_clock::now();

    std::vector<ManifestEntry> manifest;
    std::vector<uint8_t> allDataFallback;

    auto normalizeSlashes = [](std::string p) -> std::string {
        for (auto& c : p) if (c == '\\') c = '/';
        return p;
    };

    std::filesystem::path src(sourcePath);
    bool isSrcDir = std::filesystem::is_directory(src);
    std::vector<std::filesystem::path> srcFiles;

    if (isSrcDir) {
        if (noRecursive) {
            for (auto& p : std::filesystem::directory_iterator(src)) {
                if (std::filesystem::is_regular_file(p.path())) srcFiles.push_back(p.path());
            }
        } else {
            for (auto& p : std::filesystem::recursive_directory_iterator(src)) {
                if (std::filesystem::is_regular_file(p.path())) srcFiles.push_back(p.path());
            }
        }
        std::sort(srcFiles.begin(), srcFiles.end());
        for (auto& f : srcFiles) {
            ManifestEntry e;
            e.relPath = normalizeSlashes(std::filesystem::relative(f, src).u8string());
            e.fileSize = std::filesystem::file_size(f);
            auto ftime = std::filesystem::last_write_time(f);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
            e.mtime = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(sctp));
            e.attributes = GetFileAttributesA(f.string().c_str());
            manifest.push_back(e);
        }
    } else if (std::filesystem::is_regular_file(src)) {
        ManifestEntry e;
        e.relPath = normalizeSlashes(src.filename().u8string());
        e.fileSize = std::filesystem::file_size(src);
        auto ftime = std::filesystem::last_write_time(src);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
        e.mtime = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(sctp));
        e.attributes = GetFileAttributesA(sourcePath.c_str());
        manifest.push_back(e);
    } else {
        std::cerr << "Error: source path is not a file or directory: " << sourcePath << "\n";
        return;
    }

    uint64_t fileSize = 0;
    for (auto& me : manifest) fileSize += me.fileSize;

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

    std::vector<uint64_t> originalBlockHashes(totalBlocks);
    std::vector<uint32_t> trackBlockCounts(totalTracks, 0);
    for (uint64_t i = 0; i < totalBlocks; ++i) {
        trackBlockCounts[i % totalTracks]++;
    }

    std::vector<std::vector<uint8_t>> trackBufs(totalTracks);
    for (uint16_t t = 0; t < totalTracks; ++t) {
        trackBufs[t].reserve(trackBlockCounts[t] * blockSize);
    }

    std::string tempFilePath;
    HANDLE hReadFile = INVALID_HANDLE_VALUE;
    if (manifest.size() == 1) {
        std::string singleFilePath = isSrcDir ? srcFiles[0].string() : sourcePath;
        hReadFile = CreateFileA(singleFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    } else if (isSrcDir) {
        char tempPathBuf[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPathBuf);
        HANDLE hTempWrite = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 100; ++attempt) {
            tempFilePath = std::string(tempPathBuf) + "whp_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64() + attempt) + ".tmp";
            hTempWrite = CreateFileA(tempFilePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (hTempWrite != INVALID_HANDLE_VALUE) break;
            if (attempt == 99) { tempFilePath.clear(); break; }
        }
        if (hTempWrite == INVALID_HANDLE_VALUE) {
            std::cerr << "Error: Cannot create temp file.\n";
            return;
        }
        if (hTempWrite != INVALID_HANDLE_VALUE) {
            std::vector<uint8_t> copyBuf(64ULL * 1024 * 1024);
            bool writeFailed = false;
            for (auto& f : srcFiles) {
                HANDLE hSrc = CreateFileA(f.string().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hSrc == INVALID_HANDLE_VALUE) { writeFailed = true; break; }
                DWORD br = 0;
                while (ReadFile(hSrc, copyBuf.data(), static_cast<DWORD>(copyBuf.size()), &br, NULL) && br > 0) {
                    DWORD bw = 0;
                    if (!WriteFile(hTempWrite, copyBuf.data(), br, &bw, NULL)) { writeFailed = true; break; }
                }
                CloseHandle(hSrc);
                if (writeFailed) break;
            }
            CloseHandle(hTempWrite);
            if (!writeFailed) {
                hReadFile = CreateFileA(tempFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            }
        }
        if (hReadFile == INVALID_HANDLE_VALUE) {
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

    {
        Progress prog(totalBlocks, "Hash");
        uint32_t blocksPerRead = static_cast<uint32_t>((64ULL * 1024 * 1024) / blockSize);
        if (blocksPerRead < 1) blocksPerRead = 1;
        size_t readBufSize = static_cast<size_t>(blocksPerRead) * blockSize;
        std::vector<uint8_t> readBuf(readBufSize);
        uint64_t blockIdx = 0;

        if (hReadFile != INVALID_HANDLE_VALUE) {
            DWORD bytesRead = 0;
            while (blockIdx < totalBlocks && ReadFile(hReadFile, readBuf.data(), static_cast<DWORD>(readBufSize), &bytesRead, NULL) && bytesRead > 0) {
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
                if (INVALID_SET_FILE_POINTER == SetFilePointer(hReadFile, static_cast<LONG>(offset), NULL, FILE_BEGIN) && GetLastError() != NO_ERROR) {
                    std::cerr << "Error: Failed to seek in source file.\n";
                    CloseHandle(hReadFile);
                    return;
                }
                DWORD lastRead = 0;
                if (!ReadFile(hReadFile, lastBuf.data(), static_cast<DWORD>(lastBlockSize), &lastRead, NULL)) {
                    std::cerr << "Error: Failed to read last block from source file.\n";
                    CloseHandle(hReadFile);
                    return;
                }
                uint16_t t = static_cast<uint16_t>(blockIdx % totalTracks);
                trackBufs[t].insert(trackBufs[t].end(), lastBuf.data(), lastBuf.data() + lastBlockSize);
                originalBlockHashes[blockIdx] = useXxh64 ? XXH3_64bits(lastBuf.data(), lastBlockSize) : XXH32(lastBuf.data(), lastBlockSize, 0);
                prog.tick();
            }
            CloseHandle(hReadFile);
            hReadFile = INVALID_HANDLE_VALUE;
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
    Progress prog(totalParityBlocks, "Parity");

    auto writeMainHeaderAndHashes = [&](std::ofstream& out) {
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
        mainHeader.blockSequence = 0;
        mainHeader.expectedBlockHash = 0;

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
    };

    std::ofstream finalOut(parityPath, std::ios::binary);
    if (!finalOut) {
        std::cerr << "Error: Cannot create final parity file.\n";
        if (!tempFilePath.empty()) DeleteFileA(tempFilePath.c_str());
        return;
    }
    writeMainHeaderAndHashes(finalOut);
    std::streampos headerEndPos = finalOut.tellp();

    unsigned int BATCH_SIZE = 2;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        uint64_t gb = ms.ullTotalPhys / (1024ULL * 1024 * 1024);
        if (gb >= 40) BATCH_SIZE = 3;
    }
    if (BATCH_SIZE > totalTracks) BATCH_SIZE = totalTracks;

    std::vector<std::vector<uint8_t>> trackParityData(totalTracks);
    std::atomic<bool> encodeFailed{false};
    std::mutex progMutex;

    for (uint16_t batchStart = 0; batchStart < totalTracks && !encodeFailed; batchStart += BATCH_SIZE) {
        uint16_t batchEnd = (batchStart + BATCH_SIZE < totalTracks) ? (batchStart + BATCH_SIZE) : totalTracks;
        std::vector<std::future<void>> futures;

        for (uint16_t t = batchStart; t < batchEnd; ++t) {
            if (trackBlockCounts[t] == 0) continue;

            futures.push_back(std::async(std::launch::async, [&, t]() {
                WirehairCodec enc = nullptr;
                try {
                    const std::vector<uint8_t>& trackBuf = trackBufs[t];

                    enc = wirehair_encoder_create(nullptr, trackBuf.data(), trackBuf.size(), blockSize);
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
                            uint32_t targetGlobalIndex = targetLocalIndex * totalTracks + t;
                            if (targetGlobalIndex >= originalBlockHashes.size())
                                targetGlobalIndex = static_cast<uint32_t>(originalBlockHashes.size() - 1);
                            header.blockSequence = targetGlobalIndex;
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

                    wirehair_free(enc);
                    enc = nullptr;
                    trackParityData[t] = std::move(localBuf);
                } catch (...) {
                    if (enc) wirehair_free(enc);
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

    prog.done();

    std::vector<uint8_t>().swap(allDataFallback);

    if (encodeFailed) {
        if (!tempFilePath.empty()) DeleteFileA(tempFilePath.c_str());
        return;
    }

    writeMainHeaderAndHashes(finalOut);
    uint64_t headerBlockSize = static_cast<uint64_t>(static_cast<std::streamoff>(headerEndPos));
    writeU64LE(finalOut, headerBlockSize);
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

    if (!tempFilePath.empty()) DeleteFileA(tempFilePath.c_str());
}
