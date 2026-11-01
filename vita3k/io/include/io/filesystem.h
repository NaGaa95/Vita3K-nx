// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <util/fs.h>

#ifdef _WIN32
#include <util/string_utils.h>
#endif

#include <dirent.h>

#include <memory>
#include <cstdint>
#ifdef __SWITCH__
#include <array>
#endif

using FilePtr = std::shared_ptr<FILE>;

#ifdef __SWITCH__
struct SharedFile {
    FilePtr stream;
    bool writable;

    static constexpr size_t read_cache_capacity = 256 * 1024;
    static constexpr size_t read_cache_block_size = 4 * 1024;
    struct ReadCacheEntry {
        std::unique_ptr<uint8_t[]> data;
        int64_t offset = 0;
        size_t size = 0;
        size_t capacity = 0;
        uint64_t last_use = 0;
    };
    std::array<ReadCacheEntry, read_cache_capacity / read_cache_block_size> read_cache;
    size_t read_cache_allocated = 0;
    uint64_t read_cache_clock = 0;
    int64_t sequential_read_end = -1;
    size_t sequential_read_bytes = 0;

    int64_t read_at(void *data, size_t size, int64_t offset);
    ReadCacheEntry *prepare_read_cache(size_t size);
    void invalidate_read_cache();
};

using SharedFilePtr = std::shared_ptr<SharedFile>;
SharedFilePtr open_shared_file(const fs::path &path, int flags);
#endif

// For opening Boost.Filesystem files, Boost returns wide strings for Windows, normal strings for other OS
// Dirent and FILE only accept and return wide char strings for Windows, and normal for other OS
#ifdef _WIN32
const wchar_t *translate_open_mode(const int flags);

inline FilePtr create_shared_file(const fs::path &path, const int open_mode) {
    const auto file = _wfopen(path.generic_wstring().c_str(), translate_open_mode(open_mode));
    return file ? FilePtr(file, std::fclose) : FilePtr();
}

using DirPtr = std::shared_ptr<_WDIR>;

inline DirPtr create_shared_dir(const fs::path &path) {
    return DirPtr(_wopendir(path.generic_wstring().c_str()), _wclosedir);
}

inline std::string get_file_in_dir(const _wdirent *file) {
    return string_utils::wide_to_utf(file->d_name);
}

inline _wdirent *get_system_dir_ptr(const DirPtr &dir) {
    return _wreaddir(dir.get());
}
#else
const char *translate_open_mode(const int flags);

inline FilePtr create_shared_file(const fs::path &path, const int open_mode) {
    const auto file = fopen(path.generic_string().c_str(), translate_open_mode(open_mode));
#ifdef __SWITCH__
    // fsdev reports no st_blksize, so newlib leaves stdio nearly unbuffered and
    // every small guest read becomes a full FS-service IPC round trip. Give
    // guest files a real buffer; large reads still bypass it.
    if (file)
        setvbuf(file, nullptr, _IOFBF, 256 * 1024);
#endif
    return file ? FilePtr(file, std::fclose) : FilePtr();
}

using DirPtr = std::shared_ptr<DIR>;

inline DirPtr create_shared_dir(const fs::path &path) {
    return DirPtr(opendir(path.generic_string().c_str()), closedir);
}

inline std::string get_file_in_dir(dirent *file) {
    return std::string(file->d_name);
}

inline dirent *get_system_dir_ptr(DirPtr dir) {
    return readdir(dir.get());
}
#endif
