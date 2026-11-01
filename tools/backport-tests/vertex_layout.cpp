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
    streams[0].stride = 0;
    p = plan();
    assert(p->streams[0].repack && p->streams[0].stride == 0 && p->streams[0].record_size == 16);
    streams[0].stride = 29;
    attributes[0].offset = 65001;
    p = plan();
    assert(p->streams[0].stride == 16 && p->streams[0].record_size == 16 && !p->attribute_bindings);
    streams[0].stride = 32;
    attributes[0].offset = 65000;
    p = plan();
    assert(!p->streams[0].repack && p->attribute_bindings && p->attributes[0].offset == 0);
    attributes[0].offset = 1;
    renderer.attribute_infos.at(0) = shader::usse::AttributeInformation(0, SCE_GXM_PARAMETER_TYPE_F16, 16, false, false, true);
    p = plan();
    assert(p->streams[0].stride == 32 && p->attributes.size() == 4);
    for (unsigned i = 0; i < 4; ++i) assert(p->attributes[i].offset == i * 8);
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
    const uint8_t source[]{1, 2, 3, 4, 5, 6};
    std::array<uint8_t, 8> padded{};
    repack_vertex_stream(p->streams[0], 3, source, sizeof(source), padded.data(), 2);
    assert((padded == std::array<uint8_t, 8>{1, 2, 3, 0, 4, 5, 6, 0}));
    attributes[0].streamIndex = 3;
    bool rejected = false;
    try { p = plan(); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::puts("Vertex layout: cached aligned path, compact large offsets, zero stride, half matrices, byte streams and invalid bindings passed");
}
