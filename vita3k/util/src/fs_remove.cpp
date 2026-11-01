// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <util/fs_remove.h>

#ifdef __SWITCH__
#include <switch.h>
#include <util/log.h>
#endif

namespace fs_utils {
bool remove_path(const std::filesystem::path &path, std::error_code &error) {
#ifdef __SWITCH__
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        return true;
    }
    if (error)
        return false;
    if (std::filesystem::is_directory(status)) {
        // libstdc++ remove_all can delete an empty directory before closing it.
        const Result result = fsdevDeleteDirectoryRecursively(path.c_str());
        if (R_FAILED(result)) {
            LOG_ERROR("Failed to remove directory '{}': Horizon result {:#x}", path.string(), result);
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
    } else {
        std::filesystem::remove(path, error);
    }
#else
    std::filesystem::remove_all(path, error);
#endif
    return !error;
}

void remove_path(const std::filesystem::path &path) {
    std::error_code error;
    if (!remove_path(path, error))
        throw std::filesystem::filesystem_error("Cannot remove path", path, error);
}
}
