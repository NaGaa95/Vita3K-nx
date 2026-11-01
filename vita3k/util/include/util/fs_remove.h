// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <system_error>

namespace fs_utils {
bool remove_path(const std::filesystem::path &path, std::error_code &error);
void remove_path(const std::filesystem::path &path);
}
