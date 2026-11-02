// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <renderer/vulkan/pipeline_cache.h>

#include <renderer/vulkan/gxm_to_vulkan.h>
#include <renderer/vulkan/state.h>
#include <renderer/vulkan/types.h>
#include <renderer/vulkan/vertex_input.h>

#include <gxm/functions.h>
#include <gxm/types.h>
#include <renderer/shaders.h>
#include <shader/spirv_recompiler.h>

#include <util/fs.h>
#include <util/log.h>
#include <util/switch_storage.h>
#include <util/switch_thread.h>

#include <SDL3/SDL_cpuinfo.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

// don't use the dispatch version, because we always hash a small amount
// with a known size
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>

namespace renderer::vulkan {

// Size of the record containing what is needed for the pipeline construction (what is after is dynamic state)
constexpr size_t record_pipeline_len = offsetof(GxmRecordState, vertex_streams);

struct PipelineVertexProgram {
    const SceGxmProgram *program;
    const std::vector<SceGxmVertexStream> &streams;
    const std::vector<SceGxmVertexAttribute> &attributes;
    const VertexProgram &renderer_data;
};

struct PipelineFragmentProgram {
    const SceGxmProgram &program;
    bool is_maskupdate;
    const VKFragmentProgram &renderer_data;
};

// structure containing everything needed to compile a pipeline
struct CompileRequest {
    std::atomic<vk::Pipeline> *pipeline;

    // this is everything we need to compile the shader on another thread (as the original data will change)
    SceGxmPrimitiveType type;
    vk::RenderPass render_pass;
    std::vector<uint32_t> vertex_program;
    std::vector<SceGxmVertexStream> vertex_streams;
    std::vector<SceGxmVertexAttribute> vertex_attributes;
    VertexProgram vertex_renderer_data;
    std::vector<uint32_t> fragment_program;
    bool fragment_is_maskupdate;
    VKFragmentProgram fragment_renderer_data;
    shader::Hints hints;
    bool has_casts;
    bool with_raw_attachment;

    // the content of the record useful for the pipeline creation
    alignas(8) uint8_t record_data[record_pipeline_len];

    const GxmRecordState *get_record() {
        // note: this object is only half defined, but we are only looking at the part that's defined
        return reinterpret_cast<const GxmRecordState *>(record_data);
    }

    PipelineVertexProgram get_vertex_program() const {
        return { reinterpret_cast<const SceGxmProgram *>(vertex_program.data()), vertex_streams, vertex_attributes, vertex_renderer_data };
    }

    PipelineFragmentProgram get_fragment_program() const {
        return { *reinterpret_cast<const SceGxmProgram *>(fragment_program.data()), fragment_is_maskupdate, fragment_renderer_data };
    }
};

static std::vector<uint32_t> copy_program(const SceGxmProgram &program) {
    std::vector<uint32_t> copy((program.size + sizeof(uint32_t) - 1) / sizeof(uint32_t));
    std::memcpy(copy.data(), &program, program.size);
    return copy;
}

PipelineCache::PipelineCache(VKState &state)
    : state(state)
    , pipeline_compile_queue_token(pipeline_compile_queue) {
}

void PipelineCache::init(bool support_rasterized_order_access) {
    vk::PipelineCacheCreateInfo pipeline_info{};
    pipeline_cache = state.device.createPipelineCache(pipeline_info);

    // the layout for uniforms buffer can be made here as it will always be the same
    {
        std::array<vk::DescriptorSetLayoutBinding, 4> layout_bindings;
        // Our vertex uniform (GXMRenderVertUniformBlock)
        layout_bindings[0] = vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
        // Our fragment uniform (GXMRenderFragUniformBlock)
        layout_bindings[1] = vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };
        // GXM vertex uniform (if no memory mapping)
        layout_bindings[2] = vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
        // GXM Fragment uniform (if no memory mapping)
        layout_bindings[3] = vk::DescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };

        vk::DescriptorSetLayoutCreateInfo descriptor_info{
            .bindingCount = state.features.enable_memory_mapping ? 2U : 4U,
            .pBindings = layout_bindings.data()
        };
        uniforms_layout = state.device.createDescriptorSetLayout(descriptor_info);
    }

    {
        // layout for the mask, color attachment as input, being an input attachment or a storage image
        // depending on whether or not we are using shader interlock
        std::array<vk::DescriptorSetLayoutBinding, 4> layout_binding;
        const vk::DescriptorType intput_image_descriptor = state.features.support_shader_interlock
            ? vk::DescriptorType::eStorageImage
            : vk::DescriptorType::eInputAttachment;
        uint32_t binding_count = 0;
        layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = intput_image_descriptor,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment
        };
        if (state.features.use_mask_bit)
            layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment
            };
        if (state.features.preserve_f16_nan_as_u16)
            layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
                .binding = 2,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment
            };

        if (state.features.use_mask_bit && state.features.support_shader_interlock)
            layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
                .binding = 3,
                .descriptorType = vk::DescriptorType::eInputAttachment,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment
            };

        vk::DescriptorSetLayoutCreateInfo descriptor_info{
            .bindingCount = binding_count,
            .pBindings = layout_binding.data()
        };
        attachments_layout = state.device.createDescriptorSetLayout(descriptor_info);
    }

    {
        // texture layout

        // empty descriptor
        {
            vk::DescriptorSetLayoutCreateInfo empty_info{};
            vertex_textures_layout[0] = state.device.createDescriptorSetLayout(empty_info);
            fragment_textures_layout[0] = vertex_textures_layout[0];
        }

        // first vertex
        std::array<vk::DescriptorSetLayoutBinding, 16> layout_bindings;
        for (uint32_t i = 0; i < 16; i++) {
            layout_bindings[i] = {
                .binding = i,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eVertex
            };
        }
        for (uint32_t i = 1; i <= 16; i++) {
            vk::DescriptorSetLayoutCreateInfo descriptor_info{
                .bindingCount = i,
                .pBindings = layout_bindings.data()
            };
            vertex_textures_layout[i] = state.device.createDescriptorSetLayout(descriptor_info);
        }

        // then fragment
        for (uint32_t i = 0; i < 16; i++) {
            layout_bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
        }
        for (uint32_t i = 1; i <= 16; i++) {
            vk::DescriptorSetLayoutCreateInfo descriptor_info{
                .bindingCount = i,
                .pBindings = layout_bindings.data()
            };
            fragment_textures_layout[i] = state.device.createDescriptorSetLayout(descriptor_info);
        }
    }

    // compute all possible pipeline layouts
    for (uint32_t vert_texture_count = 0; vert_texture_count <= 16; vert_texture_count++) {
        for (uint32_t frag_texture_count = 0; frag_texture_count <= 16; frag_texture_count++) {
            vk::PipelineLayoutCreateInfo layout_info{};
            vk::DescriptorSetLayout set_layouts[] = { uniforms_layout, attachments_layout, vertex_textures_layout[vert_texture_count], fragment_textures_layout[frag_texture_count] };
            layout_info.setSetLayouts(set_layouts);
            pipeline_layouts[vert_texture_count][frag_texture_count] = state.device.createPipelineLayout(layout_info);
        }
    }

    {
        // look for rgb vertex attribute support
        // we need to look at each format because it is not the same for all usual 3-component formats (checked on AMD Radeon HD 7800)
        // no need to test for 32-bit types, they are always supported
        vk::Format formats[] = {
            vk::Format::eR16G16B16Unorm, vk::Format::eR16G16B16Snorm,
            vk::Format::eR16G16B16Uscaled, vk::Format::eR16G16B16Sscaled,
            vk::Format::eR16G16B16Uint, vk::Format::eR16G16B16Sint,
            vk::Format::eR16G16B16Sfloat,
            vk::Format::eR8G8B8Unorm, vk::Format::eR8G8B8Snorm,
            vk::Format::eR8G8B8Uscaled, vk::Format::eR8G8B8Sscaled,
            vk::Format::eR8G8B8Uint, vk::Format::eR8G8B8Sint
        };
        for (auto fmt : formats) {
            vk::FormatProperties rgb_property = state.physical_device.getFormatProperties(fmt);
            if (!(rgb_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer)) {
                unsupported_rgb_vertex_attribute_formats.emplace(fmt);
            }
        }

        // same for scaled formats
        const vk::FormatProperties scaled_property = state.physical_device.getFormatProperties(vk::Format::eR8G8B8A8Uscaled);
        support_scaled_vertex_attribute = static_cast<bool>(scaled_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer);
        state.features.support_scaled_attribute_formats = support_scaled_vertex_attribute;

        // handle interactions between these 2 properties, the scaled support will be applied before the 3-component support
        if (!support_scaled_vertex_attribute) {
            vk::Format scaled_fmt[] = { vk::Format::eR16G16B16Uscaled, vk::Format::eR16G16B16Sscaled, vk::Format::eR8G8B8Uscaled, vk::Format::eR8G8B8Sscaled };
            for (auto fmt : scaled_fmt)
                unsupported_rgb_vertex_attribute_formats.erase(fmt);
        }

        state.features.support_rgb_attributes = unsupported_rgb_vertex_attribute_formats.empty();
    }

    support_coherent_framebuffer_fetch = support_rasterized_order_access;

    const int nb_logical_threads = SDL_GetNumLogicalCPUCores();
#ifdef __SWITCH__
    // The table below is tuned for desktop core counts and yields exactly one
    // worker below six logical cores, which is every Switch. Horizon gives
    // homebrew three cores that guest threads already share, so a second worker
    // is what fits: it halves the cold-shader backlog without taking a core away
    // from emulation for the whole session.
    // Three app cores plus the helper core's slack, and these workers sit
    // below the guest band, so a third one consumes leftover CPU rather than
    // competing for what emulation needs.
    nb_worker_threads = 3;
    (void)nb_logical_threads;
#else
    // took this from RPCS3 (slightly modified)
    if (nb_logical_threads > 12)
        nb_worker_threads = 6;
    else if (nb_logical_threads > 8)
        nb_worker_threads = 4;
    else if (nb_logical_threads >= 8)
        nb_worker_threads = 3;
    else if (nb_logical_threads >= 6)
        nb_worker_threads = 2;
    else
        nb_worker_threads = 1;
#endif

    if (use_async_compilation) {
        // we could not initialize the worker threads previously
        use_async_compilation = false;
        set_async_compilation(true);
    }
}

void PipelineCache::set_async_compilation(bool enable) {
    if (enable == use_async_compilation)
        return;

    use_async_compilation = enable;
    if (nb_worker_threads == 0)
        // not ingame yet
        return;

    if (enable) {
        LOG_INFO("Enabling asynchronous pipeline compilation with {} threads", nb_worker_threads);
        worker_threads.reserve(nb_worker_threads);
        for (int i = 0; i < nb_worker_threads; i++) {
            worker_threads.emplace_back(&PipelineCache::compiler_thread, this, std::ref(*state.mem));
        }
    } else {
        LOG_INFO("Asynchronous pipeline compilation is now disabled");

        for (size_t i = 0; i < worker_threads.size(); i++) {
            // Stop after the requests from this producer have been consumed.
            while (!pipeline_compile_queue.enqueue(pipeline_compile_queue_token, nullptr))
                std::this_thread::yield();
        }

        for (auto &thread : worker_threads) {
            if (thread.joinable())
                thread.join();
        }
        worker_threads.clear();
    }
}

// magic number put at the beginning of the pipeline cache file
constexpr uint32_t pipeline_cache_magic = 0xBEEF4321;

void PipelineCache::read_pipeline_cache() {
    const std::string pipeline_cache_name = fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION);
    const fs::path path = state.shaders_path / pipeline_cache_name;

    std::ifstream pipeline_cache_file(path, std::ios::in | std::ios::binary);
    if (!pipeline_cache_file.is_open())
        return;

    LOG_INFO("Found pipeline cache, reading...");

    pipeline_cache_file.seekg(0, std::ifstream::end);
    const auto file_size = pipeline_cache_file.tellg();
    if (file_size < 0)
        return;
    size_t pipeline_size = static_cast<size_t>(file_size);
    pipeline_cache_file.seekg(0);

    if (pipeline_size < sizeof(uint32_t) + sizeof(size_t))
        return;

    // read the hashes
    auto read_integer = [&]<typename T>(T &val) {
        pipeline_cache_file.read(reinterpret_cast<char *>(&val), sizeof(T));
    };
    uint32_t magic_number = 0;
    read_integer(magic_number);
    size_t nb_hashes = 0;
    read_integer(nb_hashes);
    // safety check
    const size_t header_size = sizeof(magic_number) + sizeof(nb_hashes);
    if (!pipeline_cache_file || magic_number != pipeline_cache_magic || nb_hashes > (pipeline_size - header_size) / sizeof(uint64_t)) {
        LOG_WARN("Pipeline cache is corrupted, ignoring it.");
        pipeline_cache_file.close();
        return;
    }
    pipeline_size -= header_size + nb_hashes * sizeof(uint64_t);

    // insert hashes with null pipeline
    for (size_t i = 0; i < nb_hashes; i++) {
        uint64_t hash;
        read_integer(hash);
        pipelines.try_emplace(hash);
    }

    std::vector<char> pipeline_data(pipeline_size);
    if (!pipeline_cache_file.read(pipeline_data.data(), pipeline_size))
        return;
    pipeline_cache_file.close();

    vk::PipelineCacheCreateInfo cache_info{
        .initialDataSize = pipeline_size,
        .pInitialData = pipeline_data.data()
    };

    state.device.destroyPipelineCache(pipeline_cache);
    pipeline_cache = state.device.createPipelineCache(cache_info);
    LOG_INFO("Pipeline cache read and loaded");
}

void PipelineCache::save_pipeline_cache() {
    try {
        std::vector<ShadersHash> hashes;
        {
            std::lock_guard lock(shaders_mutex);
            hashes = state.shaders_cache_hashs;
        }
        renderer::save_shaders_cache_hashs(state, hashes);

        const auto data = state.device.getPipelineCacheData(pipeline_cache);
        if (!data.empty()) {
            const auto path = state.shaders_path / fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION);
            std::vector<uint8_t> header(sizeof(pipeline_cache_magic) + sizeof(size_t) + pipelines.size() * sizeof(uint64_t));
            auto cursor = header.data();
            const auto append = [&]<typename T>(T value) {
                std::memcpy(cursor, &value, sizeof(value));
                cursor += sizeof(value);
            };
            append(pipeline_cache_magic);
            append(pipelines.size());
            for (const auto &[hash, _] : pipelines)
                append(hash);

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(header.data()), header.size());
            file.write(reinterpret_cast<const char *>(data.data()), data.size());
            file.close();
            if (file.fail())
                LOG_WARN("Failed to write pipeline cache {}", path);
            else
                LOG_INFO("Pipeline cache saved");
        }
        switch_commit_storage();
    } catch (const std::exception &error) {
        LOG_WARN("Failed to save pipeline cache: {}", error.what());
    }
}

void PipelineCache::cleanup() {
    // stop threads
    if (use_async_compilation)
        set_async_compilation(false);
#ifdef __SWITCH__
    if (pipeline_cache && !state.shaders_path.empty())
        save_pipeline_cache();
#endif

    for (auto &[hash, pipeline] : pipelines)
        state.device.destroy(pipeline.pipeline.load(std::memory_order_relaxed));
    pipelines.clear();

    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        for (auto &[hash, shader] : shaders)
            state.device.destroy(shader.module);
        shaders.clear();
        for (auto &[hash, shader] : masked_fetch_shaders)
            state.device.destroy(shader.module);
        masked_fetch_shaders.clear();
        state.device.destroy(mask_test_shader);
        mask_test_shader = nullptr;
    }

    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++)
                for (int l = 0; l < 2; l++) {
                    for (auto &[fmt, pass] : render_passes[i][j][k][l])
                        state.device.destroy(pass);
                    render_passes[i][j][k][l].clear();
                }

    for (auto &depth_load : render_passes_with_raw)
        for (auto &stencil_load : depth_load)
            for (auto &store : stencil_load) {
                for (auto &[format, pass] : store)
                    state.device.destroy(pass);
                store.clear();
            }
    raw_attachment_passes.clear();

    for (auto &[fmt, pass] : shader_interlock_pass)
        state.device.destroy(pass);
    shader_interlock_pass.clear();
    state.device.destroy(mask_render_pass);
    mask_render_pass = nullptr;

    for (int i = 0; i < 17; i++)
        for (int j = 0; j < 17; j++) {
            state.device.destroy(pipeline_layouts[i][j]);
            pipeline_layouts[i][j] = nullptr;
        }

    state.device.destroy(uniforms_layout);
    uniforms_layout = nullptr;
    state.device.destroy(attachments_layout);
    attachments_layout = nullptr;

    state.device.destroy(vertex_textures_layout[0]);
    vertex_textures_layout[0] = nullptr;
    fragment_textures_layout[0] = nullptr;

    for (int i = 1; i <= 16; i++) {
        state.device.destroy(vertex_textures_layout[i]);
        vertex_textures_layout[i] = nullptr;
        state.device.destroy(fragment_textures_layout[i]);
        fragment_textures_layout[i] = nullptr;
    }

    state.device.destroy(pipeline_cache);
    pipeline_cache = nullptr;

    next_pipeline_cache_save = std::numeric_limits<uint64_t>::max();
    nb_worker_threads = 0;
}

// Vulkan structs used to specify the fragment specialization constants
// Also, booleans in SPIRV are 32bit wide
static const vk::SpecializationMapEntry frag_spec_entries[] = {
    { .constantID = shader::GAMMA_CORRECTION_SPECIALIZATION_ID, .offset = 0, .size = sizeof(uint32_t) },
    { .constantID = shader::SURFACE_CAST_SPECIALIZATION_ID, .offset = sizeof(uint32_t), .size = sizeof(uint32_t) },
};

// indexed by (is_srgb << 1) | has_casts
static const uint32_t frag_spec_data[4][2] = {
    { vk::False, vk::False },
    { vk::False, vk::True },
    { vk::True, vk::False },
    { vk::True, vk::True },
};

static const vk::SpecializationInfo frag_spec_infos[4] = {
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[0]), .pData = frag_spec_data[0] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[1]), .pData = frag_spec_data[1] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[2]), .pData = frag_spec_data[2] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[3]), .pData = frag_spec_data[3] },
};

vk::ShaderModule PipelineCache::load_shader_module(const Sha256Hash &hash, const SceGxmProgram *program, const shader::Hints *hints, bool maskupdate) {
    const bool masked_fetch = program && program->get_type() == SceGxmProgramType::Fragment
        && hints && hints->masked_framebuffer_fetch;
    ShaderEntry *entry;
    {
        std::unique_lock lock(shaders_mutex);
        entry = &(masked_fetch ? masked_fetch_shaders : shaders).try_emplace(hash).first->second;
        shaders_ready.wait(lock, [&] { return !entry->compiling; });
        if (entry->error)
            std::rethrow_exception(entry->error);
        if (entry->module)
            return entry->module;
        entry->compiling = true;
    }

    vk::ShaderModule module;
    try {
        const std::string shader_version = fmt::format("vk{}-fp{}", shader::CURRENT_VERSION, state.features.force_full_precision ? 1 : 0);
        const auto shader_path = state.shaders_path / fmt::format("{}-{}.spv", shader_version, hex_string(hash));
        bool listed;
        {
            std::lock_guard lock(shaders_mutex);
            listed = std::ranges::any_of(state.shaders_cache_hashs, [&](const ShadersHash &cached) { return cached.frag == hash || cached.vert == hash; });
        }
        auto source = !masked_fetch && listed ? renderer::pre_load_shader_spirv(shader_path) : shader::usse::SpirvCode{};
        const bool needs_translation = source.empty() && program;
        if (needs_translation) {
            auto features = state.features;
            if (masked_fetch)
                features.direct_fragcolor = true;
            source = load_spirv_shader(*program, hash, features, true, *hints, maskupdate, state.shaders_path, state.shaders_log_path,
                masked_fetch ? shader_version + "-masked-fetch1" : shader_version, !masked_fetch && listed, state.shader_debug_dump, false);
        }

        if (!source.empty()) {
            const vk::ShaderModuleCreateInfo shader_info{
                .codeSize = sizeof(uint32_t) * source.size(),
                .pCode = source.data()
            };
            module = state.device.createShaderModule(shader_info);
        }
        if (program && !module)
            throw std::runtime_error("Shader translation produced no module");

        {
            std::lock_guard lock(shaders_mutex);
            if (!masked_fetch && program && module && std::none_of(state.shaders_cache_hashs.begin(), state.shaders_cache_hashs.end(), [&](const ShadersHash &cached) {
                    return cached.frag == hash || cached.vert == hash;
                })) {
                const Sha256Hash empty_hash{};
                if (program->get_type() == SceGxmProgramType::Vertex)
                    state.shaders_cache_hashs.push_back({ hash, empty_hash });
                else
                    state.shaders_cache_hashs.push_back({ empty_hash, hash });
            }
            entry->module = module;
            entry->compiling = false;
        }
    } catch (...) {
        state.device.destroy(module);
        {
            std::lock_guard lock(shaders_mutex);
            entry->error = std::current_exception();
            entry->compiling = false;
        }
        shaders_ready.notify_all();
        throw;
    }
    shaders_ready.notify_all();
    return module;
}

vk::PipelineShaderStageCreateInfo PipelineCache::retrieve_shader(const SceGxmProgram *program, const Sha256Hash &hash, bool is_vertex, bool maskupdate, MemState &mem, const shader::Hints &hints, bool is_srgb, bool has_casts) {

    // Entries for constants a module does not declare are ignored.
    const vk::SpecializationInfo *spec_info = is_vertex ? nullptr : &frag_spec_infos[(is_srgb ? 2 : 0) | (has_casts ? 1 : 0)];
    return vk::PipelineShaderStageCreateInfo{
        .stage = is_vertex ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment,
        .module = load_shader_module(hash, program, &hints, maskupdate),
        .pName = is_vertex ? "main_vs" : "main_fs",
        .pSpecializationInfo = spec_info,
    };
}

vk::RenderPass PipelineCache::retrieve_mask_render_pass() {
    if (mask_render_pass)
        return mask_render_pass;

    const vk::AttachmentDescription attachment{
        .format = vk::Format::eR8G8B8A8Unorm,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::eGeneral
    };
    const vk::AttachmentReference color_ref{ 0, vk::ImageLayout::eGeneral };
    vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics
    };
    subpass.setColorAttachments(color_ref);

    const std::array<vk::SubpassDependency, 2> dependencies{ { { .srcSubpass = VK_SUBPASS_EXTERNAL,
                                                                   .dstSubpass = 0,
                                                                   .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                                                   .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                                                   .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite,
                                                                   .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite },
        { .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
            .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite } } };
    vk::RenderPassCreateInfo pass_info;
    pass_info.setAttachments(attachment);
    pass_info.setSubpasses(subpass);
    pass_info.setDependencies(dependencies);
    mask_render_pass = state.device.createRenderPass(pass_info);
    return mask_render_pass;
}

vk::RenderPass PipelineCache::retrieve_render_pass(vk::Format format, bool depth_load, bool stencil_load, bool force_store, bool is_color_transient, bool no_color, bool has_raw_attachment) {
    const bool with_raw = has_raw_attachment && !no_color && !is_color_transient
        && state.features.preserve_f16_nan_as_u16 && format == vk::Format::eR16G16B16A16Sfloat;

    auto &render_passes_map = no_color
        ? shader_interlock_pass
        : (with_raw ? render_passes_with_raw[depth_load][stencil_load][force_store]
                    : render_passes[is_color_transient][depth_load][stencil_load][force_store]);

    auto it = render_passes_map.find(format);

    if (it != render_passes_map.end())
        return it->second;

    // create a new render pass for this format

    vk::AttachmentReference color_refs[2] = {
        { .attachment = 0, .layout = vk::ImageLayout::eGeneral },
        { .attachment = 1, .layout = vk::ImageLayout::eGeneral }
    };
    vk::AttachmentReference ds_ref{
        .attachment = no_color ? 0U : (with_raw ? 2U : 1U),
        .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal
    };
    vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics
    };

    subpass.setPDepthStencilAttachment(&ds_ref);
    if (!no_color) {
        if (support_coherent_framebuffer_fetch)
            subpass.flags = vk::SubpassDescriptionFlagBits::eRasterizationOrderAttachmentColorAccessEXT;

        subpass.colorAttachmentCount = with_raw ? 2 : 1;
        subpass.pColorAttachments = color_refs;
        // Framebuffer fetch reads only the float attachment.
        subpass.inputAttachmentCount = 1;
        subpass.pInputAttachments = color_refs;
    }

    vk::AttachmentDescription color_attachment{
        .format = format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = is_color_transient ? vk::AttachmentLoadOp::eDontCare : vk::AttachmentLoadOp::eLoad,
        .storeOp = is_color_transient ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
        .initialLayout = is_color_transient ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::eGeneral
    };

    const vk::AttachmentDescription raw_attachment{
        .format = vk::Format::eR16G16B16A16Uint,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::eGeneral
    };

    vk::AttachmentStoreOp store_op = force_store ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;
    vk::AttachmentDescription ds_attachment{
        .format = state.deep_stencil_use,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = depth_load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        .storeOp = store_op,
        .stencilLoadOp = stencil_load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        .stencilStoreOp = store_op,
        .initialLayout = (depth_load || stencil_load) ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal
    };

    std::array<vk::SubpassDependency, 5> dependencies;

    // external dependency
    // we want the previous render pass to be done when we reach the fragment stage / stencil*depth testing
    dependencies[0] = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
            | vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
    };

    // if an attachment is sampled from, we want it to be done before the next render pass fragment shader
    dependencies[1] = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits::eShaderRead,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite
    };

    if (state.features.support_shader_interlock) {
        // Interlock writes must precede both attachment access and subsequent interlock draws.
        dependencies[1].srcAccessMask |= vk::AccessFlagBits::eShaderWrite;
        if (no_color) {
            dependencies[0].dstStageMask |= vk::PipelineStageFlagBits::eFragmentShader;
            dependencies[0].dstAccessMask |= vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
            dependencies[1].dstStageMask |= vk::PipelineStageFlagBits::eFragmentShader;
            dependencies[1].dstAccessMask |= vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        } else if (state.features.use_mask_bit) {
            dependencies[0].dstStageMask |= vk::PipelineStageFlagBits::eFragmentShader;
            dependencies[0].dstAccessMask |= vk::AccessFlagBits::eInputAttachmentRead;
            dependencies[1].dstStageMask |= vk::PipelineStageFlagBits::eFragmentShader;
            dependencies[1].dstAccessMask |= vk::AccessFlagBits::eInputAttachmentRead;
        }
    }

    // The final layout transition covers the entire attachment, not just this macroblock.
    dependencies[2] = {
        .srcSubpass = 0,
        .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        .dstStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
    };

    // self-dependency
    // this allows us to use a pipeline barrier in the render pass for programmable blending
    dependencies[3] = {
        .srcSubpass = 0,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead,
        .dependencyFlags = vk::DependencyFlagBits::eByRegion
    };

    // mid-scene flush
    // unity games use it to write to a buffer in a vertex shader then use it as the vertex input in the next draw
    dependencies[4] = {
        .srcSubpass = 0,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eVertexShader,
        .dstStageMask = vk::PipelineStageFlagBits::eVertexInput,
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead
    };

    vk::RenderPassCreateInfo pass_info{};
    vk::AttachmentDescription attachments[] = { color_attachment, with_raw ? raw_attachment : ds_attachment, ds_attachment };
    pass_info.setAttachments(attachments);
    pass_info.attachmentCount = with_raw ? 3 : 2;
    pass_info.setSubpasses(subpass);
    pass_info.setDependencies(dependencies);
    if (no_color) {
        // only add the ds attachment
        pass_info.pAttachments = &attachments[2];
        pass_info.attachmentCount = 1;
        // no need for the self-dependency
        pass_info.setDependencyCount(3);
    }

    render_passes_map[format] = state.device.createRenderPass(pass_info);
    if (with_raw)
        raw_attachment_passes.insert(static_cast<VkRenderPass>(render_passes_map[format]));

    return render_passes_map[format];
}

static std::shared_ptr<const VertexInputLayout> make_vertex_input_layout(const PipelineVertexProgram &vertex_program,
    bool support_scaled_vertex_attribute, const std::set<vk::Format> &unsupported_rgb_vertex_attribute_formats,
    uint32_t max_attribute_offset, bool native_unaligned = false) {
    auto layout = std::make_shared<VertexInputLayout>();
    struct Fetch {
        size_t index;
        uint32_t stream, source, size, source_size, alignment, location, array_size, array_stride;
        vk::Format format;
    };
    std::vector<Fetch> fetches;
    const auto *vkvert = &vertex_program.renderer_data;
    layout->attribute_offsets.resize(vertex_program.attributes.size());
    for (size_t index = 0; index < vertex_program.attributes.size(); ++index) {
        const auto &attribute = vertex_program.attributes[index];
        if (!vkvert->attribute_infos.contains(attribute.regIndex))
            continue;
        if (attribute.streamIndex >= vertex_program.streams.size() || attribute.streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
            throw std::runtime_error("Invalid Vulkan vertex stream index");
        SceGxmAttributeFormat attribute_format = attribute.format;
        shader::usse::AttributeInformation info = vkvert->attribute_infos.at(attribute.regIndex);

        uint8_t component_count = attribute.componentCount;
        uint32_t source_size = component_count * gxm::attribute_format_size(attribute_format);
        // these 2 values are only used when a matrix is used as a vertex attribute
        // this is only supported for regformated attribute for now
        // TODO: add support for matrix input for non-regformated attributes
        uint32_t array_size = 1;
        uint32_t array_element_size = 0;
        vk::Format format;
        if (info.regformat) {
            // use the data from the shader itself
            component_count = info.component_count;
            switch (info.gxm_type) {
            case SCE_GXM_PARAMETER_TYPE_U8:
            case SCE_GXM_PARAMETER_TYPE_S8:
            case SCE_GXM_PARAMETER_TYPE_C10:
                attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
                break;
            case SCE_GXM_PARAMETER_TYPE_U16:
            case SCE_GXM_PARAMETER_TYPE_S16:
            case SCE_GXM_PARAMETER_TYPE_F16:
                attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_U16;
                break;
            default:
                // U32 format
                attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED;
                break;
            }

            if (info.gxm_type == SCE_GXM_PARAMETER_TYPE_C10)
                // this is 10-bit and not 8-bit
                component_count = (component_count * 10 + 7) / 8;

            source_size = component_count * gxm::attribute_format_size(attribute_format);
            if (component_count > 4) {
                // a matrix is used as an attribute, pack everything into an array of vec4
                array_size = (component_count + 3) / 4;
                array_element_size = 4 * gxm::attribute_format_size(attribute_format);
                component_count = 4;
            }

            // regformat attributes are int32
            format = translate_attribute_format(attribute_format, component_count, true, false);
            if (component_count == 3 && unsupported_rgb_vertex_attribute_formats.contains(format)) {
                component_count = 4;
                format = translate_attribute_format(attribute_format, component_count, true, false);
            }
        } else {
            // some Android GPUs do not support scaled attributes, do the conversion in the GPU instead
            if (!support_scaled_vertex_attribute)
                info.is_integer = true;

            // some AMD GPUs do not support rgb vertex attributes, so just put it as rgba
            // the 4th component will contain garbage but this is not an issue because the input
            // in the shader will be vec3 (or ivec3) and the 4th component will be discarded
            format = translate_attribute_format(attribute_format, component_count, info.is_integer, info.is_signed);
            if (component_count == 3 && unsupported_rgb_vertex_attribute_formats.contains(format)) {
                component_count = 4;
                format = translate_attribute_format(attribute_format, component_count, info.is_integer, info.is_signed);
            }
        }


        const uint32_t component_size = gxm::attribute_format_size(attribute_format);
        const uint32_t size = (array_size - 1) * array_element_size + component_count * component_size;
        fetches.push_back({index, attribute.streamIndex, attribute.offset, size, source_size, component_size,
            info.location, array_size, array_element_size, format});
        layout->used_streams |= 1u << attribute.streamIndex;
        layout->stream_count = std::max(layout->stream_count, uint32_t(attribute.streamIndex) + 1);
    }
    for (uint32_t index = 0; index < layout->stream_count; ++index) {
        auto &stream = layout->streams[index];
        if (!(layout->used_streams & (1u << index)))
            continue;
        stream.stride = vertex_program.streams[index].stride;
        for (const auto &fetch : fetches) {
            if (fetch.stream != index)
                continue;
            stream.source_record_size = std::max(stream.source_record_size, fetch.source + fetch.source_size);
            const auto &attribute = vertex_program.attributes[fetch.index];
            stream.extended_source |= fetch.source_size > attribute.componentCount * gxm::attribute_format_size(attribute.format);
            if (!native_unaligned) {
                stream.alignment = std::max(stream.alignment, fetch.alignment);
                stream.repack |= fetch.source % fetch.alignment != 0 || stream.stride % fetch.alignment != 0;
            }
            stream.repack |= fetch.source_size != fetch.size;
        }
#ifdef __APPLE__
        stream.alignment = std::max(stream.alignment, 4u);
        stream.repack |= stream.stride % 4 != 0;
#endif
        for (const auto &fetch : fetches) {
            if (fetch.stream != index)
                continue;
            const uint32_t destination = stream.repack ? align(stream.record_size, fetch.alignment) : fetch.source;
            layout->attribute_offsets[fetch.index] = destination;
            stream.record_size = std::max(stream.record_size, destination + fetch.size);
            if (stream.repack) {
                if (!stream.copies.empty() && stream.copies.back().source + stream.copies.back().size == fetch.source
                    && stream.copies.back().destination + stream.copies.back().size == destination)
                    stream.copies.back().size += fetch.source_size;
                else
                    stream.copies.push_back({fetch.source, destination, fetch.source_size});
            }
        }
        layout->extended_vertex_source |= stream.extended_source && stream.stride
            && !gxm::is_stream_instancing(SceGxmIndexSource(vertex_program.streams[index].indexSource));
        if (stream.repack && stream.stride != 0) {
            stream.stride = align(stream.record_size, stream.alignment);
            layout->repack_vertices |= !gxm::is_stream_instancing(SceGxmIndexSource(vertex_program.streams[index].indexSource));
        }
    }
    for (const auto &fetch : fetches)
        layout->attribute_bindings |= layout->attribute_offsets[fetch.index] + (fetch.array_size - 1) * fetch.array_stride > max_attribute_offset;
    const auto add_binding = [&](uint32_t binding, uint32_t stream_index) {
        const auto &stream = vertex_program.streams[stream_index];
        layout->bindings.push_back({binding, layout->streams[stream_index].stride,
            gxm::is_stream_instancing(SceGxmIndexSource(stream.indexSource)) ? vk::VertexInputRate::eInstance : vk::VertexInputRate::eVertex});
    };
    for (const auto &fetch : fetches) {
        uint32_t binding = fetch.stream;
        if (layout->attribute_bindings) {
            binding = uint32_t(layout->bindings.size());
            add_binding(binding, fetch.stream);
        }
        for (uint32_t i = 0; i < fetch.array_size; ++i)
            layout->attributes.push_back({fetch.location + i, binding, fetch.format,
                (layout->attribute_bindings ? 0 : layout->attribute_offsets[fetch.index]) + i * fetch.array_stride});
    }
    if (!layout->attribute_bindings)
        for (uint32_t index = 0; index < layout->stream_count; ++index)
            if (layout->used_streams & (1u << index))
                add_binding(index, index);
    if (native_unaligned) {
        layout->dynamic_bindings.reserve(layout->bindings.size());
        for (const auto &binding : layout->bindings)
            layout->dynamic_bindings.push_back(vk::VertexInputBindingDescription2EXT{
                .binding = binding.binding, .stride = binding.stride, .inputRate = binding.inputRate, .divisor = 1});
        layout->dynamic_attributes.reserve(layout->attributes.size());
        for (const auto &attribute : layout->attributes)
            layout->dynamic_attributes.push_back(vk::VertexInputAttributeDescription2EXT{
                .location = attribute.location, .binding = attribute.binding, .format = attribute.format, .offset = attribute.offset});
    }
    return layout;
}

const VertexInputLayout &PipelineCache::vertex_input_layout(const SceGxmVertexProgram &vertex_program) const {
    auto &cached = vertex_program.renderer_data->vulkan_vertex_input;
    if (!cached) {
        const PipelineVertexProgram program{nullptr, vertex_program.streams, vertex_program.attributes, *vertex_program.renderer_data};
        cached = make_vertex_input_layout(program, support_scaled_vertex_attribute, unsupported_rgb_vertex_attribute_formats,
            state.physical_device_properties.limits.maxVertexInputAttributeOffset, state.support_native_unaligned_vertex_input);
    }
    return *cached;
}

vk::PipelineVertexInputStateCreateInfo PipelineCache::get_vertex_input_state(const PipelineVertexProgram &vertex_program, MemState &mem) {
    static thread_local std::shared_ptr<const VertexInputLayout> layout;
    layout = vertex_program.renderer_data.vulkan_vertex_input;
    if (!layout)
        layout = make_vertex_input_layout(vertex_program, support_scaled_vertex_attribute, unsupported_rgb_vertex_attribute_formats,
            state.physical_device_properties.limits.maxVertexInputAttributeOffset, state.support_native_unaligned_vertex_input);
    vk::PipelineVertexInputStateCreateInfo input{};
    input.setVertexBindingDescriptions(layout->bindings);
    input.setVertexAttributeDescriptions(layout->attributes);
    return input;
}

void PipelineCache::compiler_thread(MemState &mem) {
    switch_allow_helper_core("shader compiler thread");
    moodycamel::ConsumerToken consumer_token(pipeline_compile_queue);

    CompileRequest *queued_request;
    while (true) {
        pipeline_compile_queue.wait_dequeue(consumer_token, queued_request);
        if (!queued_request)
            break;

        std::unique_ptr<CompileRequest> request(queued_request);
        const PipelineVertexProgram vertex_program = request->get_vertex_program();
        const PipelineFragmentProgram fragment_program = request->get_fragment_program();
        const vk::Pipeline pipeline = compile_pipeline(request->type, request->render_pass, vertex_program, fragment_program, *request->get_record(), request->hints, request->has_casts, request->with_raw_attachment, mem);
        request->pipeline->store(pipeline, std::memory_order_release);

        if (pipeline) {
            const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            next_pipeline_cache_save.store(time_s + pipeline_cache_save_delay, std::memory_order_relaxed);
            state.shaders_count_compiled.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

static vk::StencilOpState convert_op_state(const GxmStencilStateOp &state) {
    return vk::StencilOpState{
        .failOp = translate_stencil_op(state.stencil_fail),
        .passOp = translate_stencil_op(state.depth_pass),
        .depthFailOp = translate_stencil_op(state.depth_fail),
        .compareOp = translate_stencil_func(state.func)
    };
}

vk::Pipeline PipelineCache::compile_pipeline(SceGxmPrimitiveType type, vk::RenderPass render_pass, const PipelineVertexProgram &vertex_program_gxm, const PipelineFragmentProgram &fragment_program_gxm, const GxmRecordState &record, const shader::Hints &hints, bool has_casts, bool with_raw_attachment, MemState &mem, bool cache_only) try {
    const VertexProgram &vertex_program = vertex_program_gxm.renderer_data;
    const SceGxmProgram *gxm_fragment_shader = &fragment_program_gxm.program;
    const VKFragmentProgram &fragment_program = fragment_program_gxm.renderer_data;

    // the vertex input state must be computed before shader are retrieved in case symbols are stripped
    const vk::PipelineVertexInputStateCreateInfo vertex_input = get_vertex_input_state(vertex_program_gxm, mem);

    const bool maskupdate = fragment_program_gxm.is_maskupdate;
    const bool is_fragment_disabled = !maskupdate && (record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED || gxm_fragment_shader->has_no_effect());
    const vk::PipelineShaderStageCreateInfo vertex_shader = retrieve_shader(vertex_program_gxm.program, vertex_program.hash, true, false, mem, hints);
    vk::PipelineShaderStageCreateInfo fragment_shader;
    if (is_fragment_disabled && state.features.use_mask_bit) {
        const std::lock_guard<std::mutex> lock(shaders_mutex);
        if (!mask_test_shader) {
            const auto code = shader::create_mask_test_shader();
            const vk::ShaderModuleCreateInfo shader_info{
                .codeSize = code.size() * sizeof(uint32_t),
                .pCode = code.data()
            };
            mask_test_shader = state.device.createShaderModule(shader_info);
        }
        fragment_shader = {
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = mask_test_shader,
            .pName = "main_fs"
        };
    } else if (!is_fragment_disabled) {
        fragment_shader = retrieve_shader(gxm_fragment_shader, fragment_program.hash, false, maskupdate, mem, hints, record.is_gamma_corrected, has_casts);
    }
    const vk::PipelineShaderStageCreateInfo shader_stages[] = { vertex_shader, fragment_shader };
    // Depth-only draws still need the mask test before depth/stencil updates.
    const uint32_t shader_stage_count = is_fragment_disabled && !state.features.use_mask_bit ? 1U : 2U;

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = translate_primitive(type)
    };

    const bool two_sided = (record.two_sided == SCE_GXM_TWO_SIDED_ENABLED);

    const bool use_shader_interlock = !maskupdate && !is_fragment_disabled && !hints.masked_framebuffer_fetch && state.features.support_shader_interlock && gxm_fragment_shader->is_frag_color_used();

    const vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = state.features.support_clip_distance ? VK_TRUE : VK_FALSE,
        .polygonMode = translate_polygon_mode(record.front_polygon_mode),
        .cullMode = translate_cull_mode(record.cull_mode),
        // front face is always counter clockwise
        .frontFace = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = !maskupdate,
        .lineWidth = 1.0f
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1
    };
    // depth and stencil tests are always enabled on the ps vita as there is almost no cost in doing so
    // on a tiled renderer
    const vk::PipelineDepthStencilStateCreateInfo ds_info{
        .depthTestEnable = !maskupdate,
        .depthWriteEnable = !maskupdate && (record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED),
        .depthCompareOp = translate_depth_func(record.front_depth_func),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = !maskupdate,
        .front = convert_op_state(record.front_stencil_state_op),
        .back = convert_op_state(two_sided ? record.back_stencil_state_op : record.front_stencil_state_op)
    };

    vk::PipelineColorBlendStateCreateInfo color_blending{};
    if (!maskupdate && !is_fragment_disabled && support_coherent_framebuffer_fetch && gxm_fragment_shader->is_frag_color_used())
        color_blending.flags = vk::PipelineColorBlendStateCreateFlagBits::eRasterizationOrderAttachmentAccessEXT;

    const bool frag_has_no_output = static_cast<bool>(gxm_fragment_shader->program_flags & SCE_GXM_PROGRAM_FLAG_OUTPUT_UNDEFINED);
    std::array<vk::PipelineColorBlendAttachmentState, 2> blend_attachments;
    blend_attachments[1] = vk::PipelineColorBlendAttachmentState{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
    };
    if (maskupdate) {
        static const vk::PipelineColorBlendAttachmentState blending{
            .blendEnable = VK_FALSE,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
        };
        color_blending.setAttachments(blending);
    } else if (is_fragment_disabled || frag_has_no_output || use_shader_interlock) {
        // The write mask must be empty as the lack of a fragment shader results in undefined values
        blend_attachments[0] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = VK_FALSE,
            .colorWriteMask = vk::ColorComponentFlags()
        };
        blend_attachments[1].colorWriteMask = vk::ColorComponentFlags();
    } else {
        blend_attachments[0] = fragment_program.blending;
    }
    color_blending.attachmentCount = with_raw_attachment ? 2 : 1;
    color_blending.pAttachments = blend_attachments.data();

    vk::PipelineLayout pipeline_layout = pipeline_layouts[vertex_program.texture_count][fragment_program.texture_count];

    // all of these can be changed at any time using the vita graphics api (like opengl)
    // Because each one can take a lot of different values, it's better to set them as dynamic
    std::array<vk::DynamicState, 8> dynamic_states = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
        vk::DynamicState::eStencilCompareMask,
        vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eDepthBias,
    };
    uint32_t dynamic_count = 6;
    if (state.support_native_unaligned_vertex_input)
        dynamic_states[dynamic_count++] = vk::DynamicState::eVertexInputEXT;
    if (state.physical_device_features.wideLines)
        dynamic_states[dynamic_count++] = vk::DynamicState::eLineWidth;
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    dynamic_info.setDynamicStateCount(dynamic_count).setPDynamicStates(dynamic_states.data());

    // we still need to specify the viewport and scissor count even though they are dynamic
    vk::PipelineViewportStateCreateInfo viewport{
        .viewportCount = 1,
        .scissorCount = 1
    };

    vk::GraphicsPipelineCreateInfo pipeline_info{
        .flags = cache_only ? vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT : vk::PipelineCreateFlags{},
        .stageCount = shader_stage_count,
        .pStages = shader_stages,
        .pVertexInputState = state.support_native_unaligned_vertex_input ? nullptr : &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &ds_info,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0
    };

    const auto result = state.device.createGraphicsPipeline(pipeline_cache, pipeline_info);
    if (cache_only && result.result == vk::Result::ePipelineCompileRequiredEXT)
        return nullptr;
    if (result.result != vk::Result::eSuccess) {
        LOG_ERROR("Failed to create pipeline: {}", vk::to_string(result.result));
        return nullptr;
    }

    return result.value;
} catch (const std::exception &error) {
    LOG_ERROR("Failed to compile pipeline: {}", error.what());
    return nullptr;
}

vk::Pipeline PipelineCache::retrieve_pipeline(VKContext &context, SceGxmPrimitiveType &type, bool consider_for_async, MemState &mem) {
    const GxmRecordState &record = context.record;
    // get the hash of the current context
    uint64_t key = XXH3_64bits(&record, record_pipeline_len);
    if (context.shader_hints.masked_framebuffer_fetch)
        key ^= 0x7e46a312b5c098dfULL;

    // add the hash of the blending
    SceGxmFragmentProgram &fragment_program_gxm = *record.fragment_program.get(mem);
    const VKFragmentProgram &fragment_program = *reinterpret_cast<VKFragmentProgram *>(
        fragment_program_gxm.renderer_data.get());
    key ^= fragment_program.blending_hash;

    // Passes with different attachment counts require different pipelines.
    if (render_pass_has_raw_attachment(context.current_render_pass))
        key ^= 0x9E3779B97F4A7C15ull;

    // the cast paths are specialized out when no sampled unit holds a cast
    const uint32_t cast_units = context.curr_frag_ublock.cast_sampler_bits | context.curr_frag_ublock.cast_phase_bits | context.curr_frag_ublock.raw_cast_bits;
    const bool has_casts = (cast_units & fragment_program.textures_used.to_ulong()) != 0;
    if (has_casts)
        key ^= 0xC2B2AE3D27D4EB4Full;

    // add the hash of the attribute and stream layout
    SceGxmVertexProgram &vertex_program_gxm = *record.vertex_program.get(mem);
    vertex_input_layout(vertex_program_gxm);
    key ^= vertex_program_gxm.key_hash;

    const PipelineVertexProgram vertex_program{
        vertex_program_gxm.program.get(mem), vertex_program_gxm.streams,
        vertex_program_gxm.attributes, *vertex_program_gxm.renderer_data
    };
    const PipelineFragmentProgram fragment_program_view{
        *fragment_program_gxm.program.get(mem), fragment_program_gxm.is_maskupdate, fragment_program
    };

    // and also add the primitive type
    key ^= static_cast<uint64_t>(type);

    auto [it, inserted] = pipelines.try_emplace(key);
    auto &entry = it->second;
    if (entry.requested)
        return entry.pipeline.load(std::memory_order_acquire);
    const bool already_in_cache = !inserted;

    // get the correct renderpass here
    const SceGxmProgram *gxm_fragment_shader = fragment_program_gxm.program.get(mem);
    const bool maskupdate = fragment_program_gxm.is_maskupdate;
    const bool is_fragment_disabled = record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED || gxm_fragment_shader->has_no_effect();
    const bool use_shader_interlock = !maskupdate && !is_fragment_disabled && !context.shader_hints.masked_framebuffer_fetch && state.features.support_shader_interlock && gxm_fragment_shader->is_frag_color_used();
    const vk::RenderPass render_pass = maskupdate ? retrieve_mask_render_pass() : (use_shader_interlock ? context.current_shader_interlock_pass : context.current_render_pass);
    // update the shader hints
    context.shader_hints.color_format = record.color_surface.colorFormat;
    context.shader_hints.output_register_format = fragment_program.output_register_format;
    context.shader_hints.attributes = &vertex_program_gxm.attributes;

    // note: the flag can_use_deferred_compilation is not considered here because it causes way too many false positives
    const bool compile_pipeline_async = !maskupdate && !context.shader_hints.masked_framebuffer_fetch && consider_for_async && use_async_compilation;
    const bool with_raw_attachment = render_pass_has_raw_attachment(render_pass);

    if (compile_pipeline_async && already_in_cache && state.support_pipeline_creation_cache_control) {
        bool modules_ready;
        {
            std::lock_guard lock(shaders_mutex);
            const auto vertex = shaders.find(vertex_program.renderer_data.hash);
            const auto fragment = shaders.find(fragment_program.hash);
            modules_ready = vertex != shaders.end() && vertex->second.module
                && fragment != shaders.end() && fragment->second.module;
        }
        // A saved hash alone does not guarantee the driver can skip compilation.
        if (modules_ready) {
            const auto pipeline = compile_pipeline(type, render_pass, vertex_program, fragment_program_view, record, context.shader_hints, has_casts, with_raw_attachment, mem, true);
            if (pipeline) {
                entry.pipeline.store(pipeline, std::memory_order_release);
                entry.requested = true;
                return pipeline;
            }
        }
    }

    if (compile_pipeline_async) {
        auto request = std::make_unique<CompileRequest>();
        request->pipeline = &entry.pipeline;
        request->type = type;
        request->render_pass = render_pass;
        request->vertex_program = copy_program(*vertex_program.program);
        request->vertex_streams = vertex_program.streams;
        request->vertex_attributes = vertex_program.attributes;
        request->vertex_renderer_data = vertex_program.renderer_data;
        request->fragment_program = copy_program(fragment_program_view.program);
        request->fragment_is_maskupdate = fragment_program_view.is_maskupdate;
        request->fragment_renderer_data = fragment_program_view.renderer_data;
        request->hints = context.shader_hints;
        request->hints.attributes = &request->vertex_attributes;
        request->has_casts = has_casts;
        request->with_raw_attachment = with_raw_attachment;
        memcpy(request->record_data, &record, record_pipeline_len);
        if (!pipeline_compile_queue.enqueue(pipeline_compile_queue_token, request.get())) {
            LOG_ERROR_ONCE("Unable to queue pipeline compilation");
            return nullptr;
        }
        request.release();
        entry.requested = true;

        return nullptr;
    } else {
        // can't wait, compile it right now
        vk::Pipeline result = compile_pipeline(type, render_pass, vertex_program, fragment_program_view, record, context.shader_hints, has_casts, with_raw_attachment, mem);

        if (result) {
            const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            next_pipeline_cache_save.store(time_s + pipeline_cache_save_delay, std::memory_order_relaxed);
            if (!already_in_cache)
                state.shaders_count_compiled.fetch_add(1, std::memory_order_relaxed);
        }

        entry.pipeline.store(result, std::memory_order_release);
        entry.requested = true;

        return result;
    }
}

vk::ShaderModule PipelineCache::precompile_shader(const Sha256Hash &hash) {
    return load_shader_module(hash);
}
} // namespace renderer::vulkan
