// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#include <renderer/texture/readback.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

using namespace renderer::texture;

static void check(bool condition) {
    if (!condition) {
        std::cerr << "Readback bounds check failed\n";
        std::exit(1);
    }
}

int main() {
    check(ReadbackRows{ 32, 12, 3 }.span_size() == 76);
    check(ReadbackRows{ 32, 12, 1 }.span_size() == 12);
    check(!ReadbackRows{ 32, 12, 0 }.span_size());
    check(!ReadbackRows{ 32, 0, 1 }.span_size());
    check(!ReadbackRows{ 8, 12, 1 }.span_size());
    check(!ReadbackRows{ UINT32_MAX, 1, 3 }.span_size());
    check(ReadbackRows{ UINT32_MAX, UINT32_MAX, 1 }.span_size() == UINT32_MAX);
    check(readback_range_fits(0xfffffff0, 16, 0xfffffffc, 4));
    check(!readback_range_fits(0xfffffff0, 16, 0xfffffffc, 5));
    check(!readback_range_fits(100, 16, 99, 1));
    check(!readback_range_fits(100, 16, 100, 17));
    check(readback_range_fits(UINT64_MAX - 8, 8, UINT64_MAX - 4, 4));

    for (uint32_t stride = 1; stride <= 32; ++stride) {
        for (uint32_t width = 1; width <= stride; ++width) {
            for (uint32_t rows = 1; rows <= 4; ++rows) {
                std::array<uint8_t, 160> destination, source;
                destination.fill(0xaa);
                source.fill(0x55);
                const ReadbackRows layout{ stride, width, rows };
                const size_t size = *layout.span_size();
                const auto out = std::span(destination).subspan(7, size);
                const auto in = std::span<const uint8_t>(source).subspan(9, size);
                check(copy_readback_rows(out, in, layout));
                for (size_t i = 0; i < destination.size(); ++i) {
                    const bool written = i >= 7 && i < 7 + size && (i - 7) % stride < width;
                    check(destination[i] == (written ? 0x55 : 0xaa));
                }
                check(!copy_readback_rows(out.first(size - 1), in, layout));
                check(!copy_readback_rows(out, in.first(size - 1), layout));
            }
        }
    }
    std::cout << "Readback bounds checks passed\n";
}
