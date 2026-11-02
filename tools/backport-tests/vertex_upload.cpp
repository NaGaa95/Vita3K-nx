#include <renderer/vertex_stream.h>
#include <util/align.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <set>
#include <stdexcept>

namespace vk {
using Buffer = uint64_t;
using DeviceSize = uint64_t;
}

struct MemState {
    std::array<uint8_t, 4096> bytes{};
};
static bool is_valid_addr_range_size(const MemState &mem, uint32_t address, uint64_t size) {
    return address < mem.bytes.size() && size <= mem.bytes.size() - address;
}
enum SceGxmIndexSource { PerVertex, PerInstance };
namespace gxm {
static bool is_stream_instancing(SceGxmIndexSource source) { return source == PerInstance; }
}
constexpr int SCE_GXM_MAX_VERTEX_STREAMS = 16;

namespace renderer::vulkan {
struct RingBuffer {
    uint32_t cursor = ~0u, capacity = 4096, alignment = 16, data_offset = 0;
    unsigned creations = 0, copies = 0, flushes = 0;
    std::vector<uint8_t> bytes;

    uint32_t size() const { return capacity; }
    vk::Buffer handle() const { return creations ? 42 : 0; }
    void create() { assert(!handle()); bytes.resize(capacity); cursor = 0; ++creations; }
    void allocate(uint32_t data_size);
    uint8_t *mapped_data() {
        assert(handle() && data_offset < bytes.size());
        return bytes.data() + data_offset;
    }
    void flush(uint32_t size) { assert(data_offset + size <= bytes.size()); ++flushes; }
    void allocate(int, uint32_t size, const void *data) {
        allocate(size);
        std::memcpy(mapped_data(), data, size);
        ++copies;
    }
};

struct GuestPtr {
    uint32_t value = 0;
    explicit operator bool() const { return value != 0; }
    GuestPtr &operator=(std::nullptr_t) { value = 0; return *this; }
    uint32_t address() const { return value; }
    const uint8_t *get(const MemState &mem) const { return mem.bytes.data() + value; }
    template <typename> GuestPtr cast() const { return *this; }
};
struct VertexProgram { std::set<uint32_t> attribute_infos{ 0 }; };
struct SceGxmVertexProgram {
    struct Attribute { uint32_t streamIndex = 0, regIndex = 0; };
    struct Stream { uint32_t stride = 12, indexSource = PerVertex; };
    std::vector<Attribute> attributes{ {} };
    std::array<Stream, SCE_GXM_MAX_VERTEX_STREAMS> streams{};
    std::unique_ptr<VertexProgram> renderer_data = std::make_unique<VertexProgram>();
};
using SceGxmVertexAttribute = SceGxmVertexProgram::Attribute;
struct ProgramRef {
    SceGxmVertexProgram *program = nullptr;
    SceGxmVertexProgram *get(MemState &) const { return program; }
};
struct GxmRecordState {
    ProgramRef vertex_program;
    struct Stream { GuestPtr data; uint64_t size = 0; };
    std::array<Stream, SCE_GXM_MAX_VERTEX_STREAMS> vertex_streams{};
};
struct Layout {
    unsigned stream_count = 1, used_streams = 1;
    bool attribute_bindings = false;
    std::array<VertexStreamLayout, SCE_GXM_MAX_VERTEX_STREAMS> streams{};
    std::array<uint32_t, SCE_GXM_MAX_VERTEX_STREAMS> attribute_offsets{};
};
struct State {
    struct { bool enable_memory_mapping = true; } features;
    struct {
        Layout layout;
        const Layout &vertex_input_layout(const SceGxmVertexProgram &) const { return layout; }
    } pipeline_cache;
    struct {
        void access_buffer(uint32_t, uint32_t, MemState &, bool) {}
    } buffer_trapping;
    bool has_shader_store = false, cpu_sync = false;
    unsigned mapped_uploads = 0;
    bool need_cpu_buffer_sync() const { return cpu_sync; }
    std::pair<vk::Buffer, vk::DeviceSize> get_matching_mapping(GuestPtr p) { ++mapped_uploads; return { 99, p.address() }; }
};
struct VKContext {
    State state;
    GxmRecordState record;
    RingBuffer vertex_stream_ring_buffer;
    vk::Buffer vertex_stream_buffers[SCE_GXM_MAX_VERTEX_STREAMS]{};
    vk::DeviceSize vertex_stream_offsets[SCE_GXM_MAX_VERTEX_STREAMS]{};
    struct {
        template <typename... Args> void bindVertexBuffers(Args &&...) {}
    } render_cmd;
    int prerender_cmd = 0;
};

#include "vertex_upload.inc"
}

int main() {
    using namespace renderer::vulkan;
    RingBuffer uncreated;
    uncreated.allocate(0x5f0);
    assert(uncreated.data_offset == 0xffffffff);

    MemState mem;
    for (size_t i = 0; i < mem.bytes.size(); ++i)
        mem.bytes[i] = static_cast<uint8_t>(i % 251 + 1);
    SceGxmVertexProgram program;
    const auto setup = [&](VKContext &context, bool repack, uint32_t address = 256) {
        context.record.vertex_program.program = &program;
        context.record.vertex_streams[0] = { { address }, 95 * 12 };
        auto &layout = context.state.pipeline_cache.layout.streams[0];
        layout.stride = repack ? 16 : 12;
        layout.record_size = repack ? 16 : 12;
        layout.source_record_size = 12;
        layout.alignment = 4;
        layout.repack = repack;
        layout.copies = { { 0, 0, 12 } };
    };

    VKContext direct;
    setup(direct, false);
    bind_vertex_streams(direct, mem, 1, 94);
    assert(direct.state.mapped_uploads == 1 && direct.vertex_stream_ring_buffer.creations == 0);
    assert(direct.vertex_stream_buffers[0] == 99 && direct.vertex_stream_offsets[0] == 256);

    VKContext packed;
    setup(packed, true);
    bind_vertex_streams(packed, mem, 1, 94);
    auto &buffer = packed.vertex_stream_ring_buffer;
    assert(buffer.creations == 1 && buffer.flushes == 1 && buffer.data_offset == 0);
    assert(packed.vertex_stream_buffers[0] == 42);
    for (size_t vertex = 0; vertex < 95; ++vertex) {
        assert(std::memcmp(buffer.bytes.data() + vertex * 16, mem.bytes.data() + 256 + vertex * 12, 12) == 0);
        for (size_t padding = 12; padding < 16; ++padding)
            assert(buffer.bytes[vertex * 16 + padding] == 0);
    }
    setup(packed, true);
    bind_vertex_streams(packed, mem, 1, 94);
    assert(buffer.creations == 1 && buffer.flushes == 2 && buffer.data_offset == 0x5f0);

    VKContext unaligned;
    setup(unaligned, false, 258);
    bind_vertex_streams(unaligned, mem, 1, 94);
    assert(unaligned.vertex_stream_ring_buffer.creations == 1 && unaligned.vertex_stream_ring_buffer.copies == 1);
    assert(std::memcmp(unaligned.vertex_stream_ring_buffer.bytes.data(), mem.bytes.data() + 258, 95 * 12) == 0);

    VKContext copied;
    copied.state.features.enable_memory_mapping = false;
    copied.state.cpu_sync = true;
    copied.vertex_stream_ring_buffer.create();
    setup(copied, true);
    bind_vertex_streams(copied, mem, 1, 94);
    assert(copied.vertex_stream_ring_buffer.creations == 1 && copied.vertex_stream_ring_buffer.flushes == 1);
    std::puts("Vertex upload: direct mapping, first repacked/unaligned upload, buffer reuse and precreated buffers passed");
}
