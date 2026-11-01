// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <util/fs_remove.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
static fs::path fixture;

#ifdef __SWITCH__
#include <switch.h>
static unsigned native_calls = 0;
static Result native_error = 0;

Result fsdevDeleteDirectoryRecursively(const char *name) {
    const fs::path path(name);
    assert(path.is_absolute() && path.lexically_relative(fixture).begin()->native() != "..");
    ++native_calls;
    if (native_error)
        return native_error;
    std::error_code error;
    fs::remove_all(path, error);
    assert(!error);
    return 0;
}
#endif

static void write(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << "installation fixture";
    assert(file.good());
}

int main() {
    fixture = fs::temp_directory_path() / ("vita3k-install-cleanup-" + std::to_string(getpid()));
    assert(fixture.is_absolute() && fixture.parent_path() == fs::temp_directory_path());
    assert(!fs::exists(fixture));
    fs::create_directory(fixture);
    const auto payload = fixture / ".PCSB00830.pkg-installing";
    const auto decrypted = fixture / ".PCSB00830.pkg-installing_dec";
    const auto live = fixture / "PCSB00830";
    const auto outside = fixture / "unrelated";
    write(live / "original-save-marker");
    write(decrypted / "eboot.bin");
    write(outside / "keep");
    write(payload / "sce_sys/param.sfo");
    write(payload / "data/nested/large.apk");
    write(payload / "hidden/.metadata");
    write(payload / "unicode-é/file.bin");
    fs::create_directories(payload / "empty/deep/leaf");
    fs::create_directory_symlink(outside, payload / "outside-link");
    fs::create_symlink(outside / "missing", payload / "dangling-link");
    std::error_code error = std::make_error_code(std::errc::permission_denied);
    assert(fs_utils::remove_path(fixture / "missing", error) && !error);
    assert(fs_utils::remove_path(fixture / "missing", error) && !error);
    assert(fs_utils::remove_path(payload, error) && !error && !fs::exists(payload));
    assert(fs::is_regular_file(outside / "keep"));
    assert(fs::is_regular_file(live / "original-save-marker"));
    assert(fs::is_regular_file(decrypted / "eboot.bin"));
#ifdef __SWITCH__
    assert(native_calls == 1);
#endif
    write(fixture / "single-file");
    fs_utils::remove_path(fixture / "single-file");
    assert(!fs::exists(fixture / "single-file"));
    fs::create_directory_symlink(outside, fixture / "single-link");
    fs_utils::remove_path(fixture / "single-link");
    assert(fs::is_regular_file(outside / "keep"));
    assert(!fs_utils::remove_path(outside / "keep/invalid", error) && error == std::errc::not_a_directory);
#ifdef __SWITCH__
    assert(native_calls == 1);
    native_error = 0x7d202;
    assert(!fs_utils::remove_path(decrypted, error) && error == std::errc::io_error);
    assert(fs::exists(decrypted / "eboot.bin") && fs::exists(live / "original-save-marker"));
    bool threw = false;
    try {
        fs_utils::remove_path(decrypted);
    } catch (const fs::filesystem_error &) {
        threw = true;
    }
    assert(threw && fs::exists(decrypted / "eboot.bin"));
    native_error = 0;
#endif
    fs_utils::remove_path(decrypted);
    assert(!fs::exists(decrypted));
    fs_utils::remove_path(fixture);
    assert(!fs::exists(fixture));
    std::cout << "PASS: nested/empty trees, files, symlinks, missing paths, preserved live installation";
#ifdef __SWITCH__
    std::cout << ", native service routing and failure propagation";
#endif
    std::cout << '\n';
}
