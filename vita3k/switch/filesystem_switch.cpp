// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

// Native Horizon SD-card picker used by Vita3K's shared host-dialog API. This
// intentionally uses the libnx framebuffer console: shared file operations are
// entered before a game owns the Vulkan surface (the SDL launcher has its own
// graphical browser). The picker never mutates files and remains usable without
// Qt or SDL video support.

#include <util/fs.h>

#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace host::dialog::filesystem {

enum Result {
    ERROR,
    SUCCESS,
    CANCEL,
};

struct FileFilter {
    std::string display_name = "";
    std::vector<std::string> file_extensions = {};
};

Result open_file(fs::path &resulting_path, const std::vector<FileFilter> &file_filters = {}, const fs::path &default_path = "");
Result pick_folder(fs::path &resulting_path, const fs::path &default_path = "");
std::string get_error();

namespace {

struct Entry {
    fs::path path;
    std::string name;
    bool directory = false;
};

std::mutex s_dialog_mutex;
std::string s_error;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool at_sd_root(const fs::path &path) {
    std::string value = path.generic_string();
    while (value.size() > 6 && value.back() == '/')
        value.pop_back();
    return value == "sdmc:" || value == "sdmc:/";
}

fs::path parent_path(const fs::path &path) {
    if (at_sd_root(path))
        return fs::path("sdmc:/");
    fs::path parent = path.parent_path();
    if (parent.empty() || parent.generic_string() == "sdmc:")
        return fs::path("sdmc:/");
    return parent;
}

bool matches_filters(const fs::path &path, const std::vector<FileFilter> &filters) {
    bool has_extension_filter = false;
    const std::string actual = lower_ascii(path.extension().string());
    for (const auto &filter : filters) {
        for (std::string wanted : filter.file_extensions) {
            has_extension_filter = true;
            wanted = lower_ascii(wanted);
            if (wanted.starts_with("*."))
                wanted.erase(0, 1);
            else if (!wanted.empty() && wanted.front() != '.')
                wanted.insert(wanted.begin(), '.');
            if (wanted == actual)
                return true;
        }
    }
    return !has_extension_filter;
}

bool read_directory(const fs::path &path, const std::vector<FileFilter> &filters,
    bool folder_mode, std::vector<Entry> &entries) {
    entries.clear();
    try {
        for (fs::directory_iterator it(path), end; it != end; ++it) {
            const bool directory = fs::is_directory(it->status());
            if (!directory && (folder_mode || !matches_filters(it->path(), filters)))
                continue;
            entries.push_back({ it->path(), it->path().filename().string(), directory });
        }
    } catch (const std::exception &e) {
        s_error = e.what();
        return false;
    }

    std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        if (left.directory != right.directory)
            return left.directory > right.directory;
        return lower_ascii(left.name) < lower_ascii(right.name);
    });
    return true;
}

Result show_picker(fs::path &resulting_path, const std::vector<FileFilter> &filters,
    fs::path default_path, bool folder_mode) {
    std::lock_guard dialog_lock(s_dialog_mutex);
    s_error.clear();
    resulting_path.clear();

    if (default_path.empty() || !fs::exists(default_path))
        default_path = fs::path("sdmc:/switch/vita3k/");
    if (fs::exists(default_path) && !fs::is_directory(default_path))
        default_path = parent_path(default_path);
    if (!fs::exists(default_path))
        default_path = fs::path("sdmc:/");

    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad{};
    padInitializeDefault(&pad);

    fs::path current = default_path;
    std::vector<Entry> entries;
    int selected = 0;
    bool redraw = true;
    constexpr int visible_rows = 18;
    Result result = CANCEL;

    while (appletMainLoop()) {
        if (redraw) {
            if (!read_directory(current, filters, folder_mode, entries)) {
                result = ERROR;
                break;
            }
            selected = entries.empty() ? 0 : std::clamp(selected, 0, static_cast<int>(entries.size()) - 1);
            const int first = entries.empty() ? 0 : (selected / visible_rows) * visible_rows;

            std::printf("\x1b[2J\x1b[H");
            std::printf("Vita3K - %s\n\n", folder_mode ? "Select folder" : "Open file");
            std::printf("%.74s\n\n", current.generic_string().c_str());
            if (entries.empty())
                std::printf("  (empty folder)\n");
            for (int row = 0; row < visible_rows && first + row < static_cast<int>(entries.size()); ++row) {
                const int index = first + row;
                const Entry &entry = entries[index];
                std::printf("%c %s %-65.65s\n", index == selected ? '>' : ' ',
                    entry.directory ? "[D]" : "   ", entry.name.c_str());
            }
            std::printf("\nD-pad: move  A: open  B: parent/back  +: cancel\n");
            if (folder_mode)
                std::printf("X: select this folder\n");
            redraw = false;
        }

        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus)
            break;
        if (down & HidNpadButton_Up) {
            if (!entries.empty())
                selected = (selected + static_cast<int>(entries.size()) - 1) % static_cast<int>(entries.size());
            redraw = true;
        }
        if (down & HidNpadButton_Down) {
            if (!entries.empty())
                selected = (selected + 1) % static_cast<int>(entries.size());
            redraw = true;
        }
        if (down & HidNpadButton_L) {
            selected = std::max(0, selected - visible_rows);
            redraw = true;
        }
        if (down & HidNpadButton_R) {
            selected = std::min(std::max(0, static_cast<int>(entries.size()) - 1), selected + visible_rows);
            redraw = true;
        }
        if (down & HidNpadButton_B) {
            if (at_sd_root(current))
                break;
            current = parent_path(current);
            selected = 0;
            redraw = true;
        }
        if (folder_mode && (down & HidNpadButton_X)) {
            resulting_path = current;
            result = SUCCESS;
            break;
        }
        if ((down & HidNpadButton_A) && !entries.empty()) {
            const Entry chosen = entries[selected];
            if (chosen.directory) {
                current = chosen.path;
                selected = 0;
                redraw = true;
            } else if (!folder_mode) {
                resulting_path = chosen.path;
                result = SUCCESS;
                break;
            }
        }

        consoleUpdate(nullptr);
    }

    consoleExit(nullptr);
    return result;
}

} // namespace

Result open_file(fs::path &resulting_path, const std::vector<FileFilter> &file_filters, const fs::path &default_path) {
    return show_picker(resulting_path, file_filters, default_path, false);
}

Result pick_folder(fs::path &resulting_path, const fs::path &default_path) {
    return show_picker(resulting_path, {}, default_path, true);
}

std::string get_error() {
    std::lock_guard lock(s_dialog_mutex);
    return s_error;
}

} // namespace host::dialog::filesystem
