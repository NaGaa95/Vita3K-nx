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

#include <io/filesystem.h>
#include <io/util.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>

#ifdef __SWITCH__
SharedFilePtr open_shared_file(const fs::path &path, int flags) {
    // One writable host stream permits later guest writers without reopening a live reader.
    const auto host_path = path.generic_string();
    FILE *file = fopen(host_path.c_str(), "rb+");
    const bool writable = file != nullptr;
    if (!file && !(flags & SCE_O_WRONLY))
        file = fopen(host_path.c_str(), "rb");
    if (!file)
        return {};
    // Keep read-ahead across positional reads; newlib discards it on seeks in rb+ mode.
    setvbuf(file, nullptr, _IONBF, 0);
    auto shared = std::make_shared<SharedFile>(SharedFile{ FilePtr(file, std::fclose), writable });
    return shared;
}

static int64_t read_host_file(SharedFile &shared, void *data, size_t size, int64_t offset) {
    FILE *file = shared.stream.get();
    clearerr(file);
    int64_t result = -1;
    if (ftello(file) == offset || fseeko(file, offset, SEEK_SET) == 0) {
        const auto bytes = fread(data, 1, size, file);
        result = bytes == 0 && ferror(file) ? -1 : static_cast<int64_t>(bytes);
    }
    return result;
}

SharedFile::ReadCacheEntry *SharedFile::prepare_read_cache(size_t size) {
    auto *entry = &read_cache.front();
    for (auto &candidate : read_cache) {
        if (candidate.size == 0 && candidate.capacity == size)
            return &candidate;
        if (candidate.last_use < entry->last_use)
            entry = &candidate;
    }
    entry->size = 0;
    if (entry->capacity == size)
        return entry;

    read_cache_allocated -= entry->capacity;
    entry->data.reset();
    entry->capacity = 0;
    while (read_cache_allocated + size > read_cache_capacity) {
        ReadCacheEntry *victim = nullptr;
        for (auto &candidate : read_cache) {
            if (&candidate != entry && candidate.capacity && (!victim || candidate.last_use < victim->last_use))
                victim = &candidate;
        }
        read_cache_allocated -= victim->capacity;
        victim->data.reset();
        victim->size = victim->capacity = 0;
        victim->last_use = 0;
    }
    entry->data.reset(new (std::nothrow) uint8_t[size]);
    if (!entry->data)
        return nullptr;
    entry->capacity = size;
    read_cache_allocated += size;
    return entry;
}

void SharedFile::invalidate_read_cache() {
    for (auto &entry : read_cache)
        entry.size = 0;
    sequential_read_end = -1;
    sequential_read_bytes = 0;
}

int64_t SharedFile::read_at(void *data, size_t size, int64_t offset) {
    if (offset < 0 || size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - offset)) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0)
        return 0;
    if (offset != sequential_read_end)
        sequential_read_bytes = 0;

    auto finish = [&](int64_t bytes) {
        if (bytes > 0) {
            sequential_read_end = offset + bytes;
            sequential_read_bytes = std::min<uint64_t>(read_cache_capacity, sequential_read_bytes + bytes);
        }
        return bytes;
    };
    auto *output = static_cast<uint8_t *>(data);
    size_t copied = 0;
    while (copied < size) {
        const int64_t position = offset + static_cast<int64_t>(copied);
        ReadCacheEntry *entry = nullptr;
        for (auto &candidate : read_cache) {
            if (position >= candidate.offset && static_cast<uint64_t>(position - candidate.offset) < candidate.size) {
                entry = &candidate;
                break;
            }
        }
        bool short_read = false;
        if (!entry) {
            const auto remaining = size - copied;
            if (remaining >= 64 * 1024) {
                const auto bytes = read_host_file(*this, output + copied, remaining, position);
                return finish(bytes < 0 ? (copied ? static_cast<int64_t>(copied) : -1) : static_cast<int64_t>(copied) + bytes);
            }

            size_t read_ahead = read_cache_block_size;
            while (read_ahead < read_cache_capacity && read_ahead <= sequential_read_bytes + copied)
                read_ahead *= 2;
            const auto prefix = static_cast<size_t>(position % read_cache_block_size);
            const auto needed = (prefix + remaining + read_cache_block_size - 1) / read_cache_block_size * read_cache_block_size;
            const auto fill_size = std::max(read_ahead, needed);
            entry = prepare_read_cache(fill_size);
            if (!entry) {
                const auto bytes = read_host_file(*this, output + copied, remaining, position);
                return finish(bytes < 0 ? (copied ? static_cast<int64_t>(copied) : -1) : static_cast<int64_t>(copied) + bytes);
            }

            entry->offset = position - prefix;
            const size_t available = std::min<uint64_t>(fill_size, std::numeric_limits<int64_t>::max() - entry->offset);
            const auto bytes = read_host_file(*this, entry->data.get(), available, entry->offset);
            entry->size = bytes > 0 ? static_cast<size_t>(bytes) : 0;
            if (bytes < 0)
                return finish(copied ? static_cast<int64_t>(copied) : -1);
            if (prefix >= entry->size)
                break;
            short_read = entry->size < available;
        }

        entry->last_use = ++read_cache_clock;
        const auto index = static_cast<size_t>(position - entry->offset);
        const auto bytes = std::min(size - copied, entry->size - index);
        std::memcpy(output + copied, entry->data.get() + index, bytes);
        copied += bytes;
        if (short_read)
            break;
    }
    return finish(copied);
}
#endif

#ifdef _WIN32
// To open wide files for Boost.Filesystem, we also need the appropriate wide mode flags for Windows, and normal flags for other OS
const wchar_t *translate_open_mode(const int flags) {
    if (flags & SCE_O_WRONLY) {
        if (flags & SCE_O_RDONLY) {
            if (flags & SCE_O_APPEND) {
                return L"ab+";
            }
            return L"rb+";
        }
        if (flags & SCE_O_APPEND) {
            return L"ab";
        }
        return L"rb+";
    }
    return L"rb";
}
#else
const char *translate_open_mode(const int flags) {
    if (flags & SCE_O_WRONLY) {
        if (flags & SCE_O_RDONLY) {
            if (flags & SCE_O_APPEND) {
                return "ab+";
            }
            return "rb+";
        }
        if (flags & SCE_O_APPEND) {
            return "ab";
        }
        return "rb+";
    }
    return "rb";
}
#endif
