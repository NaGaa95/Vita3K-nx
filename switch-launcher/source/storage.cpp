// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived in part from Dolphin-NX and Cemu-NX storage code.

#include "storage.h"

#include <switch.h>
#include <usbhsfs.h>

// libsmb2.h uses constants and types declared by smb2.h.
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
// clang-format on

#include <sys/iosupport.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Vita3KLauncher::Storage {
namespace {

constexpr std::size_t SMB_READ_AHEAD_MIN_BYTES = 64 * 1024;
constexpr std::size_t SMB_READ_AHEAD_MAX_BYTES = 1024 * 1024;
constexpr std::size_t SMB_READ_AHEAD_BUDGET = 16 * 1024 * 1024;
constexpr auto SMB_DIRECTORY_CACHE_LIFETIME = std::chrono::seconds(3);
constexpr std::size_t SMB_DIRECTORY_CACHE_LIMIT = 32;
constexpr std::size_t SMB_DIRECTORY_ENTRY_LIMIT = 4096;
constexpr std::uint64_t MAX_FILE_POSITION = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

static_assert(std::numeric_limits<off_t>::is_signed,
    "The SMB devoptab requires a signed off_t");

struct SmbMount;
struct SmbDevice;

struct CachedDirectoryEntry {
    std::string name;
    struct stat info{};
};

struct CachedDirectory {
    std::vector<CachedDirectoryEntry> entries;
    std::chrono::steady_clock::time_point expires;
    bool complete = false;
};

struct SmbFile {
    // devoptab supplies raw unconstructed storage. Keep the shared owner in a
    // separately constructed object so an open descriptor pins its mount even
    // after the registration has been tombstoned.
    std::shared_ptr<SmbMount> *lifetime = nullptr;
    SmbMount *mount = nullptr;
    smb2fh *handle = nullptr;
    std::uint8_t *read_ahead = nullptr;
    std::size_t read_ahead_capacity = 0;
    std::size_t read_ahead_offset = 0;
    std::size_t read_ahead_size = 0;
    std::size_t read_ahead_window = SMB_READ_AHEAD_MIN_BYTES;
    std::uint64_t position = 0;
    bool append = false;
};

struct SmbDir {
    std::shared_ptr<SmbMount> *lifetime = nullptr;
    SmbMount *mount = nullptr;
    smb2dir *handle = nullptr;
    std::vector<CachedDirectoryEntry> *entries = nullptr;
    std::size_t entry_index = 0;
    bool from_cache = false;
    bool complete = false;
    char path[PATH_MAX]{};
};

struct SmbMount {
    SmbShare config;
    SmbDevice *device = nullptr;
    smb2_context *context = nullptr;
    bool connected = false;
    std::atomic_bool retired{false};
    std::atomic<SmbConnectionState> state{ SmbConnectionState::Disconnected };
    std::string last_error;
    std::mutex io_mutex;
    std::size_t read_ahead_bytes = 0;
    std::unordered_map<std::string, CachedDirectory> directory_cache;

    ~SmbMount() {
        if (!context)
            return;
        if (connected)
            smb2_disconnect_share(context);
        smb2_destroy_context(context);
    }
};

// Newlib may have dispatched a callback using deviceData before RemoveDevice
// returns. Keep every registration record at a stable address for the process
// lifetime and tombstone its shared owner on unmount; a remount receives a new
// record, so an old callback can never acquire the replacement connection.
struct SmbDevice {
    std::string device_name;
    std::string root_path;
    devoptab_t devoptab{};
    std::shared_ptr<SmbMount> mount;
};

std::mutex s_mount_mutex;
std::vector<std::shared_ptr<SmbMount>> s_smb_mounts;
std::vector<std::unique_ptr<SmbDevice>> s_smb_devices;
struct SmbStatusRecord {
    SmbConnectionState state = SmbConnectionState::Disconnected;
    std::string error;
};
std::unordered_map<std::string, SmbStatusRecord> s_smb_status;
bool s_usb_initialized = false;
bool s_shutdown_requested = false;
std::atomic<std::uint64_t> s_usb_generation{ 0 };
std::mutex s_usb_mutex;
std::vector<UsbHsFsDevice> s_usb_devices;
std::mutex s_usb_callback_mutex;
UsbStatusCallback s_usb_callback = nullptr;
void *s_usb_callback_data = nullptr;

void ClearError(std::string *error) {
    if (error)
        error->clear();
}

void SetSmbStatus(const std::string &id, SmbConnectionState state,
                  const std::string &error = {}) {
    std::lock_guard lock(s_mount_mutex);
    s_smb_status[id] = {state, error};
}

void SetErrno(_reent *reent, int error) {
    if (reent)
        reent->_errno = error;
}

int Fail(_reent *reent, int error) {
    SetErrno(reent, error > 0 ? error : EIO);
    return -1;
}

void Succeed(_reent *reent) {
    SetErrno(reent, 0);
}

bool ValidId(const std::string &id) {
    if (id.empty() || id.size() > 16)
        return false;
    return std::all_of(id.begin(), id.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '_';
    });
}

std::string DeviceNameForId(const std::string &id) {
    return ValidId(id) ? "v3ksmb_" + id : std::string{};
}

template <typename Destination, typename Source>
bool CheckedAssign(Destination &destination, const Source source) {
    static_assert(std::is_integral_v<Destination> && std::is_integral_v<Source>);

    if constexpr (std::is_signed_v<Source>) {
        if (source < 0) {
            if constexpr (!std::is_signed_v<Destination>) {
                return false;
            } else {
                if (static_cast<std::intmax_t>(source)
                    < static_cast<std::intmax_t>(std::numeric_limits<Destination>::min()))
                    return false;
            }
        } else if (static_cast<std::uintmax_t>(source)
            > static_cast<std::uintmax_t>(std::numeric_limits<Destination>::max())) {
            return false;
        }
    } else if (static_cast<std::uintmax_t>(source)
        > static_cast<std::uintmax_t>(std::numeric_limits<Destination>::max())) {
        return false;
    }

    destination = static_cast<Destination>(source);
    return true;
}

// Converts a devoptab path to the share-relative form expected by libsmb2 and
// verifies that a second path (notably rename's destination) cannot escape to a
// different registered device.
int NormalizePath(const SmbMount *mount, const char *source, char *destination,
    const std::size_t destination_size) {
    if (!mount || !mount->device || !source || !destination || destination_size == 0)
        return EFAULT;

    const char *colon = std::strchr(source, ':');
    if (!colon)
        return EINVAL;
    const std::size_t device_length = static_cast<std::size_t>(colon - source);
    if (device_length != mount->device->device_name.size()
        || std::memcmp(source, mount->device->device_name.data(), device_length) != 0) {
        return EXDEV;
    }

    const char *input = colon + 1;
    while (*input == '/' || *input == '\\')
        ++input;

    std::size_t length = 0;
    bool previous_was_slash = false;
    for (; *input; ++input) {
        const bool is_slash = *input == '/' || *input == '\\';
        if (is_slash && previous_was_slash)
            continue;
        previous_was_slash = is_slash;
        if (length + 1 >= destination_size)
            return ENAMETOOLONG;
        destination[length++] = is_slash ? '/' : *input;
    }
    while (length != 0 && destination[length - 1] == '/')
        --length;
    destination[length] = '\0';
    return 0;
}

int FillStat(struct stat *output, const smb2_stat_64 &input) {
    if (!output)
        return EFAULT;
    if (input.smb2_size > MAX_FILE_POSITION)
        return EOVERFLOW;

    std::memset(output, 0, sizeof(*output));
    switch (input.smb2_type) {
    case SMB2_TYPE_FILE:
        output->st_mode = S_IFREG | 0666;
        break;
    case SMB2_TYPE_DIRECTORY:
        output->st_mode = S_IFDIR | 0777;
        break;
    case SMB2_TYPE_LINK:
        output->st_mode = S_IFLNK | 0777;
        break;
    default:
        output->st_mode = S_IFREG | 0444;
        break;
    }

    const auto links = input.smb2_nlink ? input.smb2_nlink : 1;
    if (!CheckedAssign(output->st_ino, input.smb2_ino)
        || !CheckedAssign(output->st_nlink, links)
        || !CheckedAssign(output->st_size, input.smb2_size)
        || !CheckedAssign(output->st_atime, input.smb2_atime)
        || !CheckedAssign(output->st_mtime, input.smb2_mtime)
        || !CheckedAssign(output->st_ctime, input.smb2_ctime)
        || !CheckedAssign(output->st_blksize, 65536)) {
        std::memset(output, 0, sizeof(*output));
        return EOVERFLOW;
    }
    return 0;
}

std::shared_ptr<SmbMount> MountFrom(_reent *reent) {
    auto *device=reent?static_cast<SmbDevice *>(reent->deviceData):nullptr;
    if(!device)return {};
    std::lock_guard lock(s_mount_mutex);
    std::shared_ptr<SmbMount> mount=device->mount;
    if(!mount||mount->retired.load(std::memory_order_acquire))return {};
    return mount;
}

std::shared_ptr<SmbMount> MountFrom(const SmbFile *file) {
    return file&&file->lifetime?*file->lifetime:std::shared_ptr<SmbMount>{};
}

std::shared_ptr<SmbMount> MountFrom(const SmbDir *directory) {
    return directory&&directory->lifetime?*directory->lifetime:std::shared_ptr<SmbMount>{};
}

bool PinMount(SmbFile *file,std::shared_ptr<SmbMount> mount) {
    file->lifetime=new(std::nothrow)std::shared_ptr<SmbMount>(std::move(mount));
    if(!file->lifetime)return false;
    file->mount=file->lifetime->get();return true;
}

bool PinMount(SmbDir *directory,std::shared_ptr<SmbMount> mount) {
    directory->lifetime=new(std::nothrow)std::shared_ptr<SmbMount>(std::move(mount));
    if(!directory->lifetime)return false;
    directory->mount=directory->lifetime->get();return true;
}

void ReleaseMount(SmbFile *file) {
    auto *lifetime=file->lifetime;*file={};delete lifetime;
}

void ReleaseMount(SmbDir *directory) {
    auto *lifetime=directory->lifetime;*directory={};delete lifetime;
}

bool EnsureReadAhead(SmbFile *file, std::size_t capacity) {
    if (file->read_ahead_capacity >= capacity)
        return true;
    const std::size_t growth = capacity - file->read_ahead_capacity;
    if (file->mount->read_ahead_bytes > SMB_READ_AHEAD_BUDGET
        || growth > SMB_READ_AHEAD_BUDGET - file->mount->read_ahead_bytes) {
        return false;
    }
    void *memory = std::realloc(file->read_ahead, capacity);
    if (!memory)
        return false;
    file->read_ahead = static_cast<std::uint8_t *>(memory);
    file->mount->read_ahead_bytes += growth;
    file->read_ahead_capacity = capacity;
    return true;
}

void ReleaseReadAhead(SmbFile *file) {
    if (!file->read_ahead)
        return;
    std::free(file->read_ahead);
    file->read_ahead = nullptr;
    if (file->mount->read_ahead_bytes >= file->read_ahead_capacity)
        file->mount->read_ahead_bytes -= file->read_ahead_capacity;
    else
        file->mount->read_ahead_bytes = 0;
    file->read_ahead_capacity = 0;
    file->read_ahead_offset = 0;
    file->read_ahead_size = 0;
}

void DiscardReadAhead(SmbFile *file) {
    file->read_ahead_offset = 0;
    file->read_ahead_size = 0;
}

int SynchronizeFilePosition(SmbFile *file) {
    if (file->read_ahead_size == 0)
        return 0;
    if (file->position > static_cast<std::uint64_t>(INT64_MAX))
        return -EOVERFLOW;
    std::uint64_t position = 0;
    const std::int64_t result = smb2_lseek(file->mount->context, file->handle,
        static_cast<std::int64_t>(file->position), SEEK_SET, &position);
    if (result < 0)
        return static_cast<int>(result);
    if (position != file->position)
        return -EIO;
    DiscardReadAhead(file);
    return 0;
}

int PositionForAppend(SmbFile *file) {
    smb2_stat_64 info{};
    const int stat_result = smb2_fstat(file->mount->context, file->handle, &info);
    if (stat_result < 0)
        return stat_result;
    if (info.smb2_size > MAX_FILE_POSITION
        || info.smb2_size > static_cast<std::uint64_t>(INT64_MAX)) {
        return -EOVERFLOW;
    }

    std::uint64_t position = 0;
    const std::int64_t seek_result = smb2_lseek(file->mount->context, file->handle,
        static_cast<std::int64_t>(info.smb2_size), SEEK_SET, &position);
    if (seek_result < 0)
        return static_cast<int>(seek_result);
    if (position != info.smb2_size)
        return -EIO;
    file->position = position;
    DiscardReadAhead(file);
    return 0;
}

int AddFileOffset(const std::uint64_t base, const off_t displacement,
    std::uint64_t *result) {
    if (!result)
        return EFAULT;
    if (displacement >= 0) {
        const std::uint64_t positive = static_cast<std::uint64_t>(displacement);
        if (positive > MAX_FILE_POSITION - std::min(base, MAX_FILE_POSITION))
            return EOVERFLOW;
        *result = base + positive;
    } else {
        // -(OFF_MIN) is not representable, so form the magnitude without
        // negating the minimum signed value directly.
        const std::uint64_t magnitude = static_cast<std::uint64_t>(-(displacement + 1)) + 1;
        if (magnitude > base)
            return EINVAL;
        *result = base - magnitude;
    }
    if (*result > MAX_FILE_POSITION)
        return EOVERFLOW;
    return 0;
}

int SmbOpen(_reent *reent, void *state, const char *source, int flags, int) {
    auto *file = static_cast<SmbFile *>(state);
    if (!file)
        return Fail(reent, EFAULT);
    *file = {};

    std::shared_ptr<SmbMount> mount = MountFrom(reent);
    if (!mount)
        return Fail(reent, ENODEV);
    char path[PATH_MAX]{};
    const int path_error = NormalizePath(mount.get(), source, path, sizeof(path));
    if (path_error != 0)
        return Fail(reent, path_error);

    std::lock_guard lock(mount->io_mutex);
    file->handle = smb2_open(mount->context, path, flags);
    if (!file->handle)
        return Fail(reent, EIO);
    if(!PinMount(file,mount)){
        smb2_close(mount->context,file->handle);*file={};return Fail(reent,ENOMEM);
    }
    file->append = (flags & O_APPEND) != 0;
    if (file->append) {
        const int result = PositionForAppend(file);
        if (result < 0) {
            smb2_close(mount->context, file->handle);
            ReleaseMount(file);
            return Fail(reent, -result);
        }
    }
    Succeed(reent);
    return 0;
}

int SmbClose(_reent *reent, void *state) {
    auto *file = static_cast<SmbFile *>(state);
    std::shared_ptr<SmbMount> mount = MountFrom(file);
    if (!file || !mount || !file->handle)
        return Fail(reent, EBADF);

    int result = 0;
    {
        std::lock_guard lock(mount->io_mutex);
        ReleaseReadAhead(file);
        result = smb2_close(mount->context, file->handle);
        ReleaseMount(file);
    }
    if (result < 0)
        return Fail(reent, -result);
    Succeed(reent);
    return 0;
}

ssize_t SmbRead(_reent *reent, void *state, char *output, std::size_t length) {
    auto *file = static_cast<SmbFile *>(state);
    std::shared_ptr<SmbMount> mount = MountFrom(file);
    if (!file || !mount || !file->handle)
        return Fail(reent, EBADF);
    if (length > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))
        return Fail(reent, EOVERFLOW);
    if (length != 0 && !output)
        return Fail(reent, EFAULT);
    if (length == 0) {
        Succeed(reent);
        return 0;
    }

    std::lock_guard lock(mount->io_mutex);
    const std::uint32_t maximum = std::max<std::uint32_t>(1, smb2_get_max_read_size(mount->context));
    std::size_t total = 0;

    if (file->read_ahead_offset < file->read_ahead_size) {
        const std::uint64_t representable = MAX_FILE_POSITION - file->position;
        const std::size_t cached = std::min({ length,
            file->read_ahead_size - file->read_ahead_offset,
            static_cast<std::size_t>(std::min<std::uint64_t>(
                representable, std::numeric_limits<std::size_t>::max())) });
        if (cached != 0) {
            std::memcpy(output, file->read_ahead + file->read_ahead_offset, cached);
            file->read_ahead_offset += cached;
            file->position += cached;
            total += cached;
        }
        if (file->read_ahead_offset == file->read_ahead_size)
        {
            DiscardReadAhead(file);
            file->read_ahead_window = std::min(file->read_ahead_window * 2,
                SMB_READ_AHEAD_MAX_BYTES);
        }
    }

    while (total < length) {
        const std::uint64_t representable = MAX_FILE_POSITION - file->position;
        if (representable == 0) {
            if (total == 0)
                return Fail(reent, EOVERFLOW);
            break;
        }
        const std::size_t remaining = length - total;
        const std::size_t wanted = std::min(file->read_ahead_window,
            static_cast<std::size_t>(maximum));
        const bool use_read_ahead = remaining < wanted && EnsureReadAhead(file, wanted);
        const std::size_t desired = use_read_ahead ? wanted : remaining;
        const std::uint32_t amount = static_cast<std::uint32_t>(std::min({ desired, static_cast<std::size_t>(maximum),
            static_cast<std::size_t>(std::min<std::uint64_t>(
                representable, std::numeric_limits<std::size_t>::max())) }));
        if (amount == 0) {
            if (total == 0)
                return Fail(reent, EOVERFLOW);
            break;
        }

        std::uint8_t *destination = use_read_ahead ? file->read_ahead
                                                   : reinterpret_cast<std::uint8_t *>(output + total);
        const int result = smb2_read(mount->context, file->handle, destination, amount);
        if (result < 0) {
            if (total == 0)
                return Fail(reent, -result);
            Succeed(reent);
            return static_cast<ssize_t>(total);
        }
        if (result == 0)
            break;
        if (result > static_cast<int>(amount)) {
            if (total == 0)
                return Fail(reent, EIO);
            break;
        }

        const std::size_t bytes_read = static_cast<std::size_t>(result);
        if (use_read_ahead) {
            file->read_ahead_offset = 0;
            file->read_ahead_size = bytes_read;
            const std::size_t copied = std::min(remaining, bytes_read);
            std::memcpy(output + total, file->read_ahead, copied);
            file->read_ahead_offset = copied;
            file->position += copied;
            total += copied;
            if (file->read_ahead_offset == file->read_ahead_size)
            {
                DiscardReadAhead(file);
                file->read_ahead_window = std::min(file->read_ahead_window * 2,
                    SMB_READ_AHEAD_MAX_BYTES);
            }
            break;
        }

        file->position += bytes_read;
        total += bytes_read;
        if (bytes_read < amount)
            break;
    }

    Succeed(reent);
    return static_cast<ssize_t>(total);
}

ssize_t SmbWrite(_reent *reent, void *state, const char *input, std::size_t length) {
    auto *file = static_cast<SmbFile *>(state);
    std::shared_ptr<SmbMount> mount = MountFrom(file);
    if (!file || !mount || !file->handle)
        return Fail(reent, EBADF);
    if (length > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))
        return Fail(reent, EOVERFLOW);
    if (length != 0 && !input)
        return Fail(reent, EFAULT);
    if (length == 0) {
        Succeed(reent);
        return 0;
    }

    std::lock_guard lock(mount->io_mutex);
    const int positioned = file->append ? PositionForAppend(file)
                                        : SynchronizeFilePosition(file);
    if (positioned < 0)
        return Fail(reent, -positioned);

    const std::uint32_t maximum = std::max<std::uint32_t>(1, smb2_get_max_write_size(mount->context));
    std::size_t total = 0;
    while (total < length) {
        const std::uint64_t representable = MAX_FILE_POSITION - file->position;
        if (representable == 0) {
            if (total == 0)
                return Fail(reent, EFBIG);
            break;
        }
        const std::uint32_t amount = static_cast<std::uint32_t>(std::min({ length - total, static_cast<std::size_t>(maximum),
            static_cast<std::size_t>(std::min<std::uint64_t>(
                representable, std::numeric_limits<std::size_t>::max())) }));
        if (amount == 0) {
            if (total == 0)
                return Fail(reent, EFBIG);
            break;
        }
        const int result = smb2_write(mount->context, file->handle,
            reinterpret_cast<const std::uint8_t *>(input + total), amount);
        if (result < 0) {
            if (total == 0)
                return Fail(reent, -result);
            Succeed(reent);
            return static_cast<ssize_t>(total);
        }
        if (result == 0 || result > static_cast<int>(amount)) {
            if (total == 0)
                return Fail(reent, EIO);
            break;
        }
        const std::size_t bytes_written = static_cast<std::size_t>(result);
        total += bytes_written;
        file->position += bytes_written;
    }

    Succeed(reent);
    return static_cast<ssize_t>(total);
}

off_t SmbSeek(_reent *reent, void *state, off_t displacement, int origin) {
    auto *file = static_cast<SmbFile *>(state);
    std::shared_ptr<SmbMount> mount = MountFrom(file);
    if (!file || !mount || !file->handle) {
        Fail(reent, EBADF);
        return static_cast<off_t>(-1);
    }
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        Fail(reent, EINVAL);
        return static_cast<off_t>(-1);
    }

    std::lock_guard lock(mount->io_mutex);
    std::uint64_t base = 0;
    if (origin == SEEK_CUR) {
        base = file->position;
    } else if (origin == SEEK_END) {
        smb2_stat_64 info{};
        const int result = smb2_fstat(mount->context, file->handle, &info);
        if (result < 0) {
            Fail(reent, -result);
            return static_cast<off_t>(-1);
        }
        if (info.smb2_size > MAX_FILE_POSITION) {
            Fail(reent, EOVERFLOW);
            return static_cast<off_t>(-1);
        }
        base = info.smb2_size;
    }

    std::uint64_t target = 0;
    const int offset_error = AddFileOffset(base, displacement, &target);
    if (offset_error != 0) {
        Fail(reent, offset_error);
        return static_cast<off_t>(-1);
    }

    if (file->read_ahead_size != 0
        && file->read_ahead_offset <= file->position) {
        const std::uint64_t cache_start = file->position - file->read_ahead_offset;
        const std::uint64_t cache_end = cache_start + file->read_ahead_size;
        if (cache_end >= cache_start && target >= cache_start && target <= cache_end) {
            file->read_ahead_offset = static_cast<std::size_t>(target - cache_start);
            file->position = target;
            if (file->read_ahead_offset == file->read_ahead_size)
                DiscardReadAhead(file);
            Succeed(reent);
            return static_cast<off_t>(target);
        }
    }

    std::uint64_t result_position = 0;
    const std::int64_t result = smb2_lseek(mount->context, file->handle,
        static_cast<std::int64_t>(target), SEEK_SET, &result_position);
    if (result < 0) {
        Fail(reent, static_cast<int>(-result));
        return static_cast<off_t>(-1);
    }
    if (result_position != target || result_position > MAX_FILE_POSITION) {
        // The requested target was representable, so a different response is a
        // protocol/library inconsistency. Restore the logical cursor and drop
        // any stale read-ahead state before returning an error.
        std::uint64_t ignored = 0;
        smb2_lseek(mount->context, file->handle,
            static_cast<std::int64_t>(file->position), SEEK_SET, &ignored);
        DiscardReadAhead(file);
        file->read_ahead_window = SMB_READ_AHEAD_MIN_BYTES;
        Fail(reent, result_position > MAX_FILE_POSITION ? EOVERFLOW : EIO);
        return static_cast<off_t>(-1);
    }

    DiscardReadAhead(file);
    file->read_ahead_window = SMB_READ_AHEAD_MIN_BYTES;
    file->position = result_position;
    Succeed(reent);
    return static_cast<off_t>(result_position);
}

int SmbFstat(_reent *reent, void *state, struct stat *output) {
    auto *file = static_cast<SmbFile *>(state);
    std::shared_ptr<SmbMount> mount = MountFrom(file);
    if (!file || !mount || !file->handle)
        return Fail(reent, EBADF);
    if (!output)
        return Fail(reent, EFAULT);

    std::lock_guard lock(mount->io_mutex);
    smb2_stat_64 info{};
    const int result = smb2_fstat(mount->context, file->handle, &info);
    if (result < 0)
        return Fail(reent, -result);
    const int stat_error = FillStat(output, info);
    if (stat_error != 0)
        return Fail(reent, stat_error);
    Succeed(reent);
    return 0;
}

int SmbStat(_reent *reent, const char *source, struct stat *output) {
    std::shared_ptr<SmbMount> mount = MountFrom(reent);
    if (!mount)
        return Fail(reent, ENODEV);
    if (!output)
        return Fail(reent, EFAULT);
    char path[PATH_MAX]{};
    const int path_error = NormalizePath(mount.get(), source, path, sizeof(path));
    if (path_error != 0)
        return Fail(reent, path_error);

    if (path[0] == '\0') {
        std::memset(output, 0, sizeof(*output));
        output->st_mode = S_IFDIR | 0777;
        output->st_nlink = 1;
        Succeed(reent);
        return 0;
    }

    std::lock_guard lock(mount->io_mutex);
    // dirnext already receives complete SMB stat metadata. Publish it while a
    // directory is being enumerated so the launcher's immediate stat() for the
    // same entry remains a local lookup instead of one network request per row.
    const std::string full_path(path);
    const std::size_t slash = full_path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? std::string{} : full_path.substr(0, slash);
    const std::string name = slash == std::string::npos ? full_path : full_path.substr(slash + 1);
    const auto cached_directory = mount->directory_cache.find(parent);
    if (cached_directory != mount->directory_cache.end() &&
        cached_directory->second.expires > std::chrono::steady_clock::now()) {
        const auto cached_entry = std::find_if(cached_directory->second.entries.begin(),
            cached_directory->second.entries.end(), [&name](const CachedDirectoryEntry &entry) {
                return entry.name == name;
            });
        if (cached_entry != cached_directory->second.entries.end()) {
            *output = cached_entry->info;
            Succeed(reent);
            return 0;
        }
    }
    smb2_stat_64 info{};
    const int result = smb2_stat(mount->context, path, &info);
    if (result < 0)
        return Fail(reent, -result);
    const int stat_error = FillStat(output, info);
    if (stat_error != 0)
        return Fail(reent, stat_error);
    Succeed(reent);
    return 0;
}

template <typename Operation>
int PathOperation(_reent *reent, const char *source, Operation operation,
    const int root_error = EBUSY) {
    std::shared_ptr<SmbMount> mount = MountFrom(reent);
    if (!mount)
        return Fail(reent, ENODEV);
    char path[PATH_MAX]{};
    const int path_error = NormalizePath(mount.get(), source, path, sizeof(path));
    if (path_error != 0)
        return Fail(reent, path_error);
    if (path[0] == '\0')
        return Fail(reent, root_error);

    std::lock_guard lock(mount->io_mutex);
    const int result = operation(mount.get(), path);
    if (result < 0)
        return Fail(reent, -result);
    mount->directory_cache.clear();
    Succeed(reent);
    return 0;
}

int SmbUnlink(_reent *reent, const char *path) {
    return PathOperation(reent, path, [](SmbMount *mount, const char *fixed) {
        return smb2_unlink(mount->context, fixed);
    });
}

int SmbMkdir(_reent *reent, const char *path, int) {
    return PathOperation(reent, path, [](SmbMount *mount, const char *fixed) { return smb2_mkdir(mount->context, fixed); }, EEXIST);
}

int SmbRmdir(_reent *reent, const char *path) {
    return PathOperation(reent, path, [](SmbMount *mount, const char *fixed) {
        return smb2_rmdir(mount->context, fixed);
    });
}

int SmbRename(_reent *reent, const char *source, const char *destination) {
    std::shared_ptr<SmbMount> mount = MountFrom(reent);
    if (!mount)
        return Fail(reent, ENODEV);
    char old_path[PATH_MAX]{};
    char new_path[PATH_MAX]{};
    const int old_error = NormalizePath(mount.get(), source, old_path, sizeof(old_path));
    if (old_error != 0)
        return Fail(reent, old_error);
    const int new_error = NormalizePath(mount.get(), destination, new_path, sizeof(new_path));
    if (new_error != 0)
        return Fail(reent, new_error);
    if (old_path[0] == '\0' || new_path[0] == '\0')
        return Fail(reent, EBUSY);

    std::lock_guard lock(mount->io_mutex);
    const int result = smb2_rename(mount->context, old_path, new_path);
    if (result < 0)
        return Fail(reent, -result);
    mount->directory_cache.clear();
    Succeed(reent);
    return 0;
}

DIR_ITER *SmbDirOpen(_reent *reent, DIR_ITER *state, const char *source) {
    auto *directory = state ? static_cast<SmbDir *>(state->dirStruct) : nullptr;
    if (!directory) {
        Fail(reent, EFAULT);
        return nullptr;
    }
    *directory = {};

    std::shared_ptr<SmbMount> mount = MountFrom(reent);
    if (!mount) {
        Fail(reent, ENODEV);
        return nullptr;
    }
    char path[PATH_MAX]{};
    const int path_error = NormalizePath(mount.get(), source, path, sizeof(path));
    if (path_error != 0) {
        Fail(reent, path_error);
        return nullptr;
    }

    std::lock_guard lock(mount->io_mutex);
    const auto cached = mount->directory_cache.find(path);
    if (cached != mount->directory_cache.end() && cached->second.complete &&
        cached->second.expires > std::chrono::steady_clock::now()) {
        directory->entries = new (std::nothrow) std::vector<CachedDirectoryEntry>(cached->second.entries);
        if (!directory->entries) { Fail(reent, ENOMEM); return nullptr; }
        if(!PinMount(directory,mount)){delete directory->entries;*directory={};Fail(reent,ENOMEM);return nullptr;}
        directory->from_cache = true; directory->complete = true;
        std::snprintf(directory->path, sizeof(directory->path), "%s", path);
        Succeed(reent); return state;
    }
    if (cached != mount->directory_cache.end()) mount->directory_cache.erase(cached);
    directory->handle = smb2_opendir(mount->context, path);
    if (!directory->handle) {
        Fail(reent, EIO);
        return nullptr;
    }
    directory->entries = new (std::nothrow) std::vector<CachedDirectoryEntry>();
    if (!directory->entries) { smb2_closedir(mount->context, directory->handle); *directory={}; Fail(reent,ENOMEM); return nullptr; }
    if(!PinMount(directory,mount)){
        smb2_closedir(mount->context,directory->handle);delete directory->entries;*directory={};
        Fail(reent,ENOMEM);return nullptr;
    }
    std::snprintf(directory->path, sizeof(directory->path), "%s", path);
    if(mount->directory_cache.size()>=SMB_DIRECTORY_CACHE_LIMIT)
        mount->directory_cache.erase(mount->directory_cache.begin());
    mount->directory_cache[path]={{},std::chrono::steady_clock::now()+SMB_DIRECTORY_CACHE_LIFETIME,false};
    Succeed(reent);
    return state;
}

int SmbDirReset(_reent *reent, DIR_ITER *state) {
    auto *directory = state ? static_cast<SmbDir *>(state->dirStruct) : nullptr;
    std::shared_ptr<SmbMount> mount = MountFrom(directory);
    if (!directory || !mount || (!directory->handle && !directory->from_cache))
        return Fail(reent, EBADF);
    std::lock_guard lock(mount->io_mutex);
    directory->entry_index=0;
    if(directory->from_cache){Succeed(reent);return 0;}
    smb2_rewinddir(mount->context, directory->handle);
    if(directory->entries)directory->entries->clear();
    directory->complete=false;
    Succeed(reent);
    return 0;
}

int SmbDirNext(_reent *reent, DIR_ITER *state, char *name, struct stat *output) {
    auto *directory = state ? static_cast<SmbDir *>(state->dirStruct) : nullptr;
    std::shared_ptr<SmbMount> mount = MountFrom(directory);
    if (!directory || !mount || (!directory->handle && !directory->from_cache))
        return Fail(reent, EBADF);
    if (!name || !output)
        return Fail(reent, EFAULT);

    std::lock_guard lock(mount->io_mutex);
    if(directory->from_cache){
        if(!directory->entries||directory->entry_index>=directory->entries->size())return Fail(reent,ENOENT);
        const CachedDirectoryEntry &entry=(*directory->entries)[directory->entry_index++];
        if(entry.name.size()>NAME_MAX)return Fail(reent,ENAMETOOLONG);
        std::memcpy(name,entry.name.c_str(),entry.name.size()+1);*output=entry.info;Succeed(reent);return 0;
    }
    const smb2dirent *entry = smb2_readdir(mount->context, directory->handle);
    if (!entry) {
        directory->complete=true;
        if(directory->entries){
            if(mount->directory_cache.size()>=SMB_DIRECTORY_CACHE_LIMIT)mount->directory_cache.erase(mount->directory_cache.begin());
            mount->directory_cache[directory->path]={*directory->entries,std::chrono::steady_clock::now()+SMB_DIRECTORY_CACHE_LIFETIME,true};
        }
        return Fail(reent, ENOENT);
    }
    if (!entry->name || std::strlen(entry->name) > NAME_MAX)
        return Fail(reent, ENAMETOOLONG);
    std::memcpy(name, entry->name, std::strlen(entry->name) + 1);
    const int stat_error = FillStat(output, entry->st);
    if (stat_error != 0)
        return Fail(reent, stat_error);
    if(directory->entries&&directory->entries->size()<SMB_DIRECTORY_ENTRY_LIMIT){
        const CachedDirectoryEntry cached_entry{entry->name,*output};
        directory->entries->push_back(cached_entry);
        auto &incremental=mount->directory_cache[directory->path];
        if(incremental.entries.size()<SMB_DIRECTORY_ENTRY_LIMIT)
            incremental.entries.push_back(cached_entry);
        incremental.expires=std::chrono::steady_clock::now()+SMB_DIRECTORY_CACHE_LIFETIME;
        incremental.complete=false;
    }
    Succeed(reent);
    return 0;
}

int SmbDirClose(_reent *reent, DIR_ITER *state) {
    auto *directory = state ? static_cast<SmbDir *>(state->dirStruct) : nullptr;
    std::shared_ptr<SmbMount> mount = MountFrom(directory);
    if (!directory || !mount || (!directory->handle && !directory->from_cache))
        return Fail(reent, EBADF);

    {
        std::lock_guard lock(mount->io_mutex);
        if(directory->handle)smb2_closedir(mount->context, directory->handle);
        delete directory->entries;directory->entries=nullptr;
        ReleaseMount(directory);
    }
    Succeed(reent);
    return 0;
}

int SmbStatvfs(_reent *reent, const char *source, struct statvfs *output) {
    std::shared_ptr<SmbMount> mount = MountFrom(reent);
    if (!mount)
        return Fail(reent, ENODEV);
    if (!output)
        return Fail(reent, EFAULT);
    char path[PATH_MAX]{};
    const int path_error = NormalizePath(mount.get(), source, path, sizeof(path));
    if (path_error != 0)
        return Fail(reent, path_error);

    std::lock_guard lock(mount->io_mutex);
    struct smb2_statvfs info{};
    const int result = smb2_statvfs(mount->context, path, &info);
    if (result < 0)
        return Fail(reent, -result);
    std::memset(output, 0, sizeof(*output));
    if (!CheckedAssign(output->f_bsize, info.f_bsize)
        || !CheckedAssign(output->f_frsize, info.f_frsize)
        || !CheckedAssign(output->f_blocks, info.f_blocks)
        || !CheckedAssign(output->f_bfree, info.f_bfree)
        || !CheckedAssign(output->f_bavail, info.f_bavail)
        || !CheckedAssign(output->f_files, info.f_files)
        || !CheckedAssign(output->f_ffree, info.f_ffree)
        || !CheckedAssign(output->f_favail, info.f_favail)
        || !CheckedAssign(output->f_fsid, info.f_fsid)
        || !CheckedAssign(output->f_flag, info.f_flag)
        || !CheckedAssign(output->f_namemax, info.f_namemax)) {
        std::memset(output, 0, sizeof(*output));
        return Fail(reent, EOVERFLOW);
    }
    Succeed(reent);
    return 0;
}

int SmbTruncate(_reent *reent, void *state, off_t length) {
    auto *file = static_cast<SmbFile *>(state);
    std::shared_ptr<SmbMount> mount = MountFrom(file);
    if (!file || !mount || !file->handle)
        return Fail(reent, EBADF);
    if (length < 0)
        return Fail(reent, EINVAL);

    std::lock_guard lock(mount->io_mutex);
    const int synchronized = SynchronizeFilePosition(file);
    if (synchronized < 0)
        return Fail(reent, -synchronized);
    const int result = smb2_ftruncate(
        mount->context, file->handle, static_cast<std::uint64_t>(length));
    if (result < 0)
        return Fail(reent, -result);
    Succeed(reent);
    return 0;
}

int SmbSync(_reent *reent, void *state) {
    auto *file = static_cast<SmbFile *>(state);
    std::shared_ptr<SmbMount> mount = MountFrom(file);
    if (!file || !mount || !file->handle)
        return Fail(reent, EBADF);
    std::lock_guard lock(mount->io_mutex);
    const int result = smb2_fsync(mount->context, file->handle);
    if (result < 0)
        return Fail(reent, -result);
    Succeed(reent);
    return 0;
}

void UsbStatusChanged(const UsbHsFsDevice *devices, u32 count, void *) {
    {
        std::lock_guard lock(s_usb_mutex);
        s_usb_devices.clear();
        if (devices && count)
            s_usb_devices.assign(devices, devices + count);
        s_usb_generation.fetch_add(1, std::memory_order_release);
    }
    std::lock_guard callback_lock(s_usb_callback_mutex);
    if (s_usb_callback)
        s_usb_callback(s_usb_callback_data);
}

void PopulateDevoptab(SmbDevice *device) {
    device->devoptab.name = device->device_name.c_str();
    device->devoptab.structSize = sizeof(SmbFile);
    device->devoptab.open_r = SmbOpen;
    device->devoptab.close_r = SmbClose;
    device->devoptab.write_r = SmbWrite;
    device->devoptab.read_r = SmbRead;
    device->devoptab.seek_r = SmbSeek;
    device->devoptab.fstat_r = SmbFstat;
    device->devoptab.stat_r = SmbStat;
    device->devoptab.unlink_r = SmbUnlink;
    device->devoptab.rename_r = SmbRename;
    device->devoptab.mkdir_r = SmbMkdir;
    device->devoptab.dirStateSize = sizeof(SmbDir);
    device->devoptab.diropen_r = SmbDirOpen;
    device->devoptab.dirreset_r = SmbDirReset;
    device->devoptab.dirnext_r = SmbDirNext;
    device->devoptab.dirclose_r = SmbDirClose;
    device->devoptab.statvfs_r = SmbStatvfs;
    device->devoptab.ftruncate_r = SmbTruncate;
    device->devoptab.fsync_r = SmbSync;
    device->devoptab.deviceData = device;
    device->devoptab.rmdir_r = SmbRmdir;
    device->devoptab.lstat_r = SmbStat;
}

void HashBytes(std::uint64_t *hash, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        *hash ^= bytes[index];
        *hash *= 1099511628211ULL;
    }
}

template <typename Value>
void HashInteger(std::uint64_t *hash, Value value) {
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        const std::uint8_t byte = static_cast<std::uint8_t>(value & 0xff);
        HashBytes(hash, &byte, 1);
        value >>= 8;
    }
}

template <std::size_t Size>
void HashText(std::uint64_t *hash, const char (&text)[Size]) {
    HashBytes(hash, text, strnlen(text, Size));
    const std::uint8_t separator = 0;
    HashBytes(hash, &separator, 1);
}

std::string FormatUsbId(const char *prefix, std::uint64_t hash) {
    char output[32];
    std::snprintf(output, sizeof(output), "%s-%016llx", prefix,
        static_cast<unsigned long long>(hash));
    return output;
}

std::string UsbPhysicalId(const UsbHsFsDevice &device) {
    std::uint64_t hash = 14695981039346656037ULL;
    HashInteger(&hash, device.vid); HashInteger(&hash, device.pid);
    HashText(&hash, device.serial_number);
    if (!device.serial_number[0]) {
        HashText(&hash, device.manufacturer); HashText(&hash, device.product_name);
        HashInteger(&hash, device.capacity);
    }
    return FormatUsbId("usbdev", hash);
}

std::string UsbVolumeId(const UsbHsFsDevice &device) {
    std::uint64_t hash = 14695981039346656037ULL;
    const std::string physical = UsbPhysicalId(device);
    HashBytes(&hash, physical.data(), physical.size());
    HashInteger(&hash, device.lun); HashInteger(&hash, device.fs_idx);
    HashInteger(&hash, device.fs_type); HashInteger(&hash, device.capacity);
    return FormatUsbId("usbvol", hash);
}

Location MakeUsbLocation(const UsbHsFsDevice &device) {
    Location location;
    location.id = UsbVolumeId(device); location.physical_id = UsbPhysicalId(device);
    location.mount_alias = device.name; location.path = device.name;
    if (!location.path.empty() && location.path.back() != '/') location.path += '/';
    location.serial_number.assign(device.serial_number,
        strnlen(device.serial_number, sizeof(device.serial_number)));
    location.vendor_id = device.vid; location.product_id = device.pid;
    location.lun = device.lun; location.partition = device.fs_idx;
    location.filesystem_type = device.fs_type; location.capacity = device.capacity;
    const std::uint64_t gib = device.capacity / (1024ULL * 1024ULL * 1024ULL);
    char label[256];
    std::snprintf(label, sizeof(label), "%s - %s%s%s (%llu GiB)", device.name,
        LIBUSBHSFS_FS_TYPE_STR(device.fs_type), device.product_name[0] ? " - " : "",
        device.product_name, static_cast<unsigned long long>(gib));
    location.label = label;
    return location;
}

struct SmbConnectResult {
    bool complete = false;
    int status = -ECONNABORTED;
};

void SmbConnectCallback(smb2_context *, int status, void *, void *private_data) {
    auto *result = static_cast<SmbConnectResult *>(private_data);
    result->status = status; result->complete = true;
}

} // namespace

Location SdLocation() {
    Location location;
    location.path = "sdmc:/";
    location.label = "SD Card";
    location.id = "sdmc";
    location.mount_alias = "sdmc:";
    return location;
}

std::vector<Location> ListLocalLocations() {
    std::vector<Location> result;
    result.emplace_back(SdLocation());
    std::vector<Location> usb = ListUsbLocations();
    result.insert(result.end(), std::make_move_iterator(usb.begin()),
        std::make_move_iterator(usb.end()));
    return result;
}

bool InitializeUsb(std::string *error) {
    ClearError(error);
    {
        std::lock_guard lock(s_mount_mutex);
        if (s_shutdown_requested) {
            if (error) *error = "Storage backend is shutting down";
            return false;
        }
        if (s_usb_initialized) return true;
        usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_None);
        const Result result = usbHsFsInitialize(0);
        if (R_FAILED(result)) {
            if (error) { char message[96]; std::snprintf(message, sizeof(message),
                "USB initialization failed (0x%08x)", result); *error = message; }
            return false;
        }
        s_usb_initialized = true;
        usbHsFsSetPopulateCallback(UsbStatusChanged, nullptr);
    }
    std::array<UsbHsFsDevice, 32> devices{};
    const u32 count = usbHsFsListMountedDevices(devices.data(), devices.size());
    UsbStatusChanged(devices.data(), count, nullptr);
    return true;
}

std::uint64_t UsbStatusGeneration() {
    return s_usb_generation.load(std::memory_order_acquire);
}

void SetUsbStatusCallback(UsbStatusCallback callback, void *user_data) {
    std::lock_guard lock(s_usb_callback_mutex);
    s_usb_callback = callback; s_usb_callback_data = callback ? user_data : nullptr;
}

UsbSnapshot GetUsbSnapshot() {
    UsbSnapshot snapshot;
    { std::lock_guard lock(s_mount_mutex); if (!s_usb_initialized) return snapshot; }
    std::lock_guard lock(s_usb_mutex);
    snapshot.generation = s_usb_generation.load(std::memory_order_acquire);
    snapshot.locations.reserve(s_usb_devices.size());
    for (const UsbHsFsDevice &device : s_usb_devices)
        if (device.name[0]) snapshot.locations.emplace_back(MakeUsbLocation(device));
    return snapshot;
}

std::vector<Location> ListUsbLocations() { return GetUsbSnapshot().locations; }

std::string ResolveUsbPath(const std::string &id) {
    const auto locations = ListUsbLocations();
    const auto found = std::find_if(locations.begin(), locations.end(),
        [&id](const Location &location) { return location.id == id; });
    return found == locations.end() ? std::string{} : found->path;
}

bool SafelyEjectUsb(const std::string &id, std::string *error) {
    ClearError(error);
    {
        std::lock_guard lock(s_mount_mutex);
        if (!s_usb_initialized) {
            if (error) *error = "USB storage is not initialized";
            return false;
        }
    }
    UsbHsFsDevice target{}; bool found = false;
    { std::lock_guard lock(s_usb_mutex); for (const UsbHsFsDevice &device : s_usb_devices)
        if (UsbVolumeId(device) == id || UsbPhysicalId(device) == id) {
            target = device; found = true; break;
        } }
    if (!found) { if (error) *error = "The USB drive is no longer connected"; return false; }
    if (!usbHsFsUnmountDevice(&target, true)) {
        if (error) *error = "Could not safely eject the USB drive; close files using it and try again";
        return false;
    }
    return true;
}

std::string SmbRootPath(const std::string &id) {
    const std::string device_name = DeviceNameForId(id);
    return device_name.empty() ? std::string{} : device_name + ":/";
}

std::string SmbBrowsePath(const SmbShare &share) {
    std::string result = SmbRootPath(share.id);
    if (result.empty() || share.path.empty())
        return result;

    std::string path = share.path;
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path.empty() ? result : result + path;
}

bool MountSmb(const SmbShare &share, std::string *error, const std::atomic_bool *cancel) {
    ClearError(error);
    if (!ValidId(share.id) || share.server.empty() || share.share.empty()) {
        if (error)
            *error = "SMB share settings are incomplete";
        return false;
    }

    {
        std::lock_guard lock(s_mount_mutex);
        if (s_shutdown_requested) {
            if (error)
                *error = "Storage backend is shutting down";
            s_smb_status[share.id] = {SmbConnectionState::Failed,
                "Storage backend is shutting down"};
            return false;
        }
        const auto exists = std::any_of(s_smb_mounts.begin(), s_smb_mounts.end(),
            [&share](const std::shared_ptr<SmbMount> &mount) {
                return mount->config.id == share.id;
            });
        if (exists) {
            s_smb_status[share.id]={SmbConnectionState::Connected,{}};
            return true;
        }
        s_smb_status[share.id] = {SmbConnectionState::Connecting, {}};
    }

    auto mount = std::make_shared<SmbMount>();
    mount->config = share;
    mount->context = smb2_init_context();
    if (!mount->context) {
        if (error)
            *error = "Could not create the SMB client";
        mount->state.store(SmbConnectionState::Failed, std::memory_order_release);
        SetSmbStatus(share.id,SmbConnectionState::Failed,"Could not create the SMB client");
        return false;
    }

    smb2_set_security_mode(mount->context, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(mount->context, 6);
    if (!share.user.empty())
        smb2_set_user(mount->context, share.user.c_str());
    if (!share.password.empty())
        smb2_set_password(mount->context, share.password.c_str());
    if (!share.domain.empty())
        smb2_set_domain(mount->context, share.domain.c_str());
    mount->state.store(SmbConnectionState::Connecting, std::memory_order_release);
    SmbConnectResult connection;
    int connected = smb2_connect_share_async(mount->context, share.server.c_str(),
        share.share.c_str(), share.user.empty() ? nullptr : share.user.c_str(),
        SmbConnectCallback, &connection);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while(connected>=0&&!connection.complete){
        if(cancel&&cancel->load(std::memory_order_acquire)){
            mount->last_error="SMB connection cancelled";
            mount->state.store(SmbConnectionState::Disconnected,std::memory_order_release);
            if(error)*error=mount->last_error;
            SetSmbStatus(share.id,SmbConnectionState::Disconnected,mount->last_error);
            return false;
        }
        if(std::chrono::steady_clock::now()>=deadline){
            mount->last_error="SMB connection timed out";
            mount->state.store(SmbConnectionState::Failed,std::memory_order_release);
            if(error)*error=mount->last_error;
            SetSmbStatus(share.id,SmbConnectionState::Failed,mount->last_error);
            return false;
        }
        pollfd descriptor{};descriptor.fd=smb2_get_fd(mount->context);
        descriptor.events=static_cast<short>(smb2_which_events(mount->context));
        const int polled=::poll(&descriptor,1,100);
        if(polled<0){if(errno==EINTR)continue;connected=-errno;break;}
        if(polled>0&&smb2_service(mount->context,descriptor.revents)<0){connected=-EIO;break;}
    }
    if(connected>=0)connected=connection.status;
    if (connected < 0) {
        const char *detail = smb2_get_error(mount->context);
        mount->last_error = detail && *detail ? detail : "Could not connect to the SMB share";
        mount->state.store(SmbConnectionState::Failed, std::memory_order_release);
        if (error) {
            *error = mount->last_error;
        }
        SetSmbStatus(share.id,SmbConnectionState::Failed,mount->last_error);
        return false;
    }
    mount->connected = true;
    mount->last_error.clear();
    mount->state.store(SmbConnectionState::Connected, std::memory_order_release);
    std::lock_guard lock(s_mount_mutex);
    if (s_shutdown_requested) {
        if (error)
            *error = "Storage backend is shutting down";
        s_smb_status[share.id]={SmbConnectionState::Failed,"Storage backend is shutting down"};
        return false;
    }
    const auto duplicate = std::any_of(s_smb_mounts.begin(), s_smb_mounts.end(),
        [&share](const std::shared_ptr<SmbMount> &existing) {
            return existing->config.id == share.id;
        });
    if (duplicate) {
        s_smb_status[share.id]={SmbConnectionState::Connected,{}};
        return true;
    }
    auto device=std::make_unique<SmbDevice>();
    device->device_name=DeviceNameForId(share.id);
    device->root_path=device->device_name+":/";
    device->mount=mount;
    PopulateDevoptab(device.get());
    mount->device=device.get();
    if (AddDevice(&device->devoptab) < 0) {
        mount->device=nullptr;
        if (error)
            *error = "No free filesystem slot is available for the SMB share";
        s_smb_status[share.id]={SmbConnectionState::Failed,"No free filesystem slot is available for the SMB share"};
        return false;
    }
    s_smb_devices.emplace_back(std::move(device));
    s_smb_mounts.emplace_back(std::move(mount));
    s_smb_status[share.id]={SmbConnectionState::Connected,{}};
    return true;
}

bool UnmountSmb(const std::string &id, std::string *error) {
    ClearError(error);
    std::shared_ptr<SmbMount> released;
    {
        std::lock_guard lock(s_mount_mutex);
        const auto iterator = std::find_if(s_smb_mounts.begin(), s_smb_mounts.end(),
            [&id](const std::shared_ptr<SmbMount> &mount) {
                return mount->config.id == id;
            });
        if (iterator == s_smb_mounts.end())
            return true;

        released = *iterator;
        released->retired.store(true,std::memory_order_release);
        SmbDevice *device=released->device;
        if (!device) {
            released->retired.store(false,std::memory_order_release);
            if (error)
                *error = "Could not unregister the SMB filesystem";
            return false;
        }
        // Tombstone the registration before RemoveDevice.  A callback already
        // dispatched by newlib can still hold deviceData, but once it reaches
        // MountFrom it can no longer acquire this (or a future) connection.
        std::shared_ptr<SmbMount> registered = std::move(device->mount);
        if (RemoveDevice(device->root_path.c_str()) < 0) {
            device->mount = std::move(registered);
            released->retired.store(false,std::memory_order_release);
            if (error)
                *error = "Could not unregister the SMB filesystem";
            return false;
        }
        released->state.store(SmbConnectionState::Disconnected, std::memory_order_release);
        s_smb_status[id]={SmbConnectionState::Disconnected,{}};
        s_smb_mounts.erase(iterator);
    }
    // A callback that reached MountFrom before RemoveDevice pinned this mount.
    // Wait for it outside the registry lock before the SMB context is torn down.
    std::lock_guard io_lock(released->io_mutex);
    return true;
}

bool IsSmbMounted(const std::string &id) {
    std::lock_guard lock(s_mount_mutex);
    return std::any_of(s_smb_mounts.begin(), s_smb_mounts.end(),
        [&id](const std::shared_ptr<SmbMount> &mount) {
            return mount->config.id == id;
        });
}

SmbConnectionState GetSmbConnectionState(const std::string &id) {
    std::lock_guard lock(s_mount_mutex);
    const auto found = std::find_if(s_smb_mounts.begin(), s_smb_mounts.end(),
        [&id](const std::shared_ptr<SmbMount> &mount) { return mount->config.id == id; });
    if(found!=s_smb_mounts.end())return (*found)->state.load(std::memory_order_acquire);
    const auto status=s_smb_status.find(id);
    return status==s_smb_status.end()?SmbConnectionState::Disconnected:status->second.state;
}

bool ReconnectSmb(const std::string &id, std::string *error, const std::atomic_bool *cancel) {
    ClearError(error);
    SmbShare config;
    {
        std::lock_guard lock(s_mount_mutex);
        const auto found = std::find_if(s_smb_mounts.begin(), s_smb_mounts.end(),
            [&id](const std::shared_ptr<SmbMount> &mount) { return mount->config.id == id; });
        if (found == s_smb_mounts.end()) {
            if (error) *error = "The SMB share is not mounted";
            return false;
        }
        config = (*found)->config;
        (*found)->state.store(SmbConnectionState::Reconnecting, std::memory_order_release);
        s_smb_status[id]={SmbConnectionState::Reconnecting,{}};
    }
    if (cancel && cancel->load(std::memory_order_acquire)) {
        if (error) *error = "SMB reconnect cancelled";
        std::lock_guard lock(s_mount_mutex);
        const auto active=std::find_if(s_smb_mounts.begin(),s_smb_mounts.end(),
          [&id](const std::shared_ptr<SmbMount>&mount){return mount->config.id==id;});
        if(active!=s_smb_mounts.end())(*active)->state.store(SmbConnectionState::Connected,std::memory_order_release);
        s_smb_status[id]={active!=s_smb_mounts.end()?SmbConnectionState::Connected:SmbConnectionState::Disconnected,
                          "SMB reconnect cancelled"};
        return false;
    }
    std::string unmountError;
    if (!UnmountSmb(id, &unmountError)) {
        if (error) *error = unmountError;
        std::lock_guard lock(s_mount_mutex);
        const auto active=std::find_if(s_smb_mounts.begin(),s_smb_mounts.end(),
          [&id](const std::shared_ptr<SmbMount>&mount){return mount->config.id==id;});
        if(active!=s_smb_mounts.end())(*active)->state.store(SmbConnectionState::Connected,std::memory_order_release);
        s_smb_status[id]={active!=s_smb_mounts.end()?SmbConnectionState::Connected:SmbConnectionState::Failed,
                          unmountError};
        return false;
    }
    if (cancel && cancel->load(std::memory_order_acquire)) {
        if (error) *error = "SMB reconnect cancelled";
        SetSmbStatus(id,SmbConnectionState::Disconnected,"SMB reconnect cancelled");
        return false;
    }
    return MountSmb(config, error, cancel);
}

void Shutdown() {
    SetUsbStatusCallback(nullptr);
    std::vector<std::shared_ptr<SmbMount>> removed;
    {
        std::lock_guard lock(s_mount_mutex);
        s_shutdown_requested = true;
        auto iterator = s_smb_mounts.begin();
        while (iterator != s_smb_mounts.end()) {
            const std::shared_ptr<SmbMount> mount=*iterator;
            mount->retired.store(true,std::memory_order_release);
            SmbDevice *device=mount->device;
            if(!device){
                mount->retired.store(false,std::memory_order_release);
                std::fprintf(stderr,"Vita3K launcher: could not unregister SMB device %s\n",
                    "<missing>");
                ++iterator;
                continue;
            }
            std::shared_ptr<SmbMount> registered=std::move(device->mount);
            if(RemoveDevice(device->root_path.c_str())<0){
                device->mount=std::move(registered);
                mount->retired.store(false,std::memory_order_release);
                std::fprintf(stderr,"Vita3K launcher: could not unregister SMB device %s\n",
                    device->root_path.c_str());
                ++iterator;
                continue;
            }
            mount->state.store(SmbConnectionState::Disconnected,std::memory_order_release);
            removed.emplace_back(mount);
            iterator = s_smb_mounts.erase(iterator);
        }

        if (s_usb_initialized) {
            usbHsFsSetPopulateCallback(nullptr, nullptr);
            usbHsFsExit();
            s_usb_initialized = false;
            s_usb_generation.fetch_add(1, std::memory_order_release);
        }
    }
    for(const std::shared_ptr<SmbMount> &mount:removed){
        std::lock_guard io_lock(mount->io_mutex);
    }
}

} // namespace Vita3KLauncher::Storage
