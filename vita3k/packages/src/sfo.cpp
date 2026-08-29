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

/**
 * @file sfo.cpp
 * @brief PlayStation setting file (`.sfo`) handling
 *
 * PlayStation setting files (`.sfo`) contain metadata information usually describing
 * the content they are accompanying.
 */

#include <packages/sfo.h>

#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>

namespace sfo {

bool get_data_by_id(std::string &out_data, SfoFile &file, int id) {
    std::string key;
    switch (id) {
    case 6:
        key = "CONTENT_ID";
        break;
    case 7:
        key = "NP_COMMUNICATION_ID";
        break;
    case 8:
        key = "CATEGORY";
        break;
    case 9:
        key = "TITLE";
        break;
    case 10:
        key = "STITLE";
        break;
    case 0xc:
        key = "TITLE_ID";
        break;
    case 0xe: // Todo
    default:
        return false;
    }

    return get_data_by_key(out_data, file, key);
}

bool get_data_by_key(std::string &out_data, SfoFile &file, const std::string &key) {
    auto res = std::find_if(file.entries.begin(), file.entries.end(),
        [key](const auto &et) { return et.data.first == key; });

    if (res == file.entries.end()) {
        return false;
    }
    out_data = res->data.second;

    return true;
}

bool get_param_info(sfo::SfoAppInfo &app_info, const vfs::FileBuffer &param, int sys_lang) {
    SfoFile sfo_handle;
    app_info = {};
    if (!sfo::load(sfo_handle, param))
        return false;
    sfo::get_data_by_key(app_info.app_version, sfo_handle, "APP_VER");
    if (!app_info.app_version.empty() && app_info.app_version[0] == '0')
        app_info.app_version.erase(app_info.app_version.begin());
    sfo::get_data_by_key(app_info.app_category, sfo_handle, "CATEGORY");
    sfo::get_data_by_key(app_info.app_content_id, sfo_handle, "CONTENT_ID");
    if (!sfo::get_data_by_key(app_info.app_addcont, sfo_handle, "INSTALL_DIR_ADDCONT"))
        sfo::get_data_by_key(app_info.app_addcont, sfo_handle, "TITLE_ID");
    if (!sfo::get_data_by_key(app_info.app_savedata, sfo_handle, "INSTALL_DIR_SAVEDATA"))
        sfo::get_data_by_key(app_info.app_savedata, sfo_handle, "TITLE_ID");
    sfo::get_data_by_key(app_info.app_parental_level, sfo_handle, "PARENTAL_LEVEL");
    if (!sfo::get_data_by_key(app_info.app_short_title, sfo_handle, fmt::format("STITLE_{:0>2d}", sys_lang)))
        sfo::get_data_by_key(app_info.app_short_title, sfo_handle, "STITLE");
    if (!sfo::get_data_by_key(app_info.app_title, sfo_handle, fmt::format("TITLE_{:0>2d}", sys_lang)))
        sfo::get_data_by_key(app_info.app_title, sfo_handle, "TITLE");
    std::replace(app_info.app_title.begin(), app_info.app_title.end(), '\n', ' ');
    boost::trim(app_info.app_title);
    sfo::get_data_by_key(app_info.app_title_id, sfo_handle, "TITLE_ID");

    return true;
}

bool load(SfoFile &sfile, const std::vector<uint8_t> &content) {
    sfile = {};
    if (content.size() < sizeof(SfoHeader)) {
        return false;
    }

    memcpy(&sfile.header, content.data(), sizeof(SfoHeader));
    constexpr uint32_t SFO_MAGIC = 0x46535000; // "\0PSF", little-endian
    if (sfile.header.magic != SFO_MAGIC)
        return false;

    const size_t entry_count = sfile.header.tables_entries;
    const size_t max_entries = (content.size() - sizeof(SfoHeader)) / sizeof(SfoIndexTableEntry);
    if (entry_count > max_entries)
        return false;

    const size_t index_end = sizeof(SfoHeader) + entry_count * sizeof(SfoIndexTableEntry);
    const size_t key_table_start = sfile.header.key_table_start;
    const size_t data_table_start = sfile.header.data_table_start;
    if (key_table_start < index_end || data_table_start < key_table_start
        || data_table_start > content.size())
        return false;

    sfile.entries.resize(entry_count);

    for (size_t i = 0; i < entry_count; i++) {
        memcpy(&sfile.entries[i].entry, content.data() + sizeof(SfoHeader) + i * sizeof(SfoIndexTableEntry), sizeof(SfoIndexTableEntry));
        const auto &entry = sfile.entries[i].entry;

        const size_t key_offset = entry.key_offset;
        const size_t key_table_size = data_table_start - key_table_start;
        if (key_offset >= key_table_size)
            return false;
        const uint8_t *const key_begin = content.data() + key_table_start + key_offset;
        const size_t key_bytes_available = key_table_size - key_offset;
        const auto *const key_end = static_cast<const uint8_t *>(
            std::memchr(key_begin, '\0', key_bytes_available));
        if (!key_end || key_end == key_begin)
            return false;
        sfile.entries[i].data.first.assign(
            reinterpret_cast<const char *>(key_begin), static_cast<size_t>(key_end - key_begin));

        const size_t data_offset = entry.data_offset;
        const size_t data_size = entry.data_len;
        const size_t data_max_size = entry.data_max_len;
        if (data_size > data_max_size || data_offset > content.size() - data_table_start
            || data_max_size > content.size() - data_table_start - data_offset)
            return false;
        const uint8_t *const data = content.data() + data_table_start + data_offset;

        // Interpret and convert the raw data based on its format
        switch (entry.data_fmt) {
        case SfoDataFormat::UINT32_T: {
            if (data_size != sizeof(uint32_t))
                return false;
            uint32_t value;
            memcpy(&value, data, sizeof(value));
            sfile.entries[i].data.second = std::to_string(value);
            break;
        }
        case SfoDataFormat::ASCII:
        case SfoDataFormat::UTF8:
            // Interpret the data as a raw string (may not be null-terminated)
            sfile.entries[i].data.second.assign(reinterpret_cast<const char *>(data), data_size);
            break;
        case SfoDataFormat::UTF8_NULL: {
            const auto *const terminator = static_cast<const uint8_t *>(std::memchr(data, '\0', data_size));
            if (!terminator)
                return false;
            sfile.entries[i].data.second.assign(
                reinterpret_cast<const char *>(data), static_cast<size_t>(terminator - data));
            break;
        }
        default:
            // Unknown or unsupported data format
            return false;
        }
    }

    return true;
}

} // namespace sfo
