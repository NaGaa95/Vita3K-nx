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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <io.h>
#else
#define _FILE_OFFSET_BITS 64
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <io/state.h>
#include <cerrno>
#include <limits>

static const uint32_t page_size = []() -> uint32_t {
#ifdef _WIN32
    SYSTEM_INFO system_info = {};
    GetSystemInfo(&system_info);
    return system_info.dwPageSize;
#else
    return static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
#endif
}();

SceOff FileStats::read(void *input_data, const int element_size, const SceSize element_count) const {
    if (!get_file_pointer())
        return -1;

    if (element_size == 0 || element_count == 0)
        return 0;

#ifdef __SWITCH__
    if (!(get_open_mode() & SCE_O_RDONLY)) {
        errno = EBADF;
        return -1;
    }
    if (element_size < 0 || element_count > std::numeric_limits<size_t>::max() / element_size) {
        errno = EINVAL;
        return -1;
    }
#endif

    // Trigger write protection without changing bytes beyond a short read.
    const size_t requested_bytes = static_cast<size_t>(element_size) * element_count;
    volatile uint8_t *input_addr = reinterpret_cast<volatile uint8_t *>(input_data);
    for (size_t i = 0; i < requested_bytes; i += page_size)
        input_addr[i] = input_addr[i];
    input_addr[requested_bytes - 1] = input_addr[requested_bytes - 1];

#ifdef __SWITCH__
    const auto bytes = shared_file->read_at(input_data, requested_bytes, file_offset);
    if (bytes < 0)
        return -1;
    file_offset += bytes;
    return bytes / element_size;
#else
    return fread(input_data, element_size, element_count, wrapped_file.get());
#endif
}

SceOff FileStats::write(const void *data, const SceSize size, const int count) const {
    if (!can_write_file())
        return -1;

#ifdef __SWITCH__
    if (!(get_open_mode() & SCE_O_WRONLY) || !shared_file || !shared_file->writable) {
        errno = EBADF;
        return -1;
    }
    if (size == 0 || count == 0)
        return 0;
    auto *file = get_file_pointer();
    shared_file->invalidate_read_cache();
    clearerr(file);
    const bool append = get_open_mode() & SCE_O_APPEND;
    if (fseeko(file, append ? 0 : file_offset, append ? SEEK_END : SEEK_SET) != 0)
        return -1;
    const auto result = fwrite(data, size, count, file);
    const auto position = ftello(file);
    if (position < 0)
        return -1;
    file_offset = position;
    if (fflush(file) != 0)
        return -1;
    return result;
#else
    return fwrite(data, size, count, get_file_pointer());
#endif
}

int FileStats::truncate(const SceSize size) const {
#ifdef __SWITCH__
    if (!(get_open_mode() & SCE_O_WRONLY) || !shared_file || !shared_file->writable) {
        errno = EBADF;
        return -1;
    }
    if (fflush(get_file_pointer()) != 0)
        return -1;
    shared_file->invalidate_read_cache();
#endif
#ifdef _WIN32
    return _chsize_s(_fileno(get_file_pointer()), size);
#else
    return ftruncate(fileno(get_file_pointer()), size);
#endif
}

bool FileStats::seek(const SceOff offset, const SceIoSeekMode seek_mode) const {
    if (!get_file_pointer())
        return false;

#ifdef __SWITCH__
    SceOff base = 0;
    switch (seek_mode) {
    case SCE_SEEK_SET: break;
    case SCE_SEEK_CUR: base = file_offset; break;
    case SCE_SEEK_END: {
        struct stat info{};
        if (fstat(fileno(get_file_pointer()), &info) != 0)
            return false;
        base = info.st_size;
        break;
    }
    default: return false;
    }
    if (offset < -base || (offset > 0 && base > std::numeric_limits<SceOff>::max() - offset)) {
        errno = EINVAL;
        return false;
    }
    file_offset = base + offset;
    return true;
#else

    auto base = SEEK_SET;
    switch (seek_mode) {
    case SCE_SEEK_SET:
        base = SEEK_SET;
        break;
    case SCE_SEEK_CUR:
        base = SEEK_CUR;
        break;
    case SCE_SEEK_END:
        base = SEEK_END;
        break;
    default:
        return false;
    }

#ifdef _WIN32
    return _fseeki64(wrapped_file.get(), offset, base) == 0;
#else
    return fseeko(wrapped_file.get(), offset, base) == 0;
#endif
#endif
}

SceOff FileStats::tell() const {
    if (!get_file_pointer())
        return -1;

#ifdef __SWITCH__
    return file_offset;
#elif defined(_WIN32)
    return _ftelli64(wrapped_file.get());
#else
    return ftello(wrapped_file.get());
#endif
}
