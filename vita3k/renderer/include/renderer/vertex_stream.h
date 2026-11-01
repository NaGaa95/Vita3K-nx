#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace renderer {

struct VertexStreamLayout {
    struct Copy {
        uint32_t source;
        uint32_t destination;
        uint32_t size;
    };
    uint32_t stride = 0;
    uint32_t record_size = 0;
    uint32_t source_record_size = 0;
    uint32_t alignment = 1;
    bool repack = false;
    bool extended_source = false;
    std::vector<Copy> copies;
};

inline void repack_vertex_stream(const VertexStreamLayout &layout, uint32_t source_stride,
    const uint8_t *source, size_t source_size, uint8_t *destination, size_t records) {
    const size_t destination_size = records ? (records - 1) * size_t(layout.stride) + layout.record_size : 0;
    std::memset(destination, 0, destination_size);
    for (size_t vertex = 0; vertex < records; ++vertex) {
        for (const auto &copy : layout.copies) {
            const uint64_t offset = vertex * uint64_t(source_stride) + copy.source;
            if (offset < source_size)
                std::memcpy(destination + vertex * layout.stride + copy.destination,
                    source + offset, std::min<size_t>(copy.size, source_size - offset));
        }
    }
}

} // namespace renderer
