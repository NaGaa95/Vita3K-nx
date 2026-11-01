#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>

using SceOff = int64_t;
using SceSize = uint32_t;
constexpr uint32_t page_size = 4096;
constexpr int SCE_O_RDONLY = 1;

struct FileStats {
    std::unique_ptr<FILE, decltype(&fclose)> wrapped_file{ tmpfile(), fclose };
    struct SharedFile {
        FILE *file;
        int64_t read_at(void *buffer, size_t size, int64_t offset) {
            assert(fseek(file, static_cast<long>(offset), SEEK_SET) == 0);
            return fread(buffer, 1, size, file);
        }
    } shared{ wrapped_file.get() };
    SharedFile *shared_file = &shared;
    mutable SceOff file_offset = 0;
    FILE *get_file_pointer() const { return wrapped_file.get(); }
    int get_open_mode() const { return SCE_O_RDONLY; }
    SceOff read(void *, int, SceSize) const;
};

#include "io_read.inc"

int main() {
    for (const size_t length : { 0u, 1u, 4095u, 4096u, 4097u, 8200u }) {
        FileStats file;
        assert(file.wrapped_file);
        std::array<uint8_t, 8200> source;
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<uint8_t>(i * 37 + 1);
        assert(fwrite(source.data(), 1, length, file.wrapped_file.get()) == length);
        rewind(file.wrapped_file.get());
        std::array<uint8_t, 8202> output;
        output.fill(0xA5);
        assert(file.read(output.data() + 1, 1, 8200) == static_cast<SceOff>(length));
        assert(std::equal(source.begin(), source.begin() + length, output.begin() + 1));
        assert(output.front() == 0xA5);
        assert(std::all_of(output.begin() + 1 + length, output.end(), [](uint8_t byte) { return byte == 0xA5; }));
        const auto before = output;
        assert(file.read(output.data() + 1, 1, 8200) == 0);
        assert(output == before);
        assert(file.read(nullptr, 0, 100) == 0);
        assert(file.read(nullptr, 1, 0) == 0);
    }
    std::cout << "File reads: short reads, EOF, page boundaries and untouched tail bytes passed\n";
}
