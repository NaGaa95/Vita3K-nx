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
 * @file pkg.cpp
 * @brief PlayStation Vita software package (`.pkg`) handling
 */

#include <F00DKeyEncryptorFactory.h>
#include <PsvPfsParserConfig.h>
#include <Utils.h>
#include <openssl/evp.h>
#include <rif2zrif.h>

#include <io/functions.h>

#include <config/state.h>
#include <emuenv/state.h>

#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <packages/sce_types.h>
#include <packages/sfo.h>

#include <util/bytes.h>
#include <util/log.h>

#include <climits>
#include <memory>

// Credits to mmozeiko https://github.com/mmozeiko/pkg2zip

static void ctr_init(uint8_t *counter, uint8_t *iv, uint64_t n) {
    for (int i = 15; i >= 0; i--) {
        n = n + iv[i];
        counter[i] = (uint8_t)n;
        n >>= 8;
    }
}

static bool is_safe_path_component(const std::string &component) {
    if (component.empty() || component == "." || component == "..")
        return false;

    // A PKG path component must never be able to select another devoptab or
    // directory. Vita content IDs/title IDs use only simple ASCII characters.
    return component.find_first_of("/\\:") == std::string::npos
        && component.find('\0') == std::string::npos;
}

static bool get_safe_pkg_relative_path(const std::string &name, fs::path &relative_path) {
    if (name.empty() || name.find('\0') != std::string::npos || name.find(':') != std::string::npos)
        return false;

    try {
        relative_path = fs_utils::utf8_to_path(name);
    } catch (...) {
        return false;
    }

    if (relative_path.empty() || relative_path.is_absolute()
        || relative_path.has_root_name() || relative_path.has_root_directory())
        return false;

    for (const auto &part : relative_path) {
        if (part == "..")
            return false;
    }

    relative_path = relative_path.lexically_normal();
    return !relative_path.empty() && relative_path != ".";
}

static int execute(std::string &zrif, fs::path &title_src, fs::path &title_dst, F00DEncryptorTypes type, std::string &f00d_arg, PfsProgressCallback progress = nullptr) {
    std::string title_src_str = title_src.string();
    std::string title_dst_str = title_dst.string();
    return execute(zrif, title_src_str, title_dst_str, type, f00d_arg, progress);
}

static fs::path transaction_sibling(const fs::path &final_path, const char *suffix) {
    return final_path.parent_path() / ("." + final_path.filename().string() + suffix);
}

// boost::filesystem::exists(p, ec) reports ENOENT through the error code as well
// as through its return value ("always report errno, even though some errno
// values are not status_errors" - operations.cpp). A destination that does not
// exist yet is the normal case for a first install, so the file_status has to be
// the discriminator: only status_error is a genuine failure to inspect the path.
static bool path_exists_checked(const fs::path &path, const char *description, bool &exists) {
    boost::system::error_code fs_error;
    const fs::file_status status = fs::status(path, fs_error);
    if (status.type() == fs::status_error) {
        LOG_ERROR("Failed to inspect {} '{}': {}", description, path, fs_error.message());
        return false;
    }
    exists = fs::exists(status);
    return true;
}

static bool remove_path_checked(const fs::path &path, const char *description) {
    boost::system::error_code fs_error;
    fs::remove_all(path, fs_error);
    if (fs_error) {
        LOG_ERROR("Failed to remove {} '{}': {}", description, path, fs_error.message());
        return false;
    }
    return true;
}

// Recover either side of an interrupted same-directory swap and clear only the
// transaction-owned staging directory. The live path is never removed here.
static bool prepare_staged_replace(const fs::path &final_path, const fs::path &staged_path, const fs::path &backup_path) {
    boost::system::error_code fs_error;
    fs::create_directories(final_path.parent_path(), fs_error);
    if (fs_error) {
        LOG_ERROR("Failed to create install parent '{}': {}", final_path.parent_path(), fs_error.message());
        return false;
    }

    bool final_exists = false;
    if (!path_exists_checked(final_path, "install destination", final_exists))
        return false;
    bool backup_exists = false;
    if (!path_exists_checked(backup_path, "install backup", backup_exists))
        return false;

    if (backup_exists && !final_exists) {
        fs::rename(backup_path, final_path, fs_error);
        if (fs_error) {
            LOG_ERROR("Failed to restore interrupted install '{}': {}", final_path, fs_error.message());
            return false;
        }
    } else if (backup_exists && !remove_path_checked(backup_path, "stale install backup")) {
        return false;
    }

    return remove_path_checked(staged_path, "stale install staging directory");
}

static bool commit_staged_replace(const fs::path &final_path, const fs::path &staged_path, const fs::path &backup_path) {
    boost::system::error_code fs_error;
    if (!fs::is_directory(staged_path, fs_error) || fs_error) {
        LOG_ERROR("Install staging directory '{}' is missing or unreadable{}{}", staged_path,
            fs_error ? ": " : "", fs_error ? fs_error.message() : "");
        return false;
    }

    bool moved_existing = false;
    bool final_exists = false;
    if (!path_exists_checked(final_path, "install destination", final_exists))
        return false;
    if (final_exists) {
        fs::rename(final_path, backup_path, fs_error);
        if (fs_error) {
            LOG_ERROR("Failed to stage existing install '{}' for replacement: {}", final_path, fs_error.message());
            return false;
        }
        moved_existing = true;
    }

    fs::rename(staged_path, final_path, fs_error);
    if (fs_error) {
        const std::string install_error = fs_error.message();
        if (moved_existing) {
            boost::system::error_code restore_error;
            fs::rename(backup_path, final_path, restore_error);
            if (restore_error)
                LOG_CRITICAL("Failed to restore install '{}' after replacement failure: {}", final_path, restore_error.message());
        }
        LOG_ERROR("Failed to commit staged install '{}': {}", final_path, install_error);
        return false;
    }

    if (moved_existing) {
        fs::remove_all(backup_path, fs_error);
        if (fs_error)
            LOG_WARN("Installed content, but failed to remove old backup '{}': {}", backup_path, fs_error.message());
    }
    return true;
}

bool decrypt_install_nonpdrm(EmuEnvState &emuenv, const fs::path &drmlicpath, const fs::path &title_path, const std::function<void(float)> &progress_callback) {
    fs::path title_id_src = title_path;
    fs::path title_id_dst = fs_utils::path_concat(title_path, "_dec");
    const fs::path backup_path = transaction_sibling(title_path, ".decrypt-backup");
    if (!prepare_staged_replace(title_path, title_id_dst, backup_path))
        return false;

    fs::ifstream binfile(drmlicpath, std::ios::in | std::ios::binary | std::ios::ate);
    if (!binfile) {
        LOG_ERROR("Failed to open NoNpDrm license '{}'", drmlicpath);
        return false;
    }
    std::string zRIF = rif2zrif(binfile);
    F00DEncryptorTypes f00d_enc_type = F00DEncryptorTypes::native;
    std::string f00d_arg = std::string();

    PfsProgressCallback pfs_progress = [&](std::uint64_t processed, std::uint64_t total, const std::string &) {
        if (progress_callback)
            progress_callback(total ? static_cast<float>(processed) / static_cast<float>(total) : 1.f);
    };

    const int decrypt_result = execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg, pfs_progress);
    const bool theme_without_keystone = emuenv.app_info.app_category == "theme"
        && decrypt_result == PFS_EXECUTE_MISSING_KEYSTONE;
    if (decrypt_result < 0 && !theme_without_keystone) {
        remove_path_checked(title_id_dst, "failed NoNpDrm output");
        return false;
    }
    if (theme_without_keystone)
        LOG_INFO("Theme PFS decrypted successfully; accepting the expected missing-keystone result");

    if (!emuenv.app_info.app_category.contains("gp") && !copy_license(emuenv, drmlicpath)) {
        remove_path_checked(title_id_dst, "uncommitted NoNpDrm output");
        return false;
    }

    return commit_staged_replace(title_id_src, title_id_dst, backup_path);
}

bool install_pkg(const fs::path &pkg_path, EmuEnvState &emuenv, std::string &p_zRIF, const std::function<void(float)> &progress_callback) {
    std::unique_ptr<FILE, decltype(&fclose)> infile_owner(FOPEN(pkg_path.c_str(), "rb"), &fclose);
    FILE *const infile = infile_owner.get();
    if (!infile) {
        LOG_CRITICAL("Failed to load pkg file in path: {}", fs_utils::path_to_utf8(pkg_path));
        return false;
    }

    if (fseek(infile, 0, SEEK_END) != 0) {
        LOG_ERROR("Failed to seek to the end of pkg file: {}", fs_utils::path_to_utf8(pkg_path));
        return false;
    }
    const auto pkg_size_result = ftell(infile);
    if (pkg_size_result < 0) {
        LOG_ERROR("Failed to determine pkg file size: {}", fs_utils::path_to_utf8(pkg_path));
        return false;
    }
    const uint64_t pkg_size = static_cast<uint64_t>(pkg_size_result);

    PkgHeader pkg_header{};
    PkgExtHeader ext_header{};
    if (pkg_size < sizeof(PkgHeader) + sizeof(PkgExtHeader)
        || fseek(infile, 0, SEEK_SET) != 0
        || fread(&pkg_header, sizeof(PkgHeader), 1, infile) != 1
        || fread(&ext_header, sizeof(PkgExtHeader), 1, infile) != 1) {
        LOG_ERROR("The pkg header is truncated or unreadable");
        return false;
    }

    if (progress_callback)
        progress_callback(0);

    if (byte_swap(pkg_header.magic) != 0x7F504b47 || byte_swap(ext_header.magic) != 0x7F657874) {
        LOG_ERROR("Not a valid pkg file!");
        return false;
    }

    const auto range_is_in_pkg = [pkg_size](const uint64_t offset, const uint64_t size) {
        return offset <= pkg_size && size <= pkg_size - offset;
    };

    if (pkg_size < byte_swap(pkg_header.total_size)) {
        LOG_ERROR("The pkg file is too small");
        return false;
    }

    const uint64_t pkg_data_offset = byte_swap(pkg_header.data_offset);
    const uint64_t pkg_file_count = byte_swap(pkg_header.file_count);
    if (!range_is_in_pkg(pkg_data_offset, pkg_file_count * sizeof(PkgEntry))) {
        LOG_ERROR("The pkg file is too small");
        return false;
    }
    const auto data_range_is_in_pkg = [pkg_size, pkg_data_offset](const uint64_t offset, const uint64_t size) {
        return pkg_data_offset <= pkg_size
            && offset <= pkg_size - pkg_data_offset
            && size <= pkg_size - pkg_data_offset - offset;
    };

    uint64_t info_offset = byte_swap(pkg_header.info_offset);
    uint32_t content_type = 0;
    uint32_t sfo_offset = 0;
    uint32_t sfo_size = 0;
    uint32_t items_offset = 0;

    const uint32_t info_count = byte_swap(pkg_header.info_count);
    if (info_count > pkg_size / (2 * sizeof(uint32_t))) {
        LOG_ERROR("The pkg info table is invalid");
        return false;
    }
    for (uint32_t i = 0; i < info_count; i++) {
        uint32_t block[4]{};
        if (!range_is_in_pkg(info_offset, sizeof(block))
            || fseek(infile, info_offset, SEEK_SET) != 0
            || fread(block, sizeof(block), 1, infile) != 1) {
            LOG_ERROR("The pkg info table is truncated at entry {}", i);
            return false;
        }

        auto type = byte_swap(block[0]);
        auto size = byte_swap(block[1]);

        switch (type) {
        case 2:
            content_type = byte_swap(block[2]);
            break;
        case 13:
            items_offset = byte_swap(block[2]);
            break;
        case 14:
            sfo_offset = byte_swap(block[2]);
            sfo_size = byte_swap(block[3]);
            break;
        default:
            break;
        }

        const uint64_t next_info_offset = info_offset + 2 * sizeof(uint32_t) + size;
        if (next_info_offset < info_offset || next_info_offset > pkg_size) {
            LOG_ERROR("The pkg info table has an invalid entry size at entry {}", i);
            return false;
        }
        info_offset = next_info_offset;
    }

    PkgType type;

    switch (content_type) {
    case 0x15:
        type = PkgType::PKG_TYPE_VITA_APP;
        break;
    case 0x16:
        type = PkgType::PKG_TYPE_VITA_DLC;
        break;
    case 0x1F:
        type = PkgType::PKG_TYPE_VITA_THEME;
        break;
    default:
        LOG_ERROR("Unsupported content type: {}", content_type);
        return false;
        break;
    }

    auto key_type = byte_swap(ext_header.data_type2) & 7;

    uint8_t main_key[16];
    const uint8_t *pkg_vita_key = nullptr;
    switch (key_type) {
    case 2:
        pkg_vita_key = pkg_vita_2;
        break;
    case 3:
        pkg_vita_key = pkg_vita_3;
        break;
    case 4:
        pkg_vita_key = pkg_vita_4;
        break;
    default:
        LOG_ERROR("Unknown encryption key");
        return false;
        break;
    }

    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher_ctx(
        EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    std::unique_ptr<EVP_CIPHER, decltype(&EVP_CIPHER_free)> cipher_CTR(
        EVP_CIPHER_fetch(nullptr, "AES-128-CTR", nullptr), &EVP_CIPHER_free);
    std::unique_ptr<EVP_CIPHER, decltype(&EVP_CIPHER_free)> cipher_ECB(
        EVP_CIPHER_fetch(nullptr, "AES-128-ECB", nullptr), &EVP_CIPHER_free);
    if (!cipher_ctx || !cipher_CTR || !cipher_ECB) {
        LOG_ERROR("OpenSSL AES ciphers are unavailable; cannot decrypt pkg");
        return false;
    }

    // get the main key
    int main_key_len = 0;
    int main_key_final_len = 0;
    if (EVP_EncryptInit_ex(cipher_ctx.get(), cipher_ECB.get(), nullptr, pkg_vita_key, nullptr) != 1
        || EVP_CIPHER_CTX_set_padding(cipher_ctx.get(), 0) != 1
        || EVP_EncryptUpdate(cipher_ctx.get(), main_key, &main_key_len, pkg_header.pkg_data_iv, 0x10) != 1
        || EVP_EncryptFinal_ex(cipher_ctx.get(), main_key + main_key_len, &main_key_final_len) != 1
        || main_key_len + main_key_final_len != static_cast<int>(sizeof(main_key))) {
        LOG_ERROR("Failed to derive the pkg AES key");
        return false;
    }

    if (!range_is_in_pkg(sfo_offset, sfo_size) || sfo_size < sizeof(SfoHeader)) {
        LOG_ERROR("The pkg SFO range is invalid");
        return false;
    }
    std::vector<uint8_t> sfo_buffer(sfo_size);
    SfoFile sfo_file;
    if (fseek(infile, sfo_offset, SEEK_SET) != 0
        || fread(sfo_buffer.data(), sfo_buffer.size(), 1, infile) != 1
        || !sfo::load(sfo_file, sfo_buffer)) {
        LOG_ERROR("Failed to read or parse the pkg SFO");
        return false;
    }
    sfo::get_param_info(emuenv.app_info, sfo_buffer, emuenv.cfg.sys_lang);

    if (!is_safe_path_component(emuenv.app_info.app_title_id)) {
        LOG_ERROR("The pkg contains an invalid title id");
        return false;
    }

    if (type == PkgType::PKG_TYPE_VITA_DLC) {
        if (emuenv.app_info.app_content_id.size() <= 20) {
            LOG_ERROR("The pkg contains an invalid DLC content id");
            return false;
        }
        emuenv.app_info.app_content_id = emuenv.app_info.app_content_id.substr(20);
    }

    if ((type == PkgType::PKG_TYPE_VITA_DLC || type == PkgType::PKG_TYPE_VITA_THEME)
        && !is_safe_path_component(emuenv.app_info.app_content_id)) {
        LOG_ERROR("The pkg contains an unsafe content id");
        return false;
    }

    if (type == PkgType::PKG_TYPE_VITA_APP && strcmp(emuenv.app_info.app_category.c_str(), "gp") == 0) {
        type = PkgType::PKG_TYPE_VITA_PATCH;
    }

    const fs::path ux0_path = emuenv.vita_fs_path / "ux0";
    fs::path content_final_path;
    fs::path install_final_path;

    switch (type) {
    case PkgType::PKG_TYPE_VITA_APP:
        content_final_path = ux0_path / "app" / emuenv.app_info.app_title_id;
        install_final_path = content_final_path;
        emuenv.app_info.app_title += " (App)";
        break;
    case PkgType::PKG_TYPE_VITA_DLC:
        content_final_path = ux0_path / "addcont" / emuenv.app_info.app_title_id / emuenv.app_info.app_content_id;
        install_final_path = content_final_path;
        emuenv.app_info.app_title += " (DLC)";
        break;
    case PkgType::PKG_TYPE_VITA_PATCH:
        content_final_path = ux0_path / "patch" / emuenv.app_info.app_title_id;
        // Vita3K's existing semantics overlay an update into ux0/app rather than
        // retaining ux0/patch. Build that overlay in a sibling transaction tree.
        install_final_path = ux0_path / "app" / emuenv.app_info.app_title_id;
        emuenv.app_info.app_title += " (Update)";
        break;
    case PkgType::PKG_TYPE_VITA_THEME:
        content_final_path = ux0_path / "theme" / emuenv.app_info.app_content_id;
        install_final_path = content_final_path;
        emuenv.app_info.app_category = "theme";
        emuenv.app_info.app_title += " (Theme)";
        break;
    }

    const bool is_patch = type == PkgType::PKG_TYPE_VITA_PATCH;
    if (is_patch && !fs::is_regular_file(install_final_path / "sce_sys/param.sfo")) {
        LOG_ERROR("Install app before patch: {} has no base app in {}", emuenv.app_info.app_title_id, install_final_path);
        return false;
    }
    fs::path path = transaction_sibling(content_final_path,
        is_patch ? ".pkg-extracting" : ".pkg-installing");
    const fs::path commit_staged_path = is_patch
        ? transaction_sibling(install_final_path, ".pkg-installing")
        : path;
    const fs::path backup_path = transaction_sibling(install_final_path, ".pkg-backup");
    if (!prepare_staged_replace(install_final_path, commit_staged_path, backup_path))
        return false;
    if (is_patch && !remove_path_checked(path, "stale patch extraction directory"))
        return false;
    if (!remove_path_checked(fs_utils::path_concat(path, "_dec"), "stale decrypted PKG staging directory"))
        return false;

    auto decrypt_aes_ctr = [&](uint64_t offset, unsigned char *data, size_t size) {
        if (size > static_cast<size_t>(INT_MAX))
            return false;
        uint8_t counter[0x10];
        ctr_init(counter, pkg_header.pkg_data_iv, offset);
        int decrypted_len = 0;
        int final_len = 0;
        return EVP_DecryptInit_ex(cipher_ctx.get(), cipher_CTR.get(), nullptr, main_key, counter) == 1
            && EVP_CIPHER_CTX_set_padding(cipher_ctx.get(), 0) == 1
            && EVP_DecryptUpdate(cipher_ctx.get(), data, &decrypted_len, data, static_cast<int>(size)) == 1
            && EVP_DecryptFinal_ex(cipher_ctx.get(), data + decrypted_len, &final_len) == 1
            && static_cast<size_t>(decrypted_len + final_len) == size;
    };

#ifdef __SWITCH__
    // SD-card writes through the fsdev devoptab are the extraction bottleneck on
    // Switch; a much larger streaming buffer cuts per-chunk syscall overhead and
    // improves SD throughput substantially.
    std::vector<uint8_t> buffer(0x400000); // 4 MiB
#else
    std::vector<uint8_t> buffer(0x10000);
#endif
    for (uint32_t i = 0; i < pkg_file_count; i++) {
        PkgEntry entry{};
        const uint64_t file_offset = static_cast<uint64_t>(items_offset) + static_cast<uint64_t>(i) * sizeof(PkgEntry);
        if (!data_range_is_in_pkg(file_offset, sizeof(PkgEntry))
            || fseek(infile, pkg_data_offset + file_offset, SEEK_SET) != 0
            || fread(&entry, sizeof(PkgEntry), 1, infile) != 1
            || !decrypt_aes_ctr(file_offset / 16, reinterpret_cast<unsigned char *>(&entry), sizeof(PkgEntry))) {
            LOG_ERROR("Failed to read or decrypt pkg entry {}", i);
            return false;
        }

        const uint64_t name_offset = byte_swap(entry.name_offset);
        const uint64_t name_size = byte_swap(entry.name_size);
        const uint64_t data_offset = byte_swap(entry.data_offset);
        const uint64_t entry_data_size = byte_swap(entry.data_size);
        if (!data_range_is_in_pkg(name_offset, name_size)
            || !data_range_is_in_pkg(data_offset, entry_data_size)
            || name_size == 0 || name_size > 1024 * 1024 || name_size > static_cast<uint64_t>(INT_MAX)) {
            LOG_ERROR("The pkg file size is too small, possibly corrupted (pkg_size={}, data_offset={}, entry {}: name_off={} name_sz={} data_off={} data_sz={})",
                pkg_size, pkg_data_offset, i, name_offset, name_size, data_offset, entry_data_size);
            return false;
        }

        if (progress_callback) {
            const auto file_count = static_cast<float>(pkg_file_count);
            progress_callback(static_cast<float>(i) / file_count * 100.f * 0.6f);
        }

        std::vector<unsigned char> name(static_cast<size_t>(name_size));
        if (fseek(infile, pkg_data_offset + name_offset, SEEK_SET) != 0
            || fread(name.data(), name.size(), 1, infile) != 1
            || !decrypt_aes_ctr(name_offset / 16, name.data(), name.size())) {
            LOG_ERROR("Failed to read or decrypt the name of pkg entry {}", i);
            return false;
        }

        while (!name.empty() && name.back() == 0)
            name.pop_back();
        const std::string string_name(name.begin(), name.end());
        fs::path relative_name;
        if (!get_safe_pkg_relative_path(string_name, relative_name)) {
            LOG_ERROR("Pkg entry {} has an unsafe path", i);
            return false;
        }

        LOG_INFO("{}", string_name);
        const fs::path output_path = path / relative_name;
        boost::system::error_code fs_error;

        if ((byte_swap(entry.type) & 0xFF) == 4 || (byte_swap(entry.type) & 0xFF) == 18) { // Directory
            fs::create_directories(output_path, fs_error);
            if (fs_error) {
                LOG_ERROR("Failed to create pkg directory '{}': {}", output_path, fs_error.message());
                return false;
            }
        } else { // File
            fs::create_directories(output_path.parent_path(), fs_error);
            if (fs_error) {
                LOG_ERROR("Failed to create parent directory for '{}': {}", output_path, fs_error.message());
                return false;
            }
            fs::ofstream outfile(output_path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!outfile.is_open()) {
                LOG_ERROR("Failed to create pkg output file '{}'", output_path);
                return false;
            }

            uint8_t counter[0x10];
            ctr_init(counter, pkg_header.pkg_data_iv, data_offset / 16);
            if (EVP_DecryptInit_ex(cipher_ctx.get(), cipher_CTR.get(), nullptr, main_key, counter) != 1
                || EVP_CIPHER_CTX_set_padding(cipher_ctx.get(), 0) != 1
                || fseek(infile, pkg_data_offset + data_offset, SEEK_SET) != 0) {
                LOG_ERROR("Failed to initialize decryption for pkg entry {}", i);
                return false;
            }

            uint64_t data_size = entry_data_size;
            while (data_size != 0) {
                const size_t size = static_cast<size_t>(std::min<uint64_t>(data_size, buffer.size()));
                int decrypted_len = 0;
                if (fread(buffer.data(), size, 1, infile) != 1
                    || EVP_DecryptUpdate(cipher_ctx.get(), buffer.data(), &decrypted_len,
                           buffer.data(), static_cast<int>(size))
                        != 1
                    || decrypted_len != static_cast<int>(size)) {
                    LOG_ERROR("Failed while decrypting pkg entry {}", i);
                    return false;
                }

                outfile.write(reinterpret_cast<char *>(buffer.data()), decrypted_len);
                if (!outfile.good()) {
                    LOG_ERROR("Failed while writing pkg output file '{}'", output_path);
                    return false;
                }
                data_size -= size;
            }

            int final_len = 0;
            if (EVP_DecryptFinal_ex(cipher_ctx.get(), buffer.data(), &final_len) != 1) {
                LOG_ERROR("Failed to finalize decryption for pkg entry {}", i);
                return false;
            }
            if (final_len != 0)
                outfile.write(reinterpret_cast<char *>(buffer.data()), final_len);
            outfile.flush();
            const bool write_succeeded = outfile.good();
            outfile.close();
            if (!write_succeeded || outfile.fail()) {
                LOG_ERROR("Failed to finish pkg output file '{}'", output_path);
                return false;
            }
        }
    }
    infile_owner.reset();
    fs::path title_id_src = path;
    fs::path title_id_dst = fs_utils::path_concat(path, "_dec");
    std::string zRIF = p_zRIF;
    F00DEncryptorTypes f00d_enc_type = F00DEncryptorTypes::native;
    std::string f00d_arg = std::string();

    if (progress_callback)
        progress_callback(80);
    const PfsProgressCallback pfs_progress = [&](std::uint64_t processed, std::uint64_t total, const std::string &) {
        if (progress_callback && total)
            progress_callback(80.f + static_cast<float>(processed) / static_cast<float>(total) * 10.f);
    };
    const int decrypt_result = execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg, pfs_progress);
    const bool theme_without_keystone = type == PkgType::PKG_TYPE_VITA_THEME
        && decrypt_result == PFS_EXECUTE_MISSING_KEYSTONE;
    if (decrypt_result < 0 && !theme_without_keystone) {
        remove_path_checked(title_id_src, "failed PKG staging directory");
        remove_path_checked(title_id_dst, "failed decrypted PKG output");
        return false;
    }
    if (theme_without_keystone)
        LOG_INFO("Theme PFS decrypted successfully; accepting the expected missing-keystone result");

    boost::system::error_code fs_error;
    if (!fs::is_directory(title_id_dst, fs_error) || fs_error) {
        LOG_ERROR("Decrypted PKG output '{}' is missing or unreadable{}{}", title_id_dst,
            fs_error ? ": " : "", fs_error ? fs_error.message() : "");
        return false;
    }
    if (!remove_path_checked(title_id_src, "encrypted PKG staging directory"))
        return false;
    fs::rename(title_id_dst, title_id_src, fs_error);
    if (fs_error) {
        LOG_ERROR("Failed to finalize decrypted PKG staging directory '{}': {}", title_id_src, fs_error.message());
        return false;
    }

    if (is_patch) {
        bool app_exists = false;
        if (!path_exists_checked(install_final_path, "base app", app_exists))
            return false;
        if (app_exists && !fs_utils::copy_directory_contents(install_final_path, commit_staged_path)) {
            LOG_ERROR("Failed to stage base app '{}' for update", install_final_path);
            return false;
        }
        if (!fs_utils::copy_directory_contents(title_id_src, commit_staged_path)) {
            LOG_ERROR("Failed to overlay staged update '{}'", title_id_src);
            return false;
        }
        if (!remove_path_checked(title_id_src, "decrypted patch staging directory"))
            return false;
    }

    // Apps and patches require a durable license before their directory swap.
    if ((type == PkgType::PKG_TYPE_VITA_APP || is_patch) && !create_license(emuenv, zRIF))
        return false;

    if (!commit_staged_replace(install_final_path, commit_staged_path, backup_path))
        return false;

    if (is_patch) {
        fs::remove_all(content_final_path, fs_error);
        if (fs_error)
            LOG_WARN("Update installed, but failed to remove legacy patch directory '{}': {}", content_final_path, fs_error.message());
    }

    if (progress_callback)
        progress_callback(100);
    return true;
}

bool peek_pkg_info(const fs::path &pkg_path, sfo::SfoAppInfo &info) {
    std::unique_ptr<FILE, decltype(&fclose)> infile(FOPEN(pkg_path.c_str(), "rb"), &fclose);
    if (!infile || fseek(infile.get(), 0, SEEK_END) != 0)
        return false;
    const long size_result = ftell(infile.get());
    if (size_result < 0)
        return false;
    const uint64_t pkg_size = static_cast<uint64_t>(size_result);

    PkgHeader pkg_header{};
    if (pkg_size < sizeof(PkgHeader) || fseek(infile.get(), 0, SEEK_SET) != 0
        || fread(&pkg_header, sizeof(PkgHeader), 1, infile.get()) != 1
        || byte_swap(pkg_header.magic) != 0x7F504B47)
        return false;

    const auto in_pkg = [pkg_size](const uint64_t offset, const uint64_t size) {
        return offset <= pkg_size && size <= pkg_size - offset;
    };
    uint64_t info_offset = byte_swap(pkg_header.info_offset);
    const uint32_t info_count = byte_swap(pkg_header.info_count);
    if (info_count > pkg_size / (2 * sizeof(uint32_t)))
        return false;
    uint32_t sfo_offset = 0;
    uint32_t sfo_size = 0;
    for (uint32_t i = 0; i < info_count; i++) {
        uint32_t block[4]{};
        if (!in_pkg(info_offset, sizeof(block)) || fseek(infile.get(), info_offset, SEEK_SET) != 0
            || fread(block, sizeof(block), 1, infile.get()) != 1)
            return false;
        if (byte_swap(block[0]) == 14) {
            sfo_offset = byte_swap(block[2]);
            sfo_size = byte_swap(block[3]);
        }
        const uint64_t next_info_offset = info_offset + 2 * sizeof(uint32_t) + byte_swap(block[1]);
        if (next_info_offset < info_offset || next_info_offset > pkg_size)
            return false;
        info_offset = next_info_offset;
    }
    if (!in_pkg(sfo_offset, sfo_size) || sfo_size < sizeof(SfoHeader))
        return false;

    std::vector<uint8_t> sfo_buffer(sfo_size);
    if (fseek(infile.get(), sfo_offset, SEEK_SET) != 0
        || fread(sfo_buffer.data(), sfo_buffer.size(), 1, infile.get()) != 1)
        return false;
    info = {};
    return sfo::get_param_info(info, sfo_buffer, 0);
}

std::string find_pkg_zrif(const fs::path &pkg_path, const fs::path &vita_fs_path) {
    std::unique_ptr<FILE, decltype(&fclose)> infile(FOPEN(pkg_path.c_str(), "rb"), &fclose);
    if (!infile)
        return {};

    PkgHeader pkg_header{};
    if (fread(&pkg_header, sizeof(PkgHeader), 1, infile.get()) != 1
        || byte_swap(pkg_header.magic) != 0x7F504B47) {
        LOG_ERROR("Cannot read a valid PKG header from '{}'", pkg_path);
        return {};
    }

    const char *const content_id_end = static_cast<const char *>(
        std::memchr(pkg_header.content_id, '\0', sizeof(pkg_header.content_id)));
    if (!content_id_end) {
        LOG_ERROR("PKG '{}' has an unterminated content id", pkg_path);
        return {};
    }
    const std::string content_id(pkg_header.content_id,
        static_cast<size_t>(content_id_end - pkg_header.content_id));
    if (content_id.size() < 16 || !is_safe_path_component(content_id))
        return {};

    const std::string title_id = content_id.substr(7, 9);
    if (!is_safe_path_component(title_id))
        return {};
    const auto rif_path = vita_fs_path / "ux0/license" / title_id / (content_id + ".rif");

    if (!fs::exists(rif_path))
        return {};

    LOG_INFO("Found license file: {}", rif_path);
    fs::ifstream binfile(rif_path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!binfile)
        return {};

    // Fail-fast license sanity check. psvpfstools' zRIF->klicensee path rejects a
    // rif whose uint16 at offset 4 is zero (get_license_type classifies it as
    // PsmDrm instead of NpDrm). A zeroed/short rif (e.g. a bad work.bin copy) would
    // otherwise only surface that error *after* the multi-minute PKG extraction.
    const std::streampos rif_size = binfile.tellg();
    uint8_t rif_head[8] = {};
    binfile.seekg(0, std::ios::beg);
    binfile.read(reinterpret_cast<char *>(rif_head), sizeof(rif_head));
    if (!binfile) {
        LOG_ERROR("Failed to read license header at {}", rif_path);
        return {};
    }
    const uint16_t lic_type_field = static_cast<uint16_t>(rif_head[4] | (rif_head[5] << 8));
    if (rif_size < 512 || lic_type_field == 0) {
        LOG_ERROR("License at {} is invalid (size={}, offset4=0x{:04x}): not a valid NpDrm rif — "
                  "the work.bin is missing or zeroed. Reinstall a valid work.bin.",
            rif_path, static_cast<long long>(rif_size), lic_type_field);
        return {};
    }

    binfile.seekg(0, std::ios::end); // rif2zrif expects tellg() == file size
    return rif2zrif(binfile);
}
