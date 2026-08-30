// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace renderer::texture {

struct ReadbackRows {
    uint32_t stride = 0;
    uint32_t row_bytes = 0;
    uint32_t count = 0;

    std::optional<uint32_t> span_size() const {
        if (!count || !row_bytes || row_bytes > stride)
            return std::nullopt;
        const uint64_t size = uint64_t(count - 1) * stride + row_bytes;
        if (size > std::numeric_limits<uint32_t>::max())
            return std::nullopt;
        return uint32_t(size);
    }
};

inline bool readback_range_fits(uint64_t base, uint64_t size, uint64_t address, uint64_t bytes) {
    return address >= base && bytes <= size && address - base <= size - bytes;
}

inline bool copy_readback_rows(std::span<uint8_t> destination, std::span<const uint8_t> source, ReadbackRows rows) {
    const auto size = rows.span_size();
    if (!size || *size > destination.size() || *size > source.size())
        return false;
    for (uint32_t row = 0; row < rows.count; ++row)
        memcpy(destination.data() + size_t(row) * rows.stride, source.data() + size_t(row) * rows.stride, rows.row_bytes);
    return true;
}

} // namespace renderer::texture
