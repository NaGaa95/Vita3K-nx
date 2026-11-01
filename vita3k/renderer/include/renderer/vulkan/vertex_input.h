#pragma once

#include <gxm/types.h>
#include <renderer/vertex_stream.h>
#include <vulkan/vulkan.hpp>

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <vector>

namespace renderer::vulkan {

struct VertexInputLayout {
    std::array<VertexStreamLayout, SCE_GXM_MAX_VERTEX_STREAMS> streams;
    std::vector<vk::VertexInputBindingDescription> bindings;
    std::vector<vk::VertexInputAttributeDescription> attributes;
    std::vector<uint32_t> attribute_offsets;
    uint32_t used_streams = 0;
    uint32_t stream_count = 0;
    bool attribute_bindings = false;
    bool repack_vertices = false;
    bool extended_vertex_source = false;
};


} // namespace renderer::vulkan
