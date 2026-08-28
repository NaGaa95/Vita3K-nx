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
 * @file license.cpp
 * @brief Sony `NpDrm` license and `work.bin` handling
 */

#include <emuenv/state.h>

#include <packages/license.h>

#include <util/bytes.h>
#include <util/log.h>

#include <zRIF/keyflate.h>
#include <zrif2rif.h>

#include <b64/cdecode.h>

#include <algorithm>
#include <cctype>
#include <cstring>

bool validate_zrif(const std::string &zRIF) {
    if (zRIF.empty())
        return false;

    char buf[2048];
    base64_decodestate state;
    base64_init_decodestate(&state);
    const size_t decoded_len = base64_decode_block(zRIF.c_str(), static_cast<int>(zRIF.size()), buf, &state);
    if (decoded_len == 0)
        return false;

    uint8_t license[512] = {};
    const int inflated_len = inflateKey(reinterpret_cast<const uint8_t *>(buf), decoded_len, license, sizeof(license));
    return inflated_len == 512;
}

static bool open_license(const fs::path &license_path, SceNpDrmLicense &license_buf) {
    memset(&license_buf, 0, sizeof(SceNpDrmLicense));
    fs::ifstream license(license_path, std::ios::in | std::ios::binary);
    if (license.is_open()) {
        license.read((char *)&license_buf, sizeof(SceNpDrmLicense));
        const bool complete = license.gcount() == static_cast<std::streamsize>(sizeof(SceNpDrmLicense));
        license.close();
        return complete;
    }

    return false;
}

bool copy_license(EmuEnvState &emuenv, const fs::path &license_path) {
    SceNpDrmLicense license_buf;
    if (open_license(license_path, license_buf)) {
        const auto *const content_id_end = static_cast<const char *>(
            std::memchr(license_buf.content_id, '\0', sizeof(license_buf.content_id)));
        if (!content_id_end) {
            LOG_ERROR("License at {} has an unterminated content id", license_path);
            return false;
        }

        const size_t content_id_size = static_cast<size_t>(content_id_end - license_buf.content_id);
        emuenv.license_content_id.assign(license_buf.content_id, content_id_size);
        const bool valid_content_id = content_id_size >= 16
            && std::all_of(emuenv.license_content_id.begin(), emuenv.license_content_id.end(), [](const unsigned char c) {
                   return std::isalnum(c) || c == '-' || c == '_';
               });
        if (!valid_content_id) {
            LOG_ERROR("License at {} has an invalid content id", license_path);
            return false;
        }

        emuenv.license_title_id = emuenv.license_content_id.substr(7, 9);
        const auto dst_path{ emuenv.vita_fs_path / "ux0/license" / emuenv.license_title_id };
        boost::system::error_code fs_error;
        fs::create_directories(dst_path, fs_error);
        if (fs_error) {
            LOG_ERROR("Failed to create license directory {}: {}", dst_path, fs_error.message());
            return false;
        }

        const auto license_dst_path{ dst_path / fmt::format("{}.rif", emuenv.license_content_id) };
        if (license_path != license_dst_path) {
#ifdef __SWITCH__
            // boost::filesystem::copy_file has been observed to write a zeroed file
            // on the Horizon sdmc: devoptab (the derived .rif deflated to 12 bytes
            // and decoded as an invalid PsmDrm license). We already hold the full
            // 512-byte license in license_buf, so write it out byte-exact instead.
            bool write_succeeded = false;
            {
                fs::ofstream out(license_dst_path, std::ios::out | std::ios::binary | std::ios::trunc);
                if (out.is_open()) {
                    out.write(reinterpret_cast<const char *>(&license_buf), sizeof(SceNpDrmLicense));
                    out.flush();
                    write_succeeded = out.good();
                    out.close();
                    write_succeeded = write_succeeded && !out.fail();
                }
            }
            const uintmax_t written_size = fs::file_size(license_dst_path, fs_error);
            write_succeeded = write_succeeded && !fs_error && written_size == sizeof(SceNpDrmLicense);
            if (!write_succeeded) {
                boost::system::error_code remove_error;
                fs::remove(license_dst_path, remove_error);
                LOG_ERROR("Fail copy license file to: {}", license_dst_path);
                return false;
            }
#else
            fs::copy_file(license_path, license_dst_path, fs::copy_options::overwrite_existing);
#endif
            if (fs::exists(license_dst_path)) {
                LOG_INFO("Success copy license file to: {}", license_dst_path);
                return true;
            } else
                LOG_ERROR("Fail copy license file to: {}", license_dst_path);
        } else
            LOG_ERROR("Source and destination license is same at: {}", license_path);
    } else
        LOG_ERROR("License file is corrupted at: {}", license_path);

    return false;
}

void get_license(EmuEnvState &emuenv, const std::string &title_id, const std::string &content_id) {
    // Skip if it's not a retail game or already have a license
    if (!title_id.starts_with("PCS") || emuenv.license.rif.contains(title_id))
        return;

    // Get license buffer corresponding to the title id
    auto &license_buf = emuenv.license.rif[title_id];
    license_buf = {};

    // Open license file
    const auto license_path{ emuenv.vita_fs_path / "ux0/license" / title_id / fmt::format("{}.rif", content_id) };
    if (!open_license(license_path, license_buf)) {
        if (fs::exists(license_path))
            fs::remove(license_path);

        LOG_WARN("License file is corrupted or missing at: {}, using default value.", license_path);
        const auto RETAIL_APP_PATH{ emuenv.vita_fs_path / "ux0/app" / title_id / "sce_sys/retail/livearea" };
        if (fs::exists(RETAIL_APP_PATH))
            license_buf.sku_flag = 1;
        else
            license_buf.sku_flag = 0;
    } else
        license_buf.sku_flag = byte_swap(license_buf.sku_flag); // Convert to little endian
}

bool create_license(EmuEnvState &emuenv, const std::string &zRIF) {
    if (!validate_zrif(zRIF)) {
        LOG_ERROR("Invalid zRIF key provided");
        return false;
    }

    fs::create_directories(emuenv.cache_path);

    // Create a temp license file
    const auto temp_license_path = emuenv.cache_path / "temp_licence.rif";
    fs::ofstream temp_file(temp_license_path, std::ios::out | std::ios::binary);
    if (!temp_file.is_open()) {
        LOG_ERROR("Failed to create temp license file at: {}", temp_license_path);
        return false;
    }

    // Convert zRIF to RIF. zrif2rif writes the whole 512-byte record and closes
    // the stream itself, so the file is already complete and flushed here. Do not
    // close it again: close() on a closed ofstream sets failbit, and treating that
    // as a write failure is what made every packaged install fail at the last step
    // after minutes of successful decryption.
    zrif2rif(zRIF, temp_file);

    boost::system::error_code fs_error;
    const uintmax_t temp_size = fs::file_size(temp_license_path, fs_error);
    if (fs_error || temp_size != sizeof(SceNpDrmLicense)) {
        boost::system::error_code remove_error;
        fs::remove(temp_license_path, remove_error);
        LOG_ERROR("Failed to finish temp license file at: {} ({} bytes on disk, expected {})",
            temp_license_path, fs_error ? 0u : temp_size, sizeof(SceNpDrmLicense));
        return false;
    }

    const bool res = copy_license(emuenv, temp_license_path);
    fs::remove(temp_license_path, fs_error);
    return res;
}
