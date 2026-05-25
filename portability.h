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

#ifdef _WIN32
  #define NOMINMAX
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <time.h>
  #include <cerrno>
#endif

namespace os {

#ifdef _WIN32
using FileHandle = HANDLE;
inline FileHandle InvalidHandle() { return INVALID_HANDLE_VALUE; }
#else
using FileHandle = int;
inline FileHandle InvalidHandle() { return -1; }
#endif

struct Mapping {
    void* data = nullptr;
    uint64_t size = 0;
#ifdef _WIN32
    HANDLE hMap = NULL;
#else
    int fd = -1;
#endif
};

// --- Open / Close / Delete ---

inline FileHandle OpenRead(const char* path, bool sequential = false) {
#ifdef _WIN32
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        sequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_ATTRIBUTE_NORMAL, NULL);
#else
    (void)sequential;
    return ::open(path, O_RDONLY);
#endif
}

inline FileHandle OpenWrite(const char* path, bool sequential = false) {
#ifdef _WIN32
    return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        sequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_ATTRIBUTE_NORMAL, NULL);
#else
    FileHandle h = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return h;
#endif
}

inline FileHandle OpenReadWrite(const char* path) {
#ifdef _WIN32
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
#else
    return ::open(path, O_RDWR);
#endif
}

inline FileHandle CreateNew(const char* path, bool sequential = false) {
#ifdef _WIN32
    return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
        sequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_ATTRIBUTE_NORMAL, NULL);
#else
    return ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
#endif
}

inline void Close(FileHandle h) {
    if (h == InvalidHandle()) return;
#ifdef _WIN32
    CloseHandle(h);
#else
    ::close(h);
#endif
}

inline bool RemoveFile(const char* path) {
#ifdef _WIN32
    return DeleteFileA(path) != 0;
#else
    return ::unlink(path) == 0;
#endif
}

// --- Read / Write / Seek / Size ---

inline bool Read(FileHandle h, void* buf, uint32_t size, uint32_t& bytesRead) {
#ifdef _WIN32
    DWORD br = 0;
    BOOL ok = ReadFile(h, buf, size, &br, NULL);
    bytesRead = br;
    return ok != 0;
#else
    ssize_t r = ::read(h, buf, size);
    if (r < 0) { bytesRead = 0; return false; }
    bytesRead = static_cast<uint32_t>(r);
    return true;
#endif
}

inline bool Write(FileHandle h, const void* buf, uint32_t size) {
#ifdef _WIN32
    DWORD bw = 0;
    return WriteFile(h, buf, size, &bw, NULL) != 0 && bw == size;
#else
    ssize_t r = ::write(h, buf, size);
    return r >= 0 && static_cast<uint32_t>(r) == size;
#endif
}

inline bool Seek(FileHandle h, int64_t offset, int origin) {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = offset;
    return SetFilePointerEx(h, li, NULL, static_cast<DWORD>(origin)) != 0;
#else
    return ::lseek(h, offset, origin) >= 0;
#endif
}

inline uint64_t Size(FileHandle h) {
#ifdef _WIN32
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return 0;
    return static_cast<uint64_t>(li.QuadPart);
#else
    struct stat st;
    if (::fstat(h, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
#endif
}

// --- Memory mapping ---

inline bool MapRead(const char* path, Mapping& mm) {
#ifdef _WIN32
    mm.hMap = NULL;
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    HANDLE hm = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hm) { CloseHandle(hFile); return false; }
    void* p = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(hm); CloseHandle(hFile); return false; }
    mm.data = p;
    mm.hMap = hm;
    // size from file handle
    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) { UnmapViewOfFile(p); CloseHandle(hm); CloseHandle(hFile); return false; }
    mm.size = static_cast<uint64_t>(li.QuadPart);
    // Keep hFile open for the mapping's lifetime on Windows — mapping holds ref
    // We can close it though; the mapping keeps the file alive.
    CloseHandle(hFile);
    return true;
#else
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0) { ::close(fd); return false; }
    uint64_t sz = static_cast<uint64_t>(st.st_size);
    void* p = ::mmap(nullptr, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); return false; }
    mm.data = p;
    mm.size = sz;
    mm.fd = fd;
    return true;
#endif
}

inline void Unmap(Mapping& mm) {
    if (!mm.data) return;
#ifdef _WIN32
    UnmapViewOfFile(mm.data);
    if (mm.hMap) CloseHandle(mm.hMap);
#else
    ::munmap(mm.data, mm.size);
    if (mm.fd >= 0) ::close(mm.fd);
#endif
    mm.data = nullptr;
    mm.size = 0;
#ifdef _WIN32
    mm.hMap = NULL;
#else
    mm.fd = -1;
#endif
}

inline bool SetFileSize(FileHandle h, uint64_t size) {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return false;
    return SetEndOfFile(h) != 0;
#else
    return ::ftruncate(h, static_cast<off_t>(size)) == 0;
#endif
}

inline bool MapWrite(FileHandle hFile, uint64_t fileSize, Mapping& mm) {
#ifdef _WIN32
    HANDLE hm = CreateFileMappingA(hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!hm) return false;
    void* p = MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, 0);
    if (!p) { CloseHandle(hm); return false; }
    mm.data = p;
    mm.hMap = hm;
    mm.size = fileSize;
    return true;
#else
    void* p = ::mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, hFile, 0);
    if (p == MAP_FAILED) return false;
    mm.data = p;
    mm.size = fileSize;
    mm.fd = -1; // caller manages the fd
    return true;
#endif
}

// --- System info ---

inline uint64_t TotalMemoryMB() {
#ifdef _WIN32
    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        return ms.ullTotalPhys / (1024ULL * 1024);
    }
    return 4096;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize) / (1024ULL * 1024);
    }
    return 4096;
#endif
}

inline std::string TempDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, buf);
    if (len == 0) return "";
    return std::string(buf, len);
#else
    const char* d = ::getenv("TMPDIR");
    if (!d) d = ::getenv("TMP");
    if (!d) d = ::getenv("TEMPDIR");
    if (!d) d = "/tmp";
    std::string s = d;
    if (!s.empty() && s.back() != '/') s += '/';
    return s;
#endif
}

inline uint64_t ProcessId() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

inline uint64_t TickCount() {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
#endif
}

// --- File metadata ---

inline uint64_t GetModificationTime(const char* path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME ft;
    if (!GetFileTime(h, NULL, NULL, &ft)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // Convert 100-ns intervals to Unix timestamp
    return (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
#else
    struct stat st;
    if (::stat(path, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_mtime);
#endif
}

inline bool SetModificationTime(const char* path, uint64_t unixTime) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LONGLONG ll = static_cast<LONGLONG>(unixTime) * 10000000 + 116444736000000000LL;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ll);
    ft.dwHighDateTime = static_cast<DWORD>(ll >> 32);
    BOOL ok = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ok != 0;
#else
    struct timespec ts[2];
    ts[0].tv_nsec = UTIME_OMIT; // don't change atime
    ts[1].tv_sec = static_cast<time_t>(unixTime);
    ts[1].tv_nsec = 0;
    return ::utimensat(AT_FDCWD, path, ts, 0) == 0;
#endif
}

inline uint32_t GetAttributes(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path);
#else
    struct stat st;
    if (::stat(path, &st) != 0) return 0;
    return static_cast<uint32_t>(st.st_mode);
#endif
}

inline bool SetAttributes(const char* path, uint32_t attrs) {
#ifdef _WIN32
    return SetFileAttributesA(path, attrs) != 0;
#else
    return ::chmod(path, static_cast<mode_t>(attrs & 07777)) == 0;
#endif
}

} // namespace os
