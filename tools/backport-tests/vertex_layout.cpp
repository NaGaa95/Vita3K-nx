#define VULKAN_HPP_NO_CONSTRUCTORS
#include <renderer/types.h>
#include <renderer/vulkan/vertex_input.h>
#include <renderer/vulkan/gxm_to_vulkan.h>
#include <gxm/functions.h>
#include <util/align.h>
#include <set>
#include <stdexcept>
#include <cassert>
#include <cstdio>

namespace renderer::vulkan {
struct PipelineVertexProgram {
    const SceGxmProgram *program;
    const std::vector<SceGxmVertexStream> &streams;
    const std::vector<SceGxmVertexAttribute> &attributes;
    const VertexProgram &renderer_data;
};
#include "vertex_layout.inc"
}

int main() {
    using namespace renderer;
    using namespace renderer::vulkan;
    VertexProgram renderer{};
    std::vector<SceGxmVertexStream> streams(2);
    streams[0].stride = 32;
    std::vector<SceGxmVertexAttribute> attributes(1);
    attributes[0].streamIndex = 0;
    attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attributes[0].componentCount = 4;
    attributes[0].regIndex = 0;
    renderer.attribute_infos.emplace(0, shader::usse::AttributeInformation(0, SCE_GXM_PARAMETER_TYPE_F32, 4, false, false, false));
    const PipelineVertexProgram program{nullptr, streams, attributes, renderer};
    auto plan = [&] { return make_vertex_input_layout(program, true, {}, 2047); };
    auto p = plan();
    assert(!p->streams[0].repack && p->streams[0].stride == 32 && p->streams[0].copies.empty());
    attributes[0].offset = 1;
    p = plan();
    assert(p->streams[0].repack && p->streams[0].stride == 16 && p->attributes[0].offset == 0);
    auto native = make_vertex_input_layout(program, true, {}, 2047, true);
    assert(!native->streams[0].repack && native->streams[0].alignment == 1 && native->streams[0].stride == 32);
    assert(native->attributes[0].offset == 1 && native->dynamic_attributes[0].offset == 1);
    assert(native->dynamic_bindings[0].stride == 32 && native->dynamic_bindings[0].divisor == 1);
    std::array<uint8_t, 65> unaligned;
    for (size_t i = 0; i < unaligned.size(); ++i) unaligned[i] = static_cast<uint8_t>(i + 1);
    std::array<uint8_t, 32> aligned{};
    repack_vertex_stream(p->streams[0], 32, unaligned.data(), unaligned.size(), aligned.data(), 2);
    for (size_t i = 0; i < 2; ++i)
        assert(std::memcmp(aligned.data() + i * 16, unaligned.data() + i * native->streams[0].stride + native->attributes[0].offset, 16) == 0);
    streams[0].stride = 0;
    p = plan();
    assert(p->streams[0].repack && p->streams[0].stride == 0 && p->streams[0].record_size == 16);
    streams[0].stride = 29;
    attributes[0].offset = 65001;
    p = plan();
    assert(p->streams[0].stride == 16 && p->streams[0].record_size == 16 && !p->attribute_bindings);
    native = make_vertex_input_layout(program, true, {}, 2047, true);
    assert(!native->streams[0].repack && native->streams[0].stride == 29 && native->attribute_bindings);
    assert(native->dynamic_attributes[0].offset == 0 && native->attribute_offsets[0] == 65001);
    streams[0].stride = 32;
    attributes[0].offset = 65000;
    p = plan();
    assert(!p->streams[0].repack && p->attribute_bindings && p->attributes[0].offset == 0);
    attributes[0].offset = 1;
    renderer.attribute_infos.at(0) = shader::usse::AttributeInformation(0, SCE_GXM_PARAMETER_TYPE_F16, 16, false, false, true);
    p = plan();
    assert(p->streams[0].stride == 32 && p->attributes.size() == 4);
    for (unsigned i = 0; i < 4; ++i) assert(p->attributes[i].offset == i * 8);
    native = make_vertex_input_layout(program, true, {}, 2047, true);
    assert(!native->streams[0].repack && native->dynamic_attributes.size() == 4);
    for (unsigned i = 0; i < 4; ++i) assert(native->dynamic_attributes[i].offset == i * 8 + 1);
    renderer.attribute_infos.at(0) = shader::usse::AttributeInformation(0, SCE_GXM_PARAMETER_TYPE_U8, 1, false, false, false);
    attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
    attributes[0].componentCount = 1;
    streams[0].stride = 5;
    p = plan();
    assert(!p->streams[0].repack && p->streams[0].stride == 5);
    attributes[0].componentCount = 3;
    attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    attributes[0].offset = 0;
    streams[0].stride = 3;
    p = make_vertex_input_layout(program, true, {vk::Format::eR8G8B8Unorm}, 2047);
    assert(p->streams[0].repack && p->repack_vertices && p->streams[0].stride == 4 && p->streams[0].copies[0].size == 3);
    native = make_vertex_input_layout(program, true, {vk::Format::eR8G8B8Unorm}, 2047, true);
    assert(native->streams[0].repack && native->streams[0].stride == 4 && native->dynamic_bindings[0].stride == 4);
    assert(native->streams[0].copies[0].size == 3);
    const uint8_t source[]{1, 2, 3, 4, 5, 6};
    std::array<uint8_t, 8> padded{};
    repack_vertex_stream(p->streams[0], 3, source, sizeof(source), padded.data(), 2);
    assert((padded == std::array<uint8_t, 8>{1, 2, 3, 0, 4, 5, 6, 0}));
    attributes[0].streamIndex = 3;
    bool rejected = false;
    try { p = plan(); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::puts("Vertex layout: native unaligned fetches, equivalent attribute bytes, large offsets, matrices, required format padding and legacy layouts passed");
}
