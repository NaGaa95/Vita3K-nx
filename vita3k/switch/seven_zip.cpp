// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "seven_zip.h"

#include <libarchive/archive.h>
#include <libarchive/archive_entry.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

#ifdef __SWITCH__
#include <locale.h>
#include <switch.h>
#endif

namespace {

#ifdef __SWITCH__
class ArchiveLocale {
    locale_t locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
    locale_t previous = nullptr;

public:
    ArchiveLocale() {
        if (locale)
            previous = uselocale(locale);
        if (!previous) {
            if (locale)
                freelocale(locale);
            throw std::runtime_error("Could not set UTF-8 locale for 7z filenames");
        }
    }
    ~ArchiveLocale() {
        uselocale(previous);
        freelocale(locale);
    }
};
#endif

fs::path entry_path(const char *name) {
    if (!name)
        throw std::runtime_error("7z entry has no readable filename");
    std::string normalized(name);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty() || normalized.front() == '/' || normalized.find(':') != std::string::npos)
        throw std::runtime_error("7z entry has an unsafe path");
    fs::path result;
    size_t begin = 0;
    while (begin < normalized.size()) {
        const size_t end = normalized.find('/', begin);
        const std::string part = normalized.substr(begin, end == std::string::npos ? end : end - begin);
        if (part == ".." || std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; }))
            throw std::runtime_error("7z entry has an unsafe path");
        if (!part.empty() && part != ".")
            result /= fs_utils::utf8_to_path(part);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return result.empty() ? fs::path(".") : result;
}

void check_archive(struct archive *reader, int result) {
    if (result != ARCHIVE_OK) {
        const char *error = archive_error_string(reader);
        throw std::runtime_error(error ? error : "Could not read 7z archive");
    }
}

} // namespace

ExtractedSevenZip::~ExtractedSevenZip() {
    if (!directory.empty()) {
        std::error_code error;
        fs::remove_all(directory, error);
    }
}

std::unique_ptr<ExtractedSevenZip> extract_seven_zip(const fs::path &source, const fs::path &temporary_parent,
    const std::function<void(const std::string &, uint64_t, uint64_t)> &progress) {
#ifdef __SWITCH__
    ArchiveLocale locale;
#endif
    auto result = std::make_unique<ExtractedSevenZip>();
    fs::create_directories(temporary_parent);
#ifdef __SWITCH__
    const fs::path directory = temporary_parent / fmt::format(".7z-{:016x}", randomGet64());
#else
    const fs::path directory = temporary_parent / fs::unique_path(".7z-%%%%-%%%%-%%%%");
#endif
    if (!fs::create_directory(directory))
        throw std::runtime_error("Could not create the 7z extraction directory");
    result->directory = directory;

    std::unique_ptr<FILE, decltype(&std::fclose)> input(FOPEN(source.c_str(), "rb"), &std::fclose);
    if (!input)
        throw std::runtime_error("Could not open the 7z archive");
    std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
    if (!reader)
        throw std::runtime_error("Could not allocate the 7z reader");
    check_archive(reader.get(), archive_read_support_format_7zip(reader.get()));
    check_archive(reader.get(), archive_read_open_FILE(reader.get(), input.get()));

    std::vector<char> buffer(1024 * 1024);
    std::set<fs::path> prepared_directories;
    std::set<std::string> written_files;
    const auto prepare_directory = [&](const fs::path &path) {
        if (!prepared_directories.contains(path)) {
            fs::create_directories(path);
            prepared_directories.insert(path);
        }
    };
    struct archive_entry *entry = nullptr;
    for (;;) {
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF)
            break;
        check_archive(reader.get(), status);
        if (archive_entry_is_encrypted(entry))
            throw std::runtime_error("Password-protected 7z archives are not supported");
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
            throw std::runtime_error("Links are not supported in game archives");
        const fs::path relative = entry_path(archive_entry_pathname_utf8(entry));
        const fs::path destination = directory / relative;
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            prepare_directory(destination);
            continue;
        }
        if (relative == "." || archive_entry_filetype(entry) != AE_IFREG || !archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0)
            throw std::runtime_error("Unsupported 7z entry type or size");
        std::string identity = fs_utils::path_to_utf8(relative);
        std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        });
        if (!written_files.insert(identity).second)
            throw std::runtime_error("Duplicate filename in 7z archive");
        prepare_directory(destination.parent_path());
        std::unique_ptr<FILE, decltype(&std::fclose)> output(FOPEN(destination.c_str(), "wbx"), &std::fclose);
        if (!output)
            throw std::runtime_error("Could not create extracted file: " + identity);
        const uint64_t expected = static_cast<uint64_t>(archive_entry_size(entry));
        uint64_t written = 0;
        do {
            if (progress)
                progress(fs_utils::path_to_utf8(relative), written, expected);
            const la_ssize_t count = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (count < 0)
                check_archive(reader.get(), ARCHIVE_FATAL);
            if (count == 0)
                break;
            if (static_cast<uint64_t>(count) > expected - written)
                throw std::runtime_error("7z file exceeds its declared size");
            if (std::fwrite(buffer.data(), 1, static_cast<size_t>(count), output.get()) != static_cast<size_t>(count))
                throw std::runtime_error("Could not write extracted file: " + identity);
            written += static_cast<uint64_t>(count);
        } while (true);
        const bool closed = std::fclose(output.release()) == 0;
        if (!closed || written != expected)
            throw std::runtime_error("Incomplete extracted file: " + identity);
        if (archive_entry_mtime_is_set(entry)) {
            std::error_code error;
            fs::last_write_time(destination, fs::file_time_type::clock::from_sys(std::chrono::system_clock::from_time_t(archive_entry_mtime(entry))), error);
        }
        result->files.push_back(destination);
    }
    check_archive(reader.get(), archive_read_close(reader.get()));
    return result;
}
