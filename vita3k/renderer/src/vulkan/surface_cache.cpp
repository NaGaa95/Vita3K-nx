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

#include <renderer/vulkan/surface_cache.h>

#include <algorithm>
#include <cmath>

#include <renderer/texture/packed.h>
#include <renderer/texture/readback.h>
#include <mem/functions.h>

#include <gxm/functions.h>
#include <renderer/vulkan/gxm_to_vulkan.h>
#include <renderer/vulkan/state.h>
#include <renderer/vulkan/types.h>
#include <vkutil/vkutil.h>

#include <vulkan/vulkan_format_traits.hpp>

#include <util/align.h>
#include <util/log.h>
#include <util/vector_utils.h>

extern "C" {
#include <libswscale/swscale.h>
}

static bool format_support_swizzle(SceGxmColorBaseFormat format) {
    // do we support something more than the identity swizzle
    // for now we do not support any texture whose component size
    // are all not the same or not a multiple of a byte
    return format != SCE_GXM_COLOR_BASE_FORMAT_U2F10F10F10
        && format != SCE_GXM_COLOR_BASE_FORMAT_U2U10U10U10
        && format != SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4
        && format != SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5
        && format != SCE_GXM_COLOR_BASE_FORMAT_SE5M9M9M9
        && format != SCE_GXM_COLOR_BASE_FORMAT_F11F11F10
        && format != SCE_GXM_COLOR_BASE_FORMAT_U5U6U5;
}

static bool format_need_additional_memory(SceGxmColorBaseFormat format) {
    // we are using 4-component surfaces to emulate them
    // so we can't simply use the allocated memory for them
    return format == SCE_GXM_COLOR_BASE_FORMAT_U8U8U8;
}

namespace renderer::vulkan {

static bool surface_is_repacked_u4u4u4u4(const ColorSurfaceCacheInfo &surface) {
    return surface.format == SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4
        && (surface.texture.format == vk::Format::eR8G8B8A8Unorm
            || surface.texture.format == vk::Format::eR8G8B8A8Srgb);
}

static bool surface_is_repacked_float(const ColorSurfaceCacheInfo &surface) {
    return surface.texture.format == vk::Format::eR16G16B16A16Sfloat
        && (surface.format == SCE_GXM_COLOR_BASE_FORMAT_U2F10F10F10
            || surface.format == SCE_GXM_COLOR_BASE_FORMAT_SE5M9M9M9);
}

static void pack_float_surface(uint8_t *dst, const uint8_t *src, const ColorSurfaceCacheInfo &surface) {
    using renderer::texture::PackedColorOrder;
    PackedColorOrder order = PackedColorOrder::ABGR;
    switch (surface.swizzle.r) {
    case vk::ComponentSwizzle::eB: order = PackedColorOrder::ARGB; break;
    case vk::ComponentSwizzle::eA: order = PackedColorOrder::RGBA; break;
    case vk::ComponentSwizzle::eG: order = PackedColorOrder::BGRA; break;
    default: break;
    }
    const uint32_t pixel_stride = surface.stride_bytes / sizeof(uint32_t);
    for (uint32_t y = 0; y < surface.original_height; ++y) {
        for (uint32_t x = 0; x < surface.original_width; ++x) {
            std::array<uint16_t, 4> rgba;
            memcpy(rgba.data(), src + (size_t(y) * pixel_stride + x) * sizeof(rgba), sizeof(rgba));
            const uint32_t packed = surface.format == SCE_GXM_COLOR_BASE_FORMAT_SE5M9M9M9
                ? renderer::texture::pack_se5m9m9m9(rgba, surface.swizzle.r == vk::ComponentSwizzle::eB)
                : renderer::texture::pack_u2f10f10f10(rgba, order);
            memcpy(dst + size_t(y) * surface.stride_bytes + x * sizeof(packed), &packed, sizeof(packed));
        }
    }
}

static uint8_t unorm8_to_unorm4(uint8_t value) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * 15 + 127) / 255);
}

// No render-pass dependency covers these transfer reads.
// Shader writes also cover the interlock path.
static void barrier_render_to_transfer_read(vk::CommandBuffer cmd_buffer, vk::Image image) {
    const vk::ImageMemoryBarrier barrier{
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = vkutil::color_subresource_range
    };
    cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags(), {}, {}, barrier);
}

// R4G4B4A4_UNORM_PACK16 stores R in bits 12..15 and A in bits 0..3.
static uint32_t r4g4b4a4_shift(vk::ComponentSwizzle component) {
    switch (component) {
    case vk::ComponentSwizzle::eR:
        return 12;
    case vk::ComponentSwizzle::eG:
        return 8;
    case vk::ComponentSwizzle::eB:
        return 4;
    default:
        return 0;
    }
}

static void pack_rgba8_to_r4g4b4a4(uint8_t *dst, const uint8_t *src, uint32_t pixel_stride, uint32_t width, uint32_t height, const vk::ComponentMapping &swizzle) {
    const uint32_t shift_r = r4g4b4a4_shift(swizzle.r);
    const uint32_t shift_g = r4g4b4a4_shift(swizzle.g);
    const uint32_t shift_b = r4g4b4a4_shift(swizzle.b);
    const uint32_t shift_a = r4g4b4a4_shift(swizzle.a);

    for (uint32_t y = 0; y < height; y++) {
        uint16_t *dst_row = reinterpret_cast<uint16_t *>(dst + y * pixel_stride * sizeof(uint16_t));
        const uint8_t *src_row = src + y * pixel_stride * 4;

        for (uint32_t x = 0; x < width; x++) {
            const uint32_t r = unorm8_to_unorm4(src_row[x * 4 + 0]);
            const uint32_t g = unorm8_to_unorm4(src_row[x * 4 + 1]);
            const uint32_t b = unorm8_to_unorm4(src_row[x * 4 + 2]);
            const uint32_t a = unorm8_to_unorm4(src_row[x * 4 + 3]);

            dst_row[x] = static_cast<uint16_t>((r << shift_r) | (g << shift_g) | (b << shift_b) | (a << shift_a));
        }
    }
}

static void protect_surface(MemState &mem, ColorSurfaceCacheInfo &info) {
    const bool trap_reads = info.tiling == SurfaceTiling::Linear;

    uint32_t addr_start = align(info.data.address(), KiB(4));
    uint32_t addr_end = align_down(info.data.address() + info.total_bytes, KiB(4));
    bool small_surface = addr_start >= addr_end;
    if (small_surface) {
        // we still need to protect something, even if it's not completely accurate
        addr_start = align_down(info.data.address(), KiB(4));
        addr_end = align(info.data.address() + info.total_bytes, KiB(4));
    }

    // Use MemPerm::None to trap both reads and writes for surfaces that support sync,
    // MemPerm::ReadOnly to trap only writes for other surfaces
    MemPerm perm = trap_reads ? MemPerm::None : MemPerm::ReadOnly;
    std::shared_ptr<bool> need_sync = trap_reads ? info.need_surface_sync : nullptr;
    // Don't track dirty for small surfaces to avoid false positives from unrelated writes
    std::shared_ptr<bool> dirty = small_surface ? nullptr : info.dirty;

    add_protect(mem, addr_start, addr_end - addr_start, perm,
        [dirty, need_sync](Address, bool write) {
            if (write && dirty)
                *dirty = true;
            if (need_sync)
                *need_sync = true;
            return true;
        });
}

ColorSurfaceCacheInfo::~ColorSurfaceCacheInfo() {
    sws_freeContext(sws_context);
}

void VKSurfaceCache::destroy_framebuffers(vk::ImageView view) {
    vkutil::DestroyQueue &destroy_queue = state.frame().destroy_queue;
    for (auto it = framebuffer_array.begin(); it != framebuffer_array.end();) {
        // if the color of depth-stencil match the one of the render_target, this won't be used anymore
        if (it->first.first == view || it->first.second == view) {
            destroy_queue.add(it->second.standard);
            destroy_queue.add(it->second.shader_interlock);
            it = framebuffer_array.erase(it);
        } else {
            it = std::next(it);
        }
    }
}

static bool typeless_read_from_raw(const ColorSurfaceCacheInfo &info) {
    return info.raw_image && info.raw_image_filled && !info.content_is_blended;
}

static vk::Image typeless_read_source(const ColorSurfaceCacheInfo &info) {
    return typeless_read_from_raw(info) ? info.raw_image->image : info.texture.image;
}

void VKSurfaceCache::destroy_surface(ColorSurfaceCacheInfo &info) {
    vkutil::DestroyQueue &destroy_queue = state.frame().destroy_queue;

    // don't forget to destroy in the right order
    for (auto &casted : info.casted_textures) {
        destroy_queue.add(casted.reinterpret_view);
        destroy_queue.add(casted.alt_gamma_view);
        destroy_queue.add_buffer(casted.transition_buffer);
        destroy_queue.add_image(casted.raw_copy_image);
        destroy_queue.add_image(casted.texture);
    }
    info.casted_textures.clear();

    destroy_queue.add(info.alternate_view);
    destroy_queue.add(info.storage_view);
    destroy_queue.add(info.reinterpret_store_view);

    if (info.raw_image) {
        destroy_queue.add_image(*info.raw_image);
        info.raw_image.reset();
    }

    destroy_framebuffers(info.texture.view);
    destroy_queue.add_image(info.texture);

    // Reallocate for the new surface size while keeping queued readback mappings alive.
    if (info.copy_buffer)
        destroy_queue.add_buffer(*info.copy_buffer);
}

void VKSurfaceCache::create_raw_alias(ColorSurfaceCacheInfo &info) {
    VKContext *context = reinterpret_cast<VKContext *>(state.context);
    vk::CommandBuffer cmd_buffer = context->prerender_cmd;

    info.raw_image = std::make_unique<vkutil::Image>(info.width, info.height, vk::Format::eR16G16B16A16Uint);
    vkutil::Image &raw = *info.raw_image;
    raw.layout = vkutil::ImageLayout::Undefined;
    raw.init_image(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
            | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        vkutil::default_comp_mapping, vk::ImageCreateFlagBits::eMutableFormat);
    raw.transition_to(cmd_buffer, vkutil::ImageLayout::TransferDst);
    vk::ClearColorValue clear_zero{};
    clear_zero.setUint32({ 0, 0, 0, 0 });
    cmd_buffer.clearColorImage(raw.image, vk::ImageLayout::eTransferDstOptimal, clear_zero, vkutil::color_subresource_range);
    raw.transition_to(cmd_buffer, vkutil::ImageLayout::ColorAttachmentReadWrite);
    info.raw_image_filled = false;

    // Rebuild framebuffers to include the new attachment.
    destroy_framebuffers(info.texture.view);
}

void VKSurfaceCache::destroy_surface(DepthStencilSurfaceCacheInfo &info) {
    vkutil::DestroyQueue &destroy_queue = state.frame().destroy_queue;

    if (pending_ds_scene == &info)
        pending_ds_scene = nullptr;

    for (auto &read_only : info.read_surfaces) {
        destroy_queue.add_image(read_only.stencil_view);
        destroy_queue.add_image(read_only.depth_view);
    }
    info.read_surfaces.clear();

    destroy_queue.add(info.depth_view);
    destroy_queue.add(info.stencil_view);

    destroy_framebuffers(info.texture.view);
    destroy_queue.add_image(info.texture);

    if (info.sample_rate_copy) {
        destroy_framebuffers(info.sample_rate_copy->view);
        destroy_queue.add_image(*info.sample_rate_copy);
        info.sample_rate_copy.reset();
    }
}

void VKSurfaceCache::ensure_reinterpret_pipeline() {
    if (reinterpret_pipeline)
        return;

    const fs::path shader_path = state.static_assets / "shaders-builtin/vulkan" / "surface_cast_reinterpret.comp.spv";
    reinterpret_shader = vkutil::load_shader(state.device, shader_path);

    const vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge
    };
    reinterpret_sampler = state.device.createSampler(sampler_info);

    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = vk::DescriptorSetLayoutBinding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    };
    bindings[1] = vk::DescriptorSetLayoutBinding{
        .binding = 1,
        .descriptorType = vk::DescriptorType::eStorageImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    };
    vk::DescriptorSetLayoutCreateInfo layout_info{};
    layout_info.setBindings(bindings);
    reinterpret_desc_layout = state.device.createDescriptorSetLayout(layout_info);

    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(ReinterpretPushConstants)
    };
    vk::PipelineLayoutCreateInfo pl_info{};
    pl_info.setSetLayouts(reinterpret_desc_layout);
    pl_info.setPushConstantRanges(push_range);
    reinterpret_pipeline_layout = state.device.createPipelineLayout(pl_info);

    const vk::PipelineShaderStageCreateInfo stage{
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = reinterpret_shader,
        .pName = "main"
    };
    const vk::ComputePipelineCreateInfo pipeline_info{
        .stage = stage,
        .layout = reinterpret_pipeline_layout
    };
    reinterpret_pipeline = state.device.createComputePipeline(nullptr, pipeline_info).value;

    constexpr uint32_t NB_SETS = 256;
    std::array<vk::DescriptorPoolSize, 2> pool_sizes{
        vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, NB_SETS },
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, NB_SETS }
    };
    vk::DescriptorPoolCreateInfo pool_info{ .maxSets = NB_SETS };
    pool_info.setPoolSizes(pool_sizes);
    reinterpret_desc_pool = state.device.createDescriptorPool(pool_info);

    const std::vector<vk::DescriptorSetLayout> layouts(NB_SETS, reinterpret_desc_layout);
    vk::DescriptorSetAllocateInfo alloc_info{ .descriptorPool = reinterpret_desc_pool };
    alloc_info.setSetLayouts(layouts);
    reinterpret_desc_sets = state.device.allocateDescriptorSets(alloc_info);
    reinterpret_desc_idx = 0;
}

VKSurfaceCache::VKSurfaceCache(VKState &state)
    : state(state) {
    color_surface_queue.init(max_surfaces_allowed);
    ds_surface_queue.init(max_surfaces_allowed);
}

void VKSurfaceCache::cleanup() {
    if (reinterpret_pipeline) {
        state.device.destroy(reinterpret_pipeline);
        state.device.destroy(reinterpret_pipeline_layout);
        state.device.destroy(reinterpret_desc_layout);
        state.device.destroy(reinterpret_desc_pool);
        state.device.destroy(reinterpret_sampler);
        state.device.destroy(reinterpret_shader);
        reinterpret_pipeline = nullptr;
        reinterpret_pipeline_layout = nullptr;
        reinterpret_desc_layout = nullptr;
        reinterpret_desc_pool = nullptr;
        reinterpret_sampler = nullptr;
        reinterpret_shader = nullptr;
        reinterpret_desc_sets.clear();
        reinterpret_desc_idx = 0;
    }

    for (auto &[key, fb] : framebuffer_array) {
        state.device.destroy(fb.standard);
        state.device.destroy(fb.shader_interlock);
    }
    framebuffer_array.clear();

    for (auto &item : color_surface_queue.items) {
        auto &info = item.content;
        for (auto &casted : info.casted_textures) {
            if (casted.reinterpret_view) {
                state.device.destroy(casted.reinterpret_view);
                casted.reinterpret_view = nullptr;
            }
            if (casted.alt_gamma_view) {
                state.device.destroy(casted.alt_gamma_view);
                casted.alt_gamma_view = nullptr;
            }
            casted.transition_buffer.destroy();
            casted.raw_copy_image.destroy();
            casted.texture.destroy();
        }
        info.casted_textures.clear();

        if (info.alternate_view) {
            state.device.destroy(info.alternate_view);
            info.alternate_view = nullptr;
        }

        if (info.storage_view) {
            state.device.destroy(info.storage_view);
            info.storage_view = nullptr;
        }

        if (info.reinterpret_store_view) {
            state.device.destroy(info.reinterpret_store_view);
            info.reinterpret_store_view = nullptr;
        }

        if (info.blit_image)
            info.blit_image->destroy();
        if (info.copy_buffer)
            info.copy_buffer->destroy();
        if (info.raw_image)
            info.raw_image->destroy();

        info.texture.destroy();
    }

    for (auto &item : ds_surface_queue.items) {
        auto &info = item.content;
        for (auto &read_surface : info.read_surfaces) {
            read_surface.depth_view.destroy();
            read_surface.stencil_view.destroy();
        }
        info.read_surfaces.clear();

        if (info.depth_view) {
            state.device.destroy(info.depth_view);
            info.depth_view = nullptr;
        }
        if (info.stencil_view) {
            state.device.destroy(info.stencil_view);
            info.stencil_view = nullptr;
        }

        info.texture.destroy();
    }

    color_address_lookup.clear();
    depth_address_lookup.clear();
    stencil_address_lookup.clear();
    cpu_surfaces_changed.clear();
    target = nullptr;
    last_written_surface = nullptr;
}

void VKSurfaceCache::update_rendered_extent(ColorSurfaceCacheInfo &surface) {
    auto *context = static_cast<VKContext *>(state.context);
    if (!context->render_target || state.res_multiplier <= 0.0f)
        return;

    const uint32_t width = std::min<uint32_t>(surface.original_width,
        static_cast<uint32_t>(std::lround(context->render_target->width / state.res_multiplier)));
    const uint32_t height = std::min<uint32_t>(surface.original_height,
        static_cast<uint32_t>(std::lround(context->render_target->height / state.res_multiplier)));
    surface.rendered_w = std::max<uint16_t>(surface.rendered_w, static_cast<uint16_t>(width));
    surface.rendered_h = std::max<uint16_t>(surface.rendered_h, static_cast<uint16_t>(height));
}

void VKSurfaceCache::note_scene_draw_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (!last_written_surface || x1 <= x0 || y1 <= y0 || state.res_multiplier <= 0.0f)
        return;

    constexpr float tile_size = 32.0f;
    const float scale = state.res_multiplier * tile_size;
    int32_t ux0 = static_cast<int32_t>(std::floor(x0 / scale)) * 32;
    int32_t uy0 = static_cast<int32_t>(std::floor(y0 / scale)) * 32;
    int32_t ux1 = static_cast<int32_t>(std::ceil(x1 / scale)) * 32;
    int32_t uy1 = static_cast<int32_t>(std::ceil(y1 / scale)) * 32;

    const auto width = static_cast<int32_t>(last_written_surface->original_width);
    const auto height = static_cast<int32_t>(last_written_surface->original_height);
    ux0 = std::clamp(ux0, 0, width);
    uy0 = std::clamp(uy0, 0, height);
    ux1 = std::clamp(ux1, 0, width);
    uy1 = std::clamp(uy1, 0, height);
    if (ux1 <= ux0 || uy1 <= uy0)
        return;

    last_written_surface->written_x0 = std::min(last_written_surface->written_x0, ux0);
    last_written_surface->written_y0 = std::min(last_written_surface->written_y0, uy0);
    last_written_surface->written_x1 = std::max(last_written_surface->written_x1, ux1);
    last_written_surface->written_y1 = std::max(last_written_surface->written_y1, uy1);
}

SurfaceRetrieveResult VKSurfaceCache::retrieve_color_surface_for_framebuffer(MemState &mem, SceGxmColorSurface *color) {
    // Create the key to access the cache struct
    const uint32_t address = color->data.address();

    const uint32_t original_width = color->width;
    const uint32_t original_height = color->height;

    uint32_t width = static_cast<uint32_t>(original_width * state.res_multiplier);
    uint32_t height = static_cast<uint32_t>(original_height * state.res_multiplier);

    bool overlap = true;

    // Of course, this works under the assumption that range must be unique :D
    auto ite = color_address_lookup.upper_bound(address);
    if (ite == color_address_lookup.begin())
        // no match
        overlap = false;
    else
        --ite;
    // ite is now the first item with an address lower or equal to key

    overlap = (overlap && (ite->first + ite->second->total_bytes) > address);

    const SceGxmColorBaseFormat base_format = gxm::get_base_format(color->colorFormat);
    vk::Format vk_format = color::translate_surface_format(base_format);

    SurfaceTiling tiling;
    if (color->surfaceType == SCE_GXM_COLOR_SURFACE_LINEAR)
        tiling = SurfaceTiling::Linear;
    else if (color->surfaceType == SCE_GXM_COLOR_SURFACE_SWIZZLED)
        tiling = SurfaceTiling::Swizzled;
    else
        tiling = SurfaceTiling::Tiled;

    const bool is_srgb = color->gamma != 0;
    if (is_srgb) {
        if (vk_format == vk::Format::eR8G8B8A8Unorm) {
            vk_format = vk::Format::eR8G8B8A8Srgb;
        } else {
            LOG_WARN_ONCE("Trying to use gamma correction with non-compatible format {}", vk::to_string(vk_format));
        }
    }

    uint32_t bytes_per_stride = color->strideInPixels * gxm::bits_per_pixel(base_format) / 8;
    uint32_t total_surface_size = bytes_per_stride * original_height;

    VKContext *context = reinterpret_cast<VKContext *>(state.context);

    if (overlap) {
        ColorSurfaceCacheInfo &info = *ite->second;

        // There are four situations I think of:
        // 1. Different base address, lookup for write, in this case, if the cached surface range contains the given address, then
        // probably this cached surface has already been freed GPU-wise. So erase.
        // 2. Same base address, but width and height change to be larger, or format change if write. Remake a new one for both read and write situation.
        // 3. Out of cache range. In write case, create a new one, in read case, lul
        // 4. Read situation with smaller width and height, probably need to extract the needed region out.
        // 5. the surface is a gbuffer and we are currently trying to read the 2nd component, in this case key == ite->first + 4
        const bool addr_in_range_of_cache = ((address + total_surface_size) <= (ite->first + info.total_bytes + 4));
        const bool cache_probably_freed = (ite->first != address) && addr_in_range_of_cache;
        const bool surface_extent_changed = info.height < height || bytes_per_stride != info.stride_bytes || tiling != info.tiling;
        bool surface_stat_changed = false;

        if (ite->first == address)
            surface_stat_changed = surface_extent_changed || info.width < width || base_format != info.format;

        const bool invalidated = cache_probably_freed || surface_stat_changed || !addr_in_range_of_cache;
        if (invalidated) {
            destroy_surface(info);
            color_address_lookup.erase(ite);
            color_surface_queue.set_as_lru(&info);
        } else {
            color_surface_queue.set_as_mru(&info);
            update_rendered_extent(info);

            if (info.data && *info.dirty)
                protect_surface(mem, info);
            *info.dirty = false;

            last_written_surface = &info;

            // if this surface has not been rendered to for the last 60 frames, consider it is not safe not to render all shaders to it
            constexpr uint64_t big_delay_between_frames = 60;
            state.pipeline_cache.can_use_deferred_compilation = context->frame_timestamp - info.last_frame_rendered < big_delay_between_frames;
            info.last_frame_rendered = context->frame_timestamp;

            if (vk_format == info.texture.format) {
                return { info.texture.view, &info.texture, info.raw_image.get(), info.storage_view ? info.storage_view : info.texture.view };
            } else {
                // using both srgb/linear
                if (!info.alternate_view) {
                    vk::ImageViewCreateInfo view_info{
                        .image = info.texture.image,
                        .viewType = vk::ImageViewType::e2D,
                        .format = vk_format,
                        .components = vkutil::default_comp_mapping,
                        .subresourceRange = vkutil::color_subresource_range
                    };
                    info.alternate_view = state.device.createImageView(view_info);
                }

                return { info.alternate_view, &info.texture, info.raw_image.get(), info.storage_view ? info.storage_view : info.texture.view };
            }
        }
    }

    // get the least recently used (probably unused) color surface
    ColorSurfaceCacheInfo &info_added = *color_surface_queue.get_lru();
    if (info_added.texture.image)
        // deferred destruction of the existing surface
        destroy_surface(info_added);
    if (info_added.data)
        color_address_lookup.erase(info_added.data.address());

    color_surface_queue.set_as_mru(&info_added);
    info_added.last_frame_rendered = context->frame_timestamp;

    color_address_lookup[address] = &info_added;

    info_added.width = width;
    info_added.height = height;
    info_added.original_width = original_width;
    info_added.original_height = original_height;
    info_added.rendered_w = 0;
    info_added.rendered_h = 0;
    info_added.written_x0 = INT32_MAX;
    info_added.written_y0 = INT32_MAX;
    info_added.written_x1 = 0;
    info_added.written_y1 = 0;
    update_rendered_extent(info_added);
    info_added.stride_bytes = bytes_per_stride;
    info_added.data = color->data;
    info_added.total_bytes = total_surface_size;
    info_added.format = base_format;
    info_added.tiling = tiling;
    // only remember the swizzle here, it will be useful if we get to present or sample from this image with a different swizzle
    info_added.swizzle = color::translate_swizzle(color->colorFormat);

    vkutil::Image &image = info_added.texture;
    image.width = width;
    image.height = height;
    image.format = vk_format;
    image.layout = vkutil::ImageLayout::Undefined;

    // we might have to create a non-srgb/linear view later if this surface is used for presentation
    const bool need_mutable = (vk_format == vk::Format::eR8G8B8A8Unorm || vk_format == vk::Format::eR8G8B8A8Srgb);
    const bool need_storage_view = state.features.support_shader_interlock && vk_format == vk::Format::eR8G8B8A8Srgb;
    vk::ImageCreateFlags image_create_flags = need_mutable ? vk::ImageCreateFlagBits::eMutableFormat : vk::ImageCreateFlags();
    if (need_storage_view)
        image_create_flags |= vk::ImageCreateFlagBits::eExtendedUsage;
    const void *image_info_pNext = nullptr;
    if (support_image_format_specifier && need_mutable) {
        static const vk::Format view_formats[] = { vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb };
        static const vk::ImageFormatListCreateInfoKHR image_info_formats{
            .viewFormatCount = 2,
            .pViewFormats = view_formats
        };
        image_info_pNext = &image_info_formats;
    }

    vk::ImageUsageFlags surface_usages = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eInputAttachment;
    if (state.features.support_shader_interlock)
        surface_usages |= vk::ImageUsageFlagBits::eStorage;
    image.init_image(surface_usages, vkutil::default_comp_mapping, image_create_flags, image_info_pNext);
    if (need_storage_view) {
        const vk::ImageViewCreateInfo storage_view_info{
            .image = image.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .components = vkutil::default_comp_mapping,
            .subresourceRange = vkutil::color_subresource_range
        };
        info_added.storage_view = state.device.createImageView(storage_view_info);
    }

    // do it in the prerender if we read from this texture in the same scene (although this would be useless)
    vk::CommandBuffer cmd_buffer = context->prerender_cmd;
    // must do a first transition to draw the placeholder color
    image.transition_to(cmd_buffer, vkutil::ImageLayout::TransferDst);

    vk::ClearColorValue clear_color{ std::array<float, 4>({ 0.0f, 0.0f, 0.0f, 0.0f }) };
    cmd_buffer.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, clear_color, vkutil::color_subresource_range);
    image.transition_to(cmd_buffer, vkutil::ImageLayout::ColorAttachmentReadWrite);

    info_added.content_is_blended = false;
    info_added.has_phase_view = false;

    last_written_surface = &info_added;
    info_added.need_surface_sync.reset();
    info_added.need_surface_sync = std::make_shared<bool>(false);
    info_added.dirty = std::make_shared<bool>(false);
    info_added.gpu_read_sync_only = false;
    info_added.gpu_read_needs_barrier = false;

    // we only support surface sync of linear surfaces for now
    if (!can_mprotect_mapped_memory) {
        // perform surface sync on everything
        // it is slow but well... we can't mprotect the buffer
        *info_added.need_surface_sync = color->surfaceType == SCE_GXM_COLOR_SURFACE_LINEAR;
    } else {
        protect_surface(mem, info_added);
    }

    // it's not impossible that this surface will be rendered once and only used after, so do not skip any shader on it
    state.pipeline_cache.can_use_deferred_compilation = false;

    return { info_added.texture.view, &info_added.texture, info_added.raw_image.get(),
        info_added.storage_view ? info_added.storage_view : info_added.texture.view };
}

std::optional<TextureLookupResult> VKSurfaceCache::retrieve_color_surface_as_texture(const SceGxmTexture &texture, const SceGxmColorBaseFormat base_format, TextureViewport *texture_viewport, bool allow_raw_bits) {
    // Create the key to access the cache struct
    const uint32_t address = (texture.data_addr << 2);

    const uint32_t original_width = gxm::get_width(texture);
    const uint32_t original_height = gxm::get_height(texture);

    const uint32_t width = static_cast<uint32_t>(original_width * state.res_multiplier);
    const uint32_t height = static_cast<uint32_t>(original_height * state.res_multiplier);

    bool overlap = true;
    // Of course, this works under the assumption that range must be unique :D
    auto ite = color_address_lookup.upper_bound(address);
    if (ite == color_address_lookup.begin())
        // no match
        overlap = false;
    else
        --ite;
    // ite is now the first item with an address lower or equal to key

    overlap = (overlap && (ite->first + ite->second->total_bytes) > address);

    if (!overlap)
        return std::nullopt;

    if (*ite->second->dirty)
        // Guest wrote to the surface backing memory since it was rendered, so GPU data is stale.
        return std::nullopt;

    const vk::ComponentMapping swizzle = texture::translate_swizzle(gxm::get_format(texture));
    vk::Format vk_format = color::translate_surface_format(base_format);

    const bool is_srgb = texture.gamma_mode != 0;
    if (is_srgb) {
        if (vk_format == vk::Format::eR8G8B8A8Unorm) {
            vk_format = vk::Format::eR8G8B8A8Srgb;
        } else {
            LOG_WARN_ONCE("Trying to use gamma correction with non-compatible format {}", vk::to_string(vk_format));
        }
    }

    uint32_t stride_bytes = 0;
    SurfaceTiling tiling = SurfaceTiling::Swizzled;
    if (texture.texture_type() == SCE_GXM_TEXTURE_LINEAR_STRIDED) {
        stride_bytes = gxm::get_stride_in_bytes(texture);
        tiling = SurfaceTiling::Linear;
    } else {
        uint32_t pixel_stride = original_width;
        switch (texture.texture_type()) {
        case SCE_GXM_TEXTURE_LINEAR:
            // when the texture is linear, the stride should be aligned to 8 pixels
            tiling = SurfaceTiling::Linear;
            pixel_stride = align(pixel_stride, 8);
            break;
        case SCE_GXM_TEXTURE_TILED:
            // tiles are 32x32
            tiling = SurfaceTiling::Tiled;
            pixel_stride = align(pixel_stride, 32);
            break;
        case SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY:
            pixel_stride = next_power_of_two(pixel_stride);
            break;
        default:
            break;
        }
        stride_bytes = pixel_stride * gxm::bits_per_pixel(base_format) / 8;
    }
    uint32_t total_surface_size = stride_bytes * original_height;

    ColorSurfaceCacheInfo &info = *ite->second;

    if ((base_format == SCE_GXM_COLOR_BASE_FORMAT_U8U8U8 || info.format == SCE_GXM_COLOR_BASE_FORMAT_U8U8U8)
        && base_format != info.format)
        // don't even try to match u8u8u8 with something else
        return std::nullopt;

    if (tiling != info.tiling || info.stride_bytes != stride_bytes)
        // if the tiling is different, also don't try to match them
        // about the strides, I've yet to see a case where the byte stride is different
        return std::nullopt;

    // Check if we can use this surface
    bool addr_in_range_of_cache = ((address + total_surface_size) <= (ite->first + info.total_bytes + 4));

    if (ite->first != address && !addr_in_range_of_cache)
        // persona 4 sample from the top of a texture while the bottom wasn't rendered to, the fact that both the surface and
        // the texture start at the same location should be enough
        return std::nullopt;

    uint32_t bytes_per_pixel_requested = gxm::bits_per_pixel(base_format) / 8;
    uint32_t bytes_per_pixel_in_store = gxm::bits_per_pixel(info.format) / 8;

    if (std::max(bytes_per_pixel_requested, bytes_per_pixel_in_store) % std::min(bytes_per_pixel_requested, bytes_per_pixel_in_store) != 0)
        return std::nullopt;

    // Unsigned sampling preserves distinct F16 byte patterns that SNORM would merge.
    const auto store_is_f16 = [](SceGxmColorBaseFormat f) {
        return f == SCE_GXM_COLOR_BASE_FORMAT_F16
            || f == SCE_GXM_COLOR_BASE_FORMAT_F16F16
            || f == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16;
    };
    const auto unsigned_sibling = [](vk::Format fmt) {
        switch (fmt) {
        case vk::Format::eR8Snorm: return vk::Format::eR8Unorm;
        case vk::Format::eR8G8Snorm: return vk::Format::eR8G8Unorm;
        case vk::Format::eR8G8B8A8Snorm: return vk::Format::eR8G8B8A8Unorm;
        case vk::Format::eR16Snorm: return vk::Format::eR16Unorm;
        case vk::Format::eR16G16Snorm: return vk::Format::eR16G16Unorm;
        case vk::Format::eR16G16B16A16Snorm: return vk::Format::eR16G16B16A16Unorm;
        case vk::Format::eR8Sint: return vk::Format::eR8Uint;
        case vk::Format::eR8G8Sint: return vk::Format::eR8G8Uint;
        case vk::Format::eR8G8B8A8Sint: return vk::Format::eR8G8B8A8Uint;
        default: return fmt;
        }
    };
    const auto is_rgba8 = [](vk::Format fmt) {
        return fmt == vk::Format::eR8G8B8A8Unorm || fmt == vk::Format::eR8G8B8A8Srgb;
    };
    bool raw_bits_cast = allow_raw_bits && base_format == SCE_GXM_COLOR_BASE_FORMAT_F32F32
        && info.format == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16
        && swizzle.r == vk::ComponentSwizzle::eR && swizzle.g == vk::ComponentSwizzle::eG
        && info.swizzle == vkutil::rgba_mapping;
    if (raw_bits_cast) {
        if (!raw_cast_supported.has_value()) {
            const auto properties = state.physical_device.getFormatProperties(vk::Format::eR16G16B16A16Unorm);
            const auto required = vk::FormatFeatureFlagBits::eSampledImage
                | vk::FormatFeatureFlagBits::eTransferSrc | vk::FormatFeatureFlagBits::eTransferDst
                | vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
            raw_cast_supported = (properties.optimalTilingFeatures & required) == required;
        }
        raw_bits_cast = *raw_cast_supported;
    }

    const auto linear_sibling = [](vk::Format fmt) {
        return fmt == vk::Format::eR8G8B8A8Srgb ? vk::Format::eR8G8B8A8Unorm : fmt;
    };
    const bool is_typeless = bytes_per_pixel_requested != bytes_per_pixel_in_store;
    // Copy in the store gamma to preserve bytes; apply the requested gamma through the view.
    // A typeless cast is written by the reinterpret compute pass, and sRGB images are not
    // storage-capable on NVK, so its image is always linear and gamma lives in the view.
    // A typeless view of a float store carries bit patterns, not colours: its gamma bit is
    // ignored (Resistance / Black Ops rebuild halves from it and go dark if it is decoded).
    const bool cast_keeps_gamma = is_typeless && is_rgba8(vk_format) && !store_is_f16(info.format);
    const vk::Format cast_view_format = raw_bits_cast ? vk::Format::eR16G16B16A16Unorm
        : is_typeless
        ? (store_is_f16(info.format) ? unsigned_sibling(linear_sibling(vk_format)) : linear_sibling(vk_format))
        : ((is_rgba8(vk_format) && is_rgba8(info.texture.format)) ? info.texture.format : vk_format);
    const vk::Format cast_sampled_format = raw_bits_cast ? cast_view_format
        : is_typeless
        ? (cast_keeps_gamma ? vk_format : cast_view_format)
        : vk_format;
    const bool cast_needs_alt_gamma = is_rgba8(cast_view_format)
        && (is_typeless ? cast_keeps_gamma : is_rgba8(info.texture.format));

    // TODO: this is true only for linear textures (and also kind of for tiled textures) (and in this case start_x = 0),
    // for swizzled textures this is different
    const uint32_t data_delta = address - ite->first;
    uint32_t start_sourced_line = static_cast<uint32_t>((data_delta / stride_bytes) * state.res_multiplier);
    uint32_t start_x = static_cast<uint32_t>((data_delta % stride_bytes) / bytes_per_pixel_requested * state.res_multiplier);

    // Regroup aligned 64-to-32-bit casts at native resolution to avoid interleaving after upscaling.
    const uint32_t guard_native_byte_offset = stride_bytes ? (data_delta % stride_bytes) : 0u;
    const uint32_t guard_sub_texel_byte = bytes_per_pixel_in_store ? (guard_native_byte_offset % bytes_per_pixel_in_store) : 0u;
    const uint32_t guard_native_store_col = bytes_per_pixel_in_store ? (guard_native_byte_offset / bytes_per_pixel_in_store) : 0u;
    const uint32_t guard_ratio = bytes_per_pixel_requested ? (bytes_per_pixel_in_store / bytes_per_pixel_requested) : 0u;

    const bool is_typeless_cast = bytes_per_pixel_requested != bytes_per_pixel_in_store;
    const bool cast_phase_hi = is_typeless_cast && guard_ratio != 0
        && (guard_sub_texel_byte / bytes_per_pixel_requested) != 0;

    const bool use_compute_deinterleave = state.res_multiplier != 1.0f
        && bytes_per_pixel_in_store == 8 && bytes_per_pixel_requested == 4 && guard_ratio == 2
        && guard_native_store_col == 0 && start_sourced_line == 0
        && (guard_sub_texel_byte % bytes_per_pixel_requested) == 0
        && info.original_width > 0 && info.original_height > 0;

    // A word-offset view changes the cast layout, invalidating earlier copies.
    if (cast_phase_hi && !info.has_phase_view) {
        info.has_phase_view = true;
        for (CastedTexture &casted_texture : info.casted_textures)
            casted_texture.scene_timestamp = 0;
    }

    // Allocate the raw alias lazily, only when a cast might need it.
    const bool same_size_reinterpret = bytes_per_pixel_requested == bytes_per_pixel_in_store
        && base_format != info.format;
    if (state.features.preserve_f16_nan_as_u16 && (is_typeless_cast || same_size_reinterpret) && !info.raw_image
        && info.format == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16)
        create_raw_alias(info);

    if (static_cast<uint16_t>(start_sourced_line + height) > info.height)
        LOG_WARN_ONCE("Trying to use texture partially in the surface cache");

    // We should be able to use this texture, so set it as mru
    color_surface_queue.set_as_mru(&info);

    const vk::ImageView color_handle_view = reinterpret_cast<VKContext *>(state.context)->current_color_view;
    const bool is_same_image = (color_handle_view == info.texture.view) || (color_handle_view == info.alternate_view);

    if (state.features.use_texture_viewport && base_format == info.format) {
        // use a texture viewport
        *texture_viewport = {
            .ratio = {
                original_width / static_cast<float>(info.original_width),
                original_height / static_cast<float>(info.original_height) },
            .offset = { start_x / static_cast<float>(info.width), start_sourced_line / static_cast<float>(info.height) }
        };

        // if everything matches
        if (vk_format == info.texture.format && swizzle == info.swizzle)
            return TextureLookupResult{
                info.texture.view,
                info.texture.layout,
                info.texture.format
            };

        // use the other view with the correct swizzle / gamma correction
        if (!info.alternate_view) {
            vk::ComponentMapping resulting_mapping = vkutil::color_to_texture_swizzle(info.swizzle, swizzle);

            vk::ImageViewCreateInfo view_info{
                .image = info.texture.image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk_format,
                .components = resulting_mapping,
                .subresourceRange = vkutil::color_subresource_range
            };
            info.alternate_view = state.device.createImageView(view_info);
        }

        return TextureLookupResult{
            info.alternate_view,
            info.texture.layout,
            info.texture.format
        };
    }

    if (is_same_image || (start_sourced_line != 0) || (start_x != 0) || (info.width != width) || (info.height != height) || (info.format != base_format)) {
        const uint64_t scene_timestamp = reinterpret_cast<VKContext *>(state.context)->scene_timestamp;

        std::vector<CastedTexture> &casted_vec = info.casted_textures;

        CastedTexture *casted = nullptr;

        // Look in cast cache and grab one. The cache really does not store immediate grab on now, but rather to reduce the synchronization in the pipeline (use different texture)
        for (size_t i = 0; i < casted_vec.size();) {
            if ((casted_vec[i].cropped_height == height) && (casted_vec[i].cropped_width == width) && (casted_vec[i].cropped_y == start_sourced_line) && (casted_vec[i].cropped_x == start_x) && (casted_vec[i].format == base_format) && (casted_vec[i].texture.format == cast_view_format)) {
                casted = &casted_vec[i];

                if (casted->scene_timestamp == scene_timestamp) {
                    // already copied for this scene, don't do it again
                    const bool use_alt_gamma = casted->alt_gamma_view && cast_sampled_format != casted->texture.format;
                    return TextureLookupResult{
                        use_alt_gamma ? casted->alt_gamma_view : casted->texture.view,
                        casted->texture.layout,
                        use_alt_gamma ? cast_sampled_format : casted->texture.format,
                        is_typeless_cast && !info.has_phase_view,
                        cast_phase_hi,
                        raw_bits_cast
                    };
                }

                break;
            } else {
                i++;
            }
        }

        // use prerender cmd as we can't copy an image or use pipeline barriers in a render pass
        VKContext *context = reinterpret_cast<VKContext *>(state.context);
        vk::CommandBuffer cmd_buffer = context->prerender_cmd;

        if (casted == nullptr) {
            // Try to crop + cast
            casted_vec.resize(casted_vec.size() + 1);
            casted = &casted_vec[casted_vec.size() - 1];
            *casted = CastedTexture{
                .cropped_x = start_x,
                .cropped_y = start_sourced_line,
                .cropped_width = width,
                .cropped_height = height,
                .format = base_format
            };
            // Cropped casts use native dimensions to match guest UVs; full-width reads stay upscaled.
            const bool full_width_read = (start_x == 0) && (width == info.width);
            if (bytes_per_pixel_requested == bytes_per_pixel_in_store && !full_width_read) {
                casted->texture.width = original_width;
                casted->texture.height = original_height;
            } else {
                casted->texture.width = width;
                casted->texture.height = height;
            }
            casted->texture.format = cast_view_format;

            // find the swizzle we need to apply
            const std::uint8_t components_in_store = vk::componentCount(info.texture.format);
            const std::uint8_t components_requested = vk::componentCount(vk_format);
            vk::ComponentMapping resulting_swizzle;
            // Only take into consideration the current swizzle when it makes sense
            // (Not perfect but better than doing this all the time)
            if (bytes_per_pixel_requested == bytes_per_pixel_in_store && components_in_store == components_requested)
                resulting_swizzle = vkutil::color_to_texture_swizzle(info.swizzle, swizzle);
            else
                resulting_swizzle = swizzle;

            if (raw_bits_cast)
                resulting_swizzle = vkutil::default_comp_mapping;

            if (use_compute_deinterleave)
                casted->texture.init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eStorage,
                    resulting_swizzle, vk::ImageCreateFlagBits::eMutableFormat);
            else {
                // Listing both view formats lets the driver retain compression.
                const void *cast_info_pNext = nullptr;
                if (cast_needs_alt_gamma && support_image_format_specifier) {
                    static const vk::Format cast_view_formats[] = { vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb };
                    static const vk::ImageFormatListCreateInfoKHR cast_info_formats{
                        .viewFormatCount = 2,
                        .pViewFormats = cast_view_formats
                    };
                    cast_info_pNext = &cast_info_formats;
                }
                casted->texture.init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, resulting_swizzle,
                    cast_needs_alt_gamma ? vk::ImageCreateFlags(vk::ImageCreateFlagBits::eMutableFormat) : vk::ImageCreateFlags(), cast_info_pNext);
            }

            if (cast_needs_alt_gamma) {
                const vk::ImageViewCreateInfo alt_gamma_view_info{
                    .image = casted->texture.image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = (cast_view_format == vk::Format::eR8G8B8A8Srgb) ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb,
                    .components = resulting_swizzle,
                    .subresourceRange = vkutil::color_subresource_range
                };
                casted->alt_gamma_view = state.device.createImageView(alt_gamma_view_info);
            }
            casted->texture.transition_to(cmd_buffer, use_compute_deinterleave ? vkutil::ImageLayout::StorageImage : vkutil::ImageLayout::TransferDst);
        } else {
            casted->texture.transition_to_discard(cmd_buffer, use_compute_deinterleave ? vkutil::ImageLayout::StorageImage : vkutil::ImageLayout::TransferDst);
        }

        casted->scene_timestamp = scene_timestamp;

        if (bytes_per_pixel_requested == bytes_per_pixel_in_store) {
            const int32_t src_w = static_cast<int32_t>(std::min<uint32_t>(width, info.width - start_x));
            const int32_t src_h = static_cast<int32_t>(std::min<uint32_t>(height, info.height - start_sourced_line));
            // Clip the destination proportionally when the source is smaller than requested.
            const int32_t dst_w = std::max(1, static_cast<int32_t>(casted->texture.width * static_cast<uint32_t>(src_w) / width));
            const int32_t dst_h = std::max(1, static_cast<int32_t>(casted->texture.height * static_cast<uint32_t>(src_h) / height));

            // Copy matching extents to preserve bits; blits convert between formats.
            const bool is_reinterpretation = info.texture.format != casted->texture.format;
            const bool bit_copy = vk::blockSize(info.texture.format) == vk::blockSize(casted->texture.format)
                && src_w == dst_w && src_h == dst_h;
            const bool copy_from_raw = (bit_copy || raw_bits_cast) && typeless_read_from_raw(info)
                && vk::blockSize(info.raw_image->format) == vk::blockSize(casted->texture.format);
            const vk::Image src_image = copy_from_raw ? info.raw_image->image : info.texture.image;

            barrier_render_to_transfer_read(cmd_buffer, src_image);
            if (bit_copy) {
                const vk::ImageCopy image_copy{
                    .srcSubresource = vkutil::color_subresource_layer,
                    .srcOffset = { static_cast<int32_t>(start_x), static_cast<int32_t>(start_sourced_line), 0 },
                    .dstSubresource = vkutil::color_subresource_layer,
                    .dstOffset = { 0, 0, 0 },
                    .extent = { static_cast<uint32_t>(src_w), static_cast<uint32_t>(src_h), 1 }
                };
                cmd_buffer.copyImage(src_image, vk::ImageLayout::eGeneral, casted->texture.image, vk::ImageLayout::eTransferDstOptimal, image_copy);
            } else if (raw_bits_cast) {
                // Resize words only after copying them into a format without NaN encodings.
                auto &carrier = casted->raw_copy_image;
                if (!carrier.image || carrier.width != uint32_t(src_w) || carrier.height != uint32_t(src_h)) {
                    state.frame().destroy_queue.add_image(carrier);
                    carrier.width = uint32_t(src_w);
                    carrier.height = uint32_t(src_h);
                    carrier.format = vk::Format::eR16G16B16A16Unorm;
                    carrier.init_image(vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst);
                }
                carrier.transition_to_discard(cmd_buffer, vkutil::ImageLayout::TransferDst);
                const vk::ImageCopy copy{
                    .srcSubresource = vkutil::color_subresource_layer,
                    .srcOffset = { int32_t(start_x), int32_t(start_sourced_line), 0 },
                    .dstSubresource = vkutil::color_subresource_layer,
                    .dstOffset = { 0, 0, 0 },
                    .extent = { uint32_t(src_w), uint32_t(src_h), 1 }
                };
                cmd_buffer.copyImage(src_image, vk::ImageLayout::eGeneral, carrier.image, vk::ImageLayout::eTransferDstOptimal, copy);
                carrier.transition_to(cmd_buffer, vkutil::ImageLayout::TransferSrc);
                const vk::ImageBlit blit{
                    .srcSubresource = vkutil::color_subresource_layer,
                    .srcOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ src_w, src_h, 1 } },
                    .dstSubresource = vkutil::color_subresource_layer,
                    .dstOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ dst_w, dst_h, 1 } }
                };
                cmd_buffer.blitImage(carrier.image, vk::ImageLayout::eTransferSrcOptimal, casted->texture.image,
                    vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
            } else {
                const vk::ImageBlit blit{
                    .srcSubresource = vkutil::color_subresource_layer,
                    .srcOffsets = std::array<vk::Offset3D, 2>{
                        vk::Offset3D{ static_cast<int32_t>(start_x), static_cast<int32_t>(start_sourced_line), 0 },
                        vk::Offset3D{ static_cast<int32_t>(start_x) + src_w, static_cast<int32_t>(start_sourced_line) + src_h, 1 } },
                    .dstSubresource = vkutil::color_subresource_layer,
                    .dstOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ dst_w, dst_h, 1 } }
                };
                // Packed data requires nearest filtering.
                cmd_buffer.blitImage(info.texture.image, vk::ImageLayout::eGeneral, casted->texture.image, vk::ImageLayout::eTransferDstOptimal,
                    blit, is_reinterpretation ? vk::Filter::eNearest : vk::Filter::eLinear);
            }
        } else if (use_compute_deinterleave) {
            LOG_INFO_ONCE("Game is doing typeless copies, regrouped by the reinterpret compute pass");
            ensure_reinterpret_pipeline();

            const uint32_t ratio = bytes_per_pixel_in_store / bytes_per_pixel_requested;
            const uint32_t half_index = guard_sub_texel_byte / bytes_per_pixel_requested;

            if (!casted->reinterpret_view) {
                const vk::ImageViewCreateInfo cast_view_info{
                    .image = casted->texture.image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = vk::Format::eR32Uint,
                    .components = {},
                    .subresourceRange = vkutil::color_subresource_range
                };
                casted->reinterpret_view = state.device.createImageView(cast_view_info);
            }
            const bool read_from_raw = typeless_read_from_raw(info);
            if (info.reinterpret_store_view && info.reinterpret_view_is_raw != read_from_raw) {
                state.frame().destroy_queue.add(info.reinterpret_store_view);
                info.reinterpret_store_view = nullptr;
            }
            if (!info.reinterpret_store_view) {
                const vk::ImageViewCreateInfo store_view_info{
                    .image = typeless_read_source(info),
                    .viewType = vk::ImageViewType::e2D,
                    .format = vk::Format::eR32G32Uint,
                    .components = {},
                    .subresourceRange = vkutil::color_subresource_range
                };
                info.reinterpret_store_view = state.device.createImageView(store_view_info);
                info.reinterpret_view_is_raw = read_from_raw;
            }

            const vk::ImageMemoryBarrier store_to_compute{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = typeless_read_source(info),
                .subresourceRange = vkutil::color_subresource_range
            };
            cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
                vk::PipelineStageFlagBits::eComputeShader, vk::DependencyFlags(), {}, {}, store_to_compute);

            const vk::DescriptorSet dset = reinterpret_desc_sets[reinterpret_desc_idx];
            reinterpret_desc_idx = (reinterpret_desc_idx + 1) % static_cast<uint32_t>(reinterpret_desc_sets.size());

            const vk::DescriptorImageInfo store_ii{ reinterpret_sampler, info.reinterpret_store_view, vk::ImageLayout::eGeneral };
            const vk::DescriptorImageInfo cast_ii{ nullptr, casted->reinterpret_view, vk::ImageLayout::eGeneral };
            std::array<vk::WriteDescriptorSet, 2> writes;
            writes[0] = vk::WriteDescriptorSet{ .dstSet = dset, .dstBinding = 0, .dstArrayElement = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler };
            writes[0].setImageInfo(store_ii);
            writes[1] = vk::WriteDescriptorSet{ .dstSet = dset, .dstBinding = 1, .dstArrayElement = 0, .descriptorType = vk::DescriptorType::eStorageImage };
            writes[1].setImageInfo(cast_ii);
            state.device.updateDescriptorSets(writes, {});

            const ReinterpretPushConstants pc{
                .out_width = width,
                .out_height = height,
                .scaled_store_w = info.texture.width,
                .scaled_store_h = info.texture.height,
                .ratio = ratio,
                .half_index = half_index,
                .interleave = (!info.has_phase_view && width == ratio * info.texture.width) ? 1u : 0u
            };
            cmd_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, reinterpret_pipeline);
            cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, reinterpret_pipeline_layout, 0, dset, {});
            cmd_buffer.pushConstants(reinterpret_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
            cmd_buffer.dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
        } else {
            LOG_INFO_ONCE("Game is doing typeless copies");
            // We must use a transition buffer
            vk::DeviceSize buffer_size = stride_bytes * static_cast<size_t>(state.res_multiplier * align(height, 4)) + start_x * bytes_per_pixel_requested;
            if (!casted->transition_buffer.buffer || casted->transition_buffer.size < buffer_size) {
                // create or re-create the buffer
                state.frame().destroy_queue.add_buffer(casted->transition_buffer);
                casted->transition_buffer = vkutil::Buffer(buffer_size);
                casted->transition_buffer.init_buffer(vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc);
            }

            // copy the image to the buffer
            const uint32_t src_pixel_stride = static_cast<uint32_t>((info.stride_bytes / bytes_per_pixel_in_store) * state.res_multiplier);
            vk::BufferImageCopy copy_image_buffer{
                .bufferOffset = 0,
                .bufferRowLength = src_pixel_stride,
                .bufferImageHeight = height,
                .imageSubresource = vkutil::color_subresource_layer,
                .imageOffset = { 0,
                    static_cast<int32_t>(start_sourced_line),
                    0 },
                .imageExtent = { info.width, height, 1 }
            };
            const vk::Image typeless_src = typeless_read_source(info);
            barrier_render_to_transfer_read(cmd_buffer, typeless_src);
            cmd_buffer.copyImageToBuffer(typeless_src, vk::ImageLayout::eGeneral, casted->transition_buffer.buffer, copy_image_buffer);

            const vk::BufferMemoryBarrier transition_barrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = casted->transition_buffer.buffer,
                .offset = 0,
                .size = VK_WHOLE_SIZE
            };
            cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlags(), {}, transition_barrier, {});

            // then the buffer to the image
            const uint32_t dst_pixel_stride = (stride_bytes / bytes_per_pixel_requested) * state.res_multiplier;
            copy_image_buffer
                .setBufferOffset(start_x * bytes_per_pixel_requested)
                .setBufferRowLength(dst_pixel_stride)
                .setImageOffset({ 0, 0, 0 })
                .setImageExtent({ width, height, 1 });
            cmd_buffer.copyBufferToImage(casted->transition_buffer.buffer, casted->texture.image, vk::ImageLayout::eTransferDstOptimal, copy_image_buffer);
        }
        casted->texture.transition_to(cmd_buffer, vkutil::ImageLayout::SampledImage);

        const bool use_alt_gamma = casted->alt_gamma_view && cast_sampled_format != casted->texture.format;
        return TextureLookupResult{
            use_alt_gamma ? casted->alt_gamma_view : casted->texture.view,
            casted->texture.layout,
            use_alt_gamma ? cast_sampled_format : casted->texture.format,
            is_typeless_cast && !info.has_phase_view,
            cast_phase_hi,
            raw_bits_cast
        };
    } else {
        // the renderpass external dependencies should take care of the barrier
        if (swizzle == info.swizzle && vk_format == info.texture.format)
            // we can use the same texture view
            return TextureLookupResult{
                info.texture.view,
                info.texture.layout,
                info.texture.format
            };

        if (!info.alternate_view) {
            vk::ComponentMapping resulting_mapping = vkutil::color_to_texture_swizzle(info.swizzle, swizzle);

            vk::ImageViewCreateInfo view_info{
                .image = info.texture.image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk_format,
                .components = resulting_mapping,
                .subresourceRange = vkutil::color_subresource_range
            };
            info.alternate_view = state.device.createImageView(view_info);
        }

        return TextureLookupResult{
            info.alternate_view,
            vkutil::ImageLayout::ColorAttachmentReadWrite,
            vk_format
        };
    }
}

// Tiled depth-stencil allocations include the full final 32-row tile.
static int32_t ds_rows_allocated(SurfaceTiling tiling, int32_t memory_height) {
    return (tiling == SurfaceTiling::Tiled) ? align(memory_height, 32) : memory_height;
}

static bool ds_format_has_stencil(vk::Format format) {
    return format == vk::Format::eD16UnormS8Uint
        || format == vk::Format::eD24UnormS8Uint
        || format == vk::Format::eD32SfloatS8Uint;
}

bool VKSurfaceCache::begin_ds_scene_depth_check(const SceGxmDepthStencilSurface &depth_stencil,
    bool this_scene_stores, Address scene_color_addr) {
    DepthStencilSurfaceCacheInfo *cached_info = nullptr;
    if (depth_stencil.depth_data) {
        const auto it = depth_address_lookup.find(depth_stencil.depth_data.address());
        if (it != depth_address_lookup.end())
            cached_info = it->second;
    } else if (depth_stencil.stencil_data) {
        const auto it = stencil_address_lookup.find(depth_stencil.stencil_data.address());
        if (it != stencil_address_lookup.end())
            cached_info = it->second;
    }

    pending_ds_scene = cached_info;
    pending_ds_scene_stores = this_scene_stores;
    if (!cached_info)
        return true;

    const bool is_continuation = cached_info->last_scene_color_addr == scene_color_addr;
    cached_info->last_scene_color_addr = scene_color_addr;
    return cached_info->depth_content_stored || is_continuation;
}

void VKSurfaceCache::resolve_ds_scene_end(bool scene_wrote_depth) {
    if (pending_ds_scene && scene_wrote_depth)
        pending_ds_scene->depth_content_stored = pending_ds_scene_stores;
    pending_ds_scene = nullptr;
}

SurfaceRetrieveResult VKSurfaceCache::retrieve_depth_stencil_for_framebuffer(SceGxmDepthStencilSurface *depth_stencil, const uint32_t width, const uint32_t height) {
    // when writing we use the render target size which is already upscaled
    int32_t memory_width = static_cast<int32_t>(width / state.res_multiplier);
    int32_t memory_height = static_cast<int32_t>(height / state.res_multiplier);

    const SurfaceTiling tiling = (depth_stencil->get_type() == SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR) ? SurfaceTiling::Linear : SurfaceTiling::Tiled;

    // check if MSAA is used, the depth buffer is never downscaled
    if (target->multisample_mode != SCE_GXM_MULTISAMPLE_NONE)
        memory_height *= 2;
    if (target->multisample_mode == SCE_GXM_MULTISAMPLE_4X)
        memory_width *= 2;

    const bool is_stencil_only = depth_stencil->depth_data.address() == 0;
    DepthStencilSurfaceCacheInfo *cached_info = nullptr;

    if (!is_stencil_only) {
        auto it = depth_address_lookup.find(depth_stencil->depth_data.address());
        if (it != depth_address_lookup.end())
            cached_info = it->second;
    } else {
        auto it = stencil_address_lookup.find(depth_stencil->stencil_data.address());
        if (it != stencil_address_lookup.end())
            cached_info = it->second;
    }

    if (cached_info != nullptr) {
        // this the most recently used depth-stencil surface
        ds_surface_queue.set_as_mru(cached_info);

        const bool need_remake = cached_info->texture.width < width
            || cached_info->texture.height < height
            || cached_info->stride_samples != depth_stencil->get_stride()
            || cached_info->tiling != tiling;

        if (!need_remake) {
            VKContext *context = static_cast<VKContext *>(state.context);
            uint32_t scale_x = 1;
            uint32_t scale_y = 1;
            if (target->multisample_mode != SCE_GXM_MULTISAMPLE_NONE && context
                && context->record.color_surface.downscale && !depth_stencil->force_store) {
                scale_y = 2;
                if (target->multisample_mode == SCE_GXM_MULTISAMPLE_4X)
                    scale_x = 2;
            }

            vkutil::Image &full = cached_info->texture;
            const uint32_t source_width = width * scale_x;
            const uint32_t source_height = height * scale_y;
            const auto format_features = state.physical_device.getFormatProperties(full.format).optimalTilingFeatures;
            const auto required_features = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
            const bool can_blit = (format_features & required_features) == required_features;

            if ((scale_x > 1 || scale_y > 1) && can_blit && full.image
                && full.width >= source_width && full.height >= source_height) {
                if (!cached_info->sample_rate_copy
                    || cached_info->sample_rate_copy->width != width
                    || cached_info->sample_rate_copy->height != height
                    || cached_info->sample_rate_copy->format != full.format) {
                    if (cached_info->sample_rate_copy) {
                        destroy_framebuffers(cached_info->sample_rate_copy->view);
                        state.frame().destroy_queue.add_image(*cached_info->sample_rate_copy);
                    }
                    cached_info->sample_rate_copy = std::make_unique<vkutil::Image>(width, height, full.format);
                    cached_info->sample_rate_copy->init_image(vk::ImageUsageFlagBits::eDepthStencilAttachment
                        | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc
                        | vk::ImageUsageFlagBits::eSampled);
                }

                vkutil::Image &resampled = *cached_info->sample_rate_copy;
                vk::CommandBuffer command_buffer = context->prerender_cmd;
                const vk::ImageSubresourceRange &range = ds_format_has_stencil(full.format)
                    ? vkutil::ds_subresource_range
                    : vkutil::d_subresource_range;
                full.transition_to(command_buffer, vkutil::ImageLayout::TransferSrc, range);
                resampled.transition_to_discard(command_buffer, vkutil::ImageLayout::TransferDst, range);

                const std::array<vk::Offset3D, 2> source_bounds{
                    vk::Offset3D{ 0, 0, 0 },
                    vk::Offset3D{ static_cast<int32_t>(source_width - (scale_x - 1)),
                        static_cast<int32_t>(source_height - (scale_y - 1)), 1 }
                };
                const std::array<vk::Offset3D, 2> destination_bounds{
                    vk::Offset3D{ 0, 0, 0 },
                    vk::Offset3D{ static_cast<int32_t>(width), static_cast<int32_t>(height), 1 }
                };
                const auto blit_aspect = [&](vk::ImageAspectFlagBits aspect) {
                    const vk::ImageSubresourceLayers layers{ aspect, 0, 0, 1 };
                    const vk::ImageBlit region{
                        .srcSubresource = layers,
                        .srcOffsets = source_bounds,
                        .dstSubresource = layers,
                        .dstOffsets = destination_bounds
                    };
                    command_buffer.blitImage(full.image, vk::ImageLayout::eTransferSrcOptimal,
                        resampled.image, vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
                };
                blit_aspect(vk::ImageAspectFlagBits::eDepth);
                if (ds_format_has_stencil(full.format))
                    blit_aspect(vk::ImageAspectFlagBits::eStencil);

                full.transition_to(command_buffer, vkutil::ImageLayout::DepthStencilReadOnly, range);
                resampled.transition_to(command_buffer, vkutil::ImageLayout::DepthStencilReadOnly, range);
                return { resampled.view, &resampled };
            }

            return {
                cached_info->texture.view,
                &cached_info->texture
            };
        }
    } else {
        // retrieve a new depth stencil
        cached_info = ds_surface_queue.get_lru();
    }

    // erase it if it was used previously
    if (cached_info->surface.depth_data)
        depth_address_lookup.erase(cached_info->surface.depth_data.address());
    if (cached_info->surface.stencil_data)
        stencil_address_lookup.erase(cached_info->surface.stencil_data.address());
    if (cached_info->texture.image)
        destroy_surface(*cached_info);

    // update the lookup info
    ds_surface_queue.set_as_mru(cached_info);
    if (depth_stencil->depth_data)
        depth_address_lookup[depth_stencil->depth_data.address()] = cached_info;
    if (depth_stencil->stencil_data)
        stencil_address_lookup[depth_stencil->stencil_data.address()] = cached_info;

    cached_info->surface = *depth_stencil;
    cached_info->memory_width = memory_width;
    cached_info->memory_height = memory_height;
    cached_info->multisample_mode = target->multisample_mode;
    cached_info->stride_samples = depth_stencil->get_stride();
    cached_info->tiling = tiling;
    cached_info->depth_content_stored = true;
    cached_info->last_scene_color_addr = 0;

    uint32_t bytes_per_sample;
    switch (depth_stencil->get_format()) {
    case SCE_GXM_DEPTH_STENCIL_FORMAT_S8:
        bytes_per_sample = 1;
        break;
    case SCE_GXM_DEPTH_STENCIL_FORMAT_D16:
        bytes_per_sample = 2;
        break;
    default:
        bytes_per_sample = 4;
        break;
    }
    cached_info->total_bytes = bytes_per_sample * depth_stencil->get_stride() * ds_rows_allocated(tiling, memory_height);

    vkutil::Image &image = cached_info->texture;

    // use prerender cmd in case we read from the depth buffer (although I really doubt this could happen)
    VKContext *context = reinterpret_cast<VKContext *>(state.context);
    vk::CommandBuffer cmd_buffer = context->prerender_cmd;

    image.width = width;
    image.height = height;
    image.format = state.deep_stencil_use;
    image.layout = vkutil::ImageLayout::Undefined;
    image.init_image(vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled);

    image.transition_to(cmd_buffer, vkutil::ImageLayout::TransferDst, vkutil::ds_subresource_range);
    vk::ClearDepthStencilValue clear_value{
        .depth = 1.0,
        .stencil = 0
    };
    cmd_buffer.clearDepthStencilImage(image.image, vk::ImageLayout::eTransferDstOptimal, clear_value, vkutil::ds_subresource_range);
    image.transition_to(cmd_buffer, vkutil::ImageLayout::DepthStencilReadOnly, vkutil::ds_subresource_range);

    return {
        image.view,
        &image
    };
}

bool VKSurfaceCache::try_transfer_depth_gpu(Address src_address, Address dst_address, uint32_t width, uint32_t height) {
    if (src_address == dst_address)
        return false;

    const auto src_it = depth_address_lookup.find(src_address);
    const auto dst_it = depth_address_lookup.find(dst_address);
    if (src_it == depth_address_lookup.end() || dst_it == depth_address_lookup.end())
        return false;
    if (color_address_lookup.contains(src_address) || color_address_lookup.contains(dst_address))
        return false;

    DepthStencilSurfaceCacheInfo *src_info = src_it->second;
    DepthStencilSurfaceCacheInfo *dst_info = dst_it->second;
    if (!src_info || !dst_info || src_info == dst_info)
        return false;
    if (!src_info->texture.image || !dst_info->texture.image)
        return false;

    const uint32_t scaled_width = static_cast<uint32_t>(width * state.res_multiplier);
    const uint32_t scaled_height = static_cast<uint32_t>(height * state.res_multiplier);
    const uint32_t copy_width = std::min({ scaled_width, src_info->texture.width, dst_info->texture.width });
    const uint32_t copy_height = std::min({ scaled_height, src_info->texture.height, dst_info->texture.height });
    if (copy_width == 0 || copy_height == 0)
        return false;

    vk::CommandBuffer transfer_cmd = nullptr;
    const vk::Fence fence = state.device.createFence({});
    {
        const std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
        transfer_cmd = vkutil::create_single_time_command(state.device, state.multithread_command_pool);

        const vk::ImageSubresourceRange &range = ds_format_has_stencil(src_info->texture.format)
            ? vkutil::ds_subresource_range
            : vkutil::d_subresource_range;
        src_info->texture.transition_to(transfer_cmd, vkutil::ImageLayout::TransferSrc, range);
        dst_info->texture.transition_to_discard(transfer_cmd, vkutil::ImageLayout::TransferDst, range);

        const auto copy_aspect = [&](vk::ImageAspectFlagBits aspect) {
            const vk::ImageSubresourceLayers layers{ aspect, 0, 0, 1 };
            const vk::ImageCopy image_copy{
                .srcSubresource = layers,
                .srcOffset = { 0, 0, 0 },
                .dstSubresource = layers,
                .dstOffset = { 0, 0, 0 },
                .extent = { copy_width, copy_height, 1 }
            };
            transfer_cmd.copyImage(src_info->texture.image, vk::ImageLayout::eTransferSrcOptimal,
                dst_info->texture.image, vk::ImageLayout::eTransferDstOptimal, image_copy);
        };
        copy_aspect(vk::ImageAspectFlagBits::eDepth);
        if (ds_format_has_stencil(src_info->texture.format))
            copy_aspect(vk::ImageAspectFlagBits::eStencil);

        src_info->texture.transition_to(transfer_cmd, vkutil::ImageLayout::DepthStencilReadOnly, range);
        dst_info->texture.transition_to(transfer_cmd, vkutil::ImageLayout::DepthStencilReadOnly, range);
        transfer_cmd.end();
    }

    vk::SubmitInfo submit_info{};
    submit_info.setCommandBuffers(transfer_cmd);
    state.submit_general(submit_info, fence, "depth transfer submission");

    dst_info->depth_content_stored = true;

    CallbackRequestFunction cleanup = [&state = this->state, fence, transfer_cmd]() {
        const vk::Result result = state.device.waitForFences(fence, vk::True, std::numeric_limits<uint64_t>::max());
        if (result != vk::Result::eSuccess)
            LOG_ERROR("Could not wait for the depth transfer fence.");

        state.device.destroyFence(fence);

        const std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
        state.device.freeCommandBuffers(state.multithread_command_pool, transfer_cmd);
    };
    state.request_queue.push(CallbackRequest{ new CallbackRequestFunction(std::move(cleanup)) });
    return true;
}

std::optional<TextureLookupResult> VKSurfaceCache::retrieve_depth_stencil_as_texture(const SceGxmTexture &texture, TextureViewport *texture_viewport) {
    SceGxmTextureBaseFormat base_format = gxm::get_base_format(gxm::get_format(texture));
    bool can_be_depth = false;
    bool can_be_stencil = false;

    uint32_t bytes_per_sample = 4;
    switch (base_format) {
        // 8bit stencil
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
        bytes_per_sample = 1;
        can_be_stencil = true;
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16:
        bytes_per_sample = 2;
        [[fallthrough]];
    case SCE_GXM_TEXTURE_BASE_FORMAT_X8U24:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32M:
        can_be_depth = true;
        break;
    default:
        break;
    }
    int32_t memory_width = gxm::get_width(texture);
    int32_t memory_height = gxm::get_height(texture);

    SurfaceTiling tiling;
    uint32_t stride_samples;

    switch (texture.texture_type()) {
    case SCE_GXM_TEXTURE_LINEAR:
        tiling = SurfaceTiling::Linear;
        stride_samples = align(memory_width, 8);
        break;
    case SCE_GXM_TEXTURE_LINEAR_STRIDED:
        tiling = SurfaceTiling::Linear;
        stride_samples = (gxm::get_stride_in_bytes(texture) * 8) / gxm::bits_per_pixel(base_format);
        break;
    case SCE_GXM_TEXTURE_TILED:
        tiling = SurfaceTiling::Tiled;
        stride_samples = align(memory_width, 32);
        break;
    default:
        // a depth/stencil is never swizzled
        return std::nullopt;
    }

    if (stride_samples % 32 != 0)
        // a depth/stencil always has a stride which is a multiple of the tile size
        return std::nullopt;

    // take upscaling into account
    uint32_t width = static_cast<uint32_t>(memory_width * state.res_multiplier);
    uint32_t height = static_cast<uint32_t>(memory_height * state.res_multiplier);
    uint32_t total_bytes = bytes_per_sample * stride_samples * memory_height;

    const uint32_t address = texture.data_addr << 2;
    uint32_t surface_address = 0;
    DepthStencilSurfaceCacheInfo *found_info = nullptr;

    if (can_be_depth) {
        // get the first depth surface with an address lower or equal to address
        auto it = depth_address_lookup.upper_bound(address);
        if (it != depth_address_lookup.begin()) {
            --it;

            // the texture must be contained entirely in the depth surface
            if (address + total_bytes <= it->first + it->second->total_bytes) {
                surface_address = it->first;
                found_info = it->second;
            }
        }
    }
    if (!found_info && can_be_stencil) {
        // get the first stencil surface with an address lower or equal to address
        auto it = stencil_address_lookup.upper_bound(address);
        if (it != stencil_address_lookup.begin()) {
            --it;

            // note: we don't support sampling the stencil from a D24S8 depth-stencil
            // so we can assume any stencil uses only 1 byte per sample
            uint32_t surface_bytes = it->second->stride_samples * ds_rows_allocated(it->second->tiling, it->second->memory_height) * 1;

            // the texture must be contained entirely in the stencil surface
            if (address + total_bytes <= it->first + surface_bytes) {
                surface_address = it->first;
                found_info = it->second;
            }
        }
    }

    if (found_info == nullptr)
        return std::nullopt;

    DepthStencilSurfaceCacheInfo &cached_info = *found_info;
    if (tiling != cached_info.tiling || stride_samples != cached_info.stride_samples)
        return std::nullopt;

    // we sample from it, set the surface as most recently used
    ds_surface_queue.set_as_mru(found_info);

    // take MSAA into account
    if (cached_info.multisample_mode != SCE_GXM_MULTISAMPLE_NONE)
        height /= 2;
    if (cached_info.multisample_mode == SCE_GXM_MULTISAMPLE_4X)
        width /= 2;

    const bool is_stencil = can_be_stencil;

    const uint32_t delta_samples = (address - surface_address) / bytes_per_sample;
    uint32_t delta_col_samples = delta_samples % stride_samples;
    uint32_t delta_row_samples = delta_samples / stride_samples;

    vk::ImageView ds_attachment = reinterpret_cast<VKContext *>(state.context)->current_ds_view;
    const bool reading_ds_attachment = cached_info.texture.view == ds_attachment;
    const bool same_dimension = memory_width == cached_info.memory_width
        && memory_height == cached_info.memory_height
        && delta_col_samples == 0
        && delta_row_samples == 0;

    if (!reading_ds_attachment && (state.features.use_texture_viewport || same_dimension)) {
        // we can just sample from the surface itself

        // we must create a new read-only view if it is not already present
        vk::ImageView &img_view = is_stencil ? cached_info.stencil_view : cached_info.depth_view;
        if (!img_view) {
            vk::ImageSubresourceRange range = vkutil::ds_subresource_range;
            range.aspectMask = is_stencil ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlagBits::eDepth;
            vk::ImageViewCreateInfo view_info{
                .image = cached_info.texture.image,
                .viewType = vk::ImageViewType::e2D,
                .format = state.deep_stencil_use,
                .components = {},
                .subresourceRange = range
            };
            img_view = state.device.createImageView(view_info);
        }

        const float inv_surface_width = 1 / static_cast<float>(cached_info.memory_width);
        const float inv_surface_height = 1 / static_cast<float>(cached_info.memory_height);
        if (state.features.use_texture_viewport) {
            texture_viewport->offset = {
                delta_col_samples * inv_surface_width,
                delta_row_samples * inv_surface_height
            };
            texture_viewport->ratio = {
                memory_width * inv_surface_width,
                memory_height * inv_surface_height
            };
        }

        return TextureLookupResult{
            img_view,
            vkutil::ImageLayout::DepthStencilReadOnly,
            state.deep_stencil_use
        };
    }

    const uint64_t scene_timestamp = reinterpret_cast<VKContext *>(state.context)->scene_timestamp;

    int read_surface_idx = -1;
    for (int i = 0; i < cached_info.read_surfaces.size(); i++) {
        auto &read_surface = cached_info.read_surfaces[i];
        if (read_surface.depth_view.width == width
            && read_surface.depth_view.height == height
            && read_surface.delta_row == delta_row_samples
            && read_surface.delta_col == delta_col_samples) {
            read_surface_idx = i;
            break;
        }
    }

    if (read_surface_idx == -1) {
        // no compatible read surface found

        DepthSurfaceView read_only{
            .depth_view = vkutil::Image(width, height, state.deep_stencil_use),
            .scene_timestamp = 0,
            .delta_col = delta_col_samples,
            .delta_row = delta_row_samples,
        };
        read_only.depth_view.init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
        // we want a texture view with only the depth or stencil aspect bit
        // TODO: not efficient
        state.device.destroy(read_only.depth_view.view);
        read_only.depth_view.view = nullptr;

        read_surface_idx = cached_info.read_surfaces.size();
        cached_info.read_surfaces.emplace_back(std::move(read_only));
    }

    DepthSurfaceView &read_only = cached_info.read_surfaces[read_surface_idx];
    vkutil::Image &img_view = is_stencil ? read_only.stencil_view : read_only.depth_view;

    if (!img_view.view) {
        vk::ImageSubresourceRange range = vkutil::ds_subresource_range;
        range.aspectMask = is_stencil ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlagBits::eDepth;
        vk::ImageViewCreateInfo view_info{
            .image = read_only.depth_view.image,
            .viewType = vk::ImageViewType::e2D,
            .format = state.deep_stencil_use,
            .components = {},
            .subresourceRange = range
        };
        img_view.view = state.device.createImageView(view_info);
        img_view.layout = vkutil::ImageLayout::SampledImage;
    }

    // copy the depth stencil only once per scene
    if (read_only.scene_timestamp == scene_timestamp)
        return TextureLookupResult{
            img_view.view,
            img_view.layout,
            img_view.format
        };

    read_only.scene_timestamp = scene_timestamp;

    // use prerender cmd as we can't copy an image or use pipeline barriers in a render pass
    VKContext *context = reinterpret_cast<VKContext *>(state.context);
    vk::CommandBuffer cmd_buffer = context->prerender_cmd;

    delta_row_samples *= state.res_multiplier;
    delta_col_samples *= state.res_multiplier;

    read_only.depth_view.transition_to_discard(cmd_buffer, vkutil::ImageLayout::TransferDst, vkutil::ds_subresource_range);

    cached_info.texture.transition_to(cmd_buffer, vkutil::ImageLayout::TransferSrc, vkutil::ds_subresource_range);
    vk::ImageSubresourceLayers layers = vkutil::color_subresource_layer;
    layers.aspectMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    vk::ImageCopy image_copy{
        .srcSubresource = layers,
        .srcOffset = { static_cast<int>(delta_col_samples), static_cast<int>(delta_row_samples), 0 },
        .dstSubresource = layers,
        .dstOffset = { 0, 0, 0 },
        .extent = { std::min(width, cached_info.texture.width - delta_col_samples), std::min(height, cached_info.texture.height - delta_row_samples), 1U }
    };
    cmd_buffer.copyImage(cached_info.texture.image, vk::ImageLayout::eTransferSrcOptimal, read_only.depth_view.image, vk::ImageLayout::eTransferDstOptimal, image_copy);

    // transition back
    cached_info.texture.transition_to(cmd_buffer, vkutil::ImageLayout::DepthStencilReadOnly, vkutil::ds_subresource_range);
    read_only.depth_view.transition_to(cmd_buffer, vkutil::ImageLayout::SampledImage, vkutil::ds_subresource_range);

    return TextureLookupResult{
        img_view.view,
        img_view.layout,
        img_view.format
    };
}

static Framebuffer empty_framebuffer{};
Framebuffer &VKSurfaceCache::retrieve_framebuffer_handle(MemState &mem, SceGxmColorSurface *color, SceGxmDepthStencilSurface *depth_stencil,
    vk::RenderPass standard_render_pass, vk::RenderPass interlock_render_pass, vk::ImageView &color_view,
    vk::ImageView &color_storage_view, vk::ImageView &ds_view) {
    if (!target) {
        LOG_ERROR("Unable to retrieve framebuffer with no active render target!");
        return empty_framebuffer;
    }

    if (!color && !depth_stencil)
        LOG_ERROR_ONCE("Depth stencil and color surface are both null!");

    // might get modified by retrieve_color_surface_for_framebuffer
    state.pipeline_cache.can_use_deferred_compilation = true;

    // First retrieve separately the color surface and ds surface
    SurfaceRetrieveResult color_result;
    SurfaceRetrieveResult ds_result;

    if (color) {
        color_result = retrieve_color_surface_for_framebuffer(mem, color);
    } else {
        color_result.view = target->color.view;
        color_result.base_image = &target->color;
        color_result.storage_view = target->color.view;
    }

    if (depth_stencil) {
        ds_result = retrieve_depth_stencil_for_framebuffer(depth_stencil, target->width, target->height);
    } else {
        ds_result.view = target->depthstencil.view;
        ds_result.base_image = &target->depthstencil;
    }

    color_view = color_result.view;
    color_storage_view = color_result.storage_view ? color_result.storage_view : color_result.view;
    ds_view = ds_result.view;

    std::pair<vk::ImageView, vk::ImageView> key = { color_view, ds_view };
    auto it = framebuffer_array.find(key);

    if (it != framebuffer_array.end()) {
        // we already created a framebuffer for this pair
        return it->second;
    }

    // make the framebuffer as big as possible
    const uint32_t framebuffer_width = std::min(color_result.base_image->width, ds_result.base_image->width);
    const uint32_t framebuffer_height = std::min(color_result.base_image->height, ds_result.base_image->height);

    vk::FramebufferCreateInfo fb_info{
        .renderPass = standard_render_pass,
        .width = framebuffer_width,
        .height = framebuffer_height,
        .layers = 1
    };
    vk::ImageView attachments[] = { color_result.view, color_result.raw_image ? color_result.raw_image->view : ds_result.view, ds_result.view };
    fb_info.setAttachments(attachments);
    fb_info.attachmentCount = color_result.raw_image ? 3 : 2;
    vk::Framebuffer fb_standard = state.device.createFramebuffer(fb_info);

    vk::Framebuffer fb_interlock = nullptr;
    if (state.features.support_shader_interlock) {
        // we also need to create the framebuffer for shader interlock
        fb_info.renderPass = interlock_render_pass;
        fb_info.pAttachments = &attachments[2];
        fb_info.attachmentCount = 1;
        fb_interlock = state.device.createFramebuffer(fb_info);
    }

    return (framebuffer_array[key] = { fb_standard, fb_interlock, color_result.base_image, framebuffer_width, framebuffer_height, color_result.raw_image });
}

bool VKSurfaceCache::check_for_surface(MemState &mem, Address source_address, CallbackRequestFunction &callback, Address target_address) {
    if (!state.features.enable_memory_mapping || state.disable_surface_sync)
        return false;

    if (vector_utils::find_index(cpu_surfaces_changed, source_address) != -1) {
        // there is a transfer operation pending on this surface, just add the callback after and we are done
        state.request_queue.push(CallbackRequest{ new CallbackRequestFunction(std::move(callback)) });

        if (target_address)
            cpu_surfaces_changed.push_back(target_address);
        return true;
    }

    // for now, only look if the address matches exactly a color surface
    auto it = color_address_lookup.find(source_address);
    if (it == color_address_lookup.end())
        return false;

    auto &surface = *it->second;
    VKContext &context = *static_cast<VKContext *>(state.context);
    // if the frame is already rendered skip
    // Note: that's not the best behavior but it should be fine
    // also it prevents invalidated surfaces from causing issues
    if (surface.last_frame_rendered + MAX_FRAMES_RENDERING <= context.frame_timestamp)
        return false;

    if (surface.gpu_read_sync_only) {
        surface.gpu_read_sync_only = false;
        if (state.need_cpu_buffer_sync()) {
            const uint32_t guest_bpp = gxm::bits_per_pixel(surface.format) / 8;
            const renderer::texture::ReadbackRows rows{
                surface.stride_bytes,
                uint32_t(surface.original_width) * guest_bpp,
                surface.original_height
            };
            if (guest_bpp) {
                if (const auto span_size = rows.span_size())
                    state.request_queue.push(BufferSyncRequest{ surface.data.address(), *span_size, rows });
            }
        }
    }

    // we found something
    if (!*surface.need_surface_sync) {
        // first send the command to sync the surface with the GPU
        *surface.need_surface_sync = true;

        // we shouldn't have a command buffer being used, but just in case
        vk::CommandBuffer prev_cmd = context.render_cmd;

        // for the time being, just create a temp command buffer / fence
        // That's not the best approach but I guess it works
        vk::CommandBuffer surface_cmd = nullptr;
        vk::Fence fence = state.device.createFence({});
        ColorSurfaceCacheInfo *returned_info = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
            surface_cmd = vkutil::create_single_time_command(state.device, state.multithread_command_pool);

            context.render_cmd = surface_cmd;
            last_written_surface = &surface;
            returned_info = perform_surface_sync();
            context.render_cmd = prev_cmd;

            surface_cmd.end();
        }
        // submit this command
        vk::SubmitInfo submit_info{};
        submit_info.setCommandBuffers(surface_cmd);
        state.submit_general(submit_info, fence, "surface synchronization submission");

        // now we need to wait for the fence, then destroy it along with the command buffer
        // to prevent memory leaks
        CallbackRequestFunction vk_callback = [&state = this->state, fence, surface_cmd]() {
            auto result = state.device.waitForFences(fence, vk::True, std::numeric_limits<uint64_t>::max());
            if (result != vk::Result::eSuccess)
                LOG_ERROR("Could not wait for fences.");

            // destroy the objects
            state.device.destroyFence(fence);

            std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
            state.device.freeCommandBuffers(state.multithread_command_pool, surface_cmd);
        };
        state.request_queue.push(CallbackRequest{ new CallbackRequestFunction(std::move(vk_callback)) });

        if (returned_info)
            state.request_queue.push(PostSurfaceSyncRequest{ returned_info });
    }

    // now push the callback
    state.request_queue.push(CallbackRequest{ new CallbackRequestFunction(std::move(callback)) });

    if (target_address)
        cpu_surfaces_changed.push_back(target_address);

    return true;
}

bool VKSurfaceCache::submit_immediate_surface_sync(ColorSurfaceCacheInfo &surface) {
    VKContext &context = *static_cast<VKContext *>(state.context);
    const bool needed_before = *surface.need_surface_sync;
    const bool buffer_sync_before = surface.need_buffer_sync;
    const bool post_sync_before = surface.need_post_surface_sync;
    *surface.need_surface_sync = true;

    const uint32_t guest_bpp = gxm::bits_per_pixel(surface.format) / 8;
    if (state.need_cpu_buffer_sync() && guest_bpp) {
        const renderer::texture::ReadbackRows rows{
            surface.stride_bytes,
            uint32_t(surface.original_width) * guest_bpp,
            surface.original_height
        };
        if (const auto span_size = rows.span_size())
            state.buffer_trapping.access_buffer(surface.data.address(), *span_size, context.mem, true, true);
    }

    const vk::CommandBuffer previous_command = context.render_cmd;
    ColorSurfaceCacheInfo *const previous_surface = last_written_surface;

    vk::CommandBuffer surface_command = nullptr;
    ColorSurfaceCacheInfo *returned_info = nullptr;
    {
        const std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
        surface_command = vkutil::create_single_time_command(state.device, state.multithread_command_pool);
        context.render_cmd = surface_command;
        last_written_surface = &surface;
        returned_info = perform_surface_sync();
        context.render_cmd = previous_command;
        last_written_surface = previous_surface;
        surface_command.end();
    }

    if (!returned_info) {
        *surface.need_surface_sync = needed_before;
        surface.need_buffer_sync = buffer_sync_before;
        surface.need_post_surface_sync = post_sync_before;
        const std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
        state.device.freeCommandBuffers(state.multithread_command_pool, surface_command);
        return false;
    }

    const vk::Fence fence = state.device.createFence({});
    vk::SubmitInfo submit_info{};
    submit_info.setCommandBuffers(surface_command);
    state.submit_general(submit_info, fence, "GPU surface read synchronization");

    CallbackRequestFunction cleanup = [&state = this->state, fence, surface_command]() {
        const vk::Result result = state.device.waitForFences(fence, vk::True, std::numeric_limits<uint64_t>::max());
        if (result != vk::Result::eSuccess)
            LOG_ERROR("Could not wait for the GPU surface read fence.");
        state.device.destroyFence(fence);
        const std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
        state.device.freeCommandBuffers(state.multithread_command_pool, surface_command);
    };
    state.request_queue.push(CallbackRequest{ new CallbackRequestFunction(std::move(cleanup)) });
    return true;
}

bool VKSurfaceCache::sync_surface_for_gpu_read(Address address, uint32_t size) {
    if (!state.features.enable_memory_mapping || state.disable_surface_sync)
        return false;

    auto it = color_address_lookup.upper_bound(address);
    if (it == color_address_lookup.begin())
        return false;
    --it;

    if (it->first != address) {
        const bool contained = address >= it->first
            && static_cast<uint64_t>(address) + size <= static_cast<uint64_t>(it->first) + it->second->total_bytes;
        if (!contained && it != color_address_lookup.begin())
            --it;
    }

    ColorSurfaceCacheInfo &surface = *it->second;
    const bool contained = address >= it->first
        && static_cast<uint64_t>(address) + size <= static_cast<uint64_t>(it->first) + surface.total_bytes;
    if (!contained)
        return false;

    constexpr uint32_t minimum_read_size = KiB(16);
    if (it->first != address && size != 0 && size < minimum_read_size)
        return false;

    VKContext &context = *static_cast<VKContext *>(state.context);
    if (surface.last_frame_rendered + MAX_FRAMES_RENDERING <= context.frame_timestamp)
        return false;

    const bool direct_mapping = !format_need_additional_memory(surface.format)
        && !surface_is_repacked_u4u4u4u4(surface)
        && !surface_is_repacked_float(surface)
        && surface.swizzle.r == vk::ComponentSwizzle::eR;
    if (!direct_mapping)
        return false;

    const bool needed_before = *surface.need_surface_sync;
    const bool gpu_only_before = surface.gpu_read_sync_only;
    const bool barrier_before = surface.gpu_read_needs_barrier;
    if (!needed_before)
        surface.gpu_read_sync_only = true;
    surface.gpu_read_needs_barrier = true;

    const bool reads_current_surface = context.record.color_surface.data.address() == surface.data.address();
    if (reads_current_surface && context.scene_has_unsynced_color_draw) {
        if (context.in_renderpass)
            context.stop_render_pass();

        const uint32_t guest_bpp = gxm::bits_per_pixel(surface.format) / 8;
        if (state.need_cpu_buffer_sync() && guest_bpp) {
            const renderer::texture::ReadbackRows rows{
                surface.stride_bytes,
                uint32_t(surface.original_width) * guest_bpp,
                surface.original_height
            };
            if (const auto span_size = rows.span_size())
                state.buffer_trapping.access_buffer(surface.data.address(), *span_size, context.mem, true, true);
        }

        const bool buffer_sync_before = surface.need_buffer_sync;
        const bool post_sync_before = surface.need_post_surface_sync;
        ColorSurfaceCacheInfo *const previous_surface = last_written_surface;
        *surface.need_surface_sync = true;
        last_written_surface = &surface;
        ColorSurfaceCacheInfo *const returned_info = perform_surface_sync();
        last_written_surface = previous_surface;
        if (!returned_info) {
            *surface.need_surface_sync = needed_before;
            surface.gpu_read_sync_only = gpu_only_before;
            surface.gpu_read_needs_barrier = barrier_before;
            surface.need_buffer_sync = buffer_sync_before;
            surface.need_post_surface_sync = post_sync_before;
            return false;
        }

        context.scene_has_unsynced_color_draw = false;
        return true;
    }

    if (!needed_before || !barrier_before) {
        if (!submit_immediate_surface_sync(surface)) {
            surface.gpu_read_sync_only = gpu_only_before;
            surface.gpu_read_needs_barrier = barrier_before;
            return false;
        }
    }

    return true;
}

ColorSurfaceCacheInfo *VKSurfaceCache::perform_surface_sync() {
    // surface sync is supported only if memory mapping is enabled
    if (!state.features.enable_memory_mapping)
        return nullptr;

    if (last_written_surface == nullptr || !*last_written_surface->need_surface_sync)
        return nullptr;

    const uint32_t guest_bpp = gxm::bits_per_pixel(last_written_surface->format) / 8;
    const renderer::texture::ReadbackRows rows{ last_written_surface->stride_bytes,
        uint32_t(last_written_surface->original_width) * guest_bpp, last_written_surface->original_height };
    const auto readback_size = rows.span_size();
    VKContext *context = reinterpret_cast<VKContext *>(state.context);
    if (!guest_bpp || last_written_surface->stride_bytes % guest_bpp || !readback_size
        || !is_valid_addr_range_size(context->mem, last_written_surface->data.address(), *readback_size)) {
        LOG_ERROR("Invalid color surface readback dimensions");
        return nullptr;
    }
    if (context->record.color_surface.data.address() == last_written_surface->data.address()) {
        note_scene_draw_rect(context->draw_rect_x0, context->draw_rect_y0,
            context->draw_rect_x1, context->draw_rect_y1);
    }
    vk::CommandBuffer cmd_buffer = context->render_cmd;

    bool sync_from_raw = typeless_read_from_raw(*last_written_surface);
    if (sync_from_raw && state.res_multiplier != 1.0f) {
        const auto properties = state.physical_device.getFormatProperties(vk::Format::eR16G16B16A16Uint);
        const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
        sync_from_raw = (properties.optimalTilingFeatures & required) == required;
    }
    const vk::Format sync_format = sync_from_raw ? vk::Format::eR16G16B16A16Uint : last_written_surface->texture.format;
    vk::Image image_to_copy = sync_from_raw ? last_written_surface->raw_image->image : last_written_surface->texture.image;
    vk::ImageLayout image_layout = vk::ImageLayout::eGeneral;

    // this works for surface swizzles
    bool is_swizzle_identity = last_written_surface->swizzle.r == vk::ComponentSwizzle::eR;
    if (!is_swizzle_identity && !format_support_swizzle(last_written_surface->format)) {
        LOG_WARN_ONCE("Surface sync with swizzle not support on {}", vk::to_string(last_written_surface->texture.format));

        is_swizzle_identity = true;
    }

    const uint32_t pixel_stride = last_written_surface->stride_bytes / guest_bpp;
    const bool needs_copy_buffer = format_need_additional_memory(last_written_surface->format)
        || surface_is_repacked_u4u4u4u4(*last_written_surface)
        || surface_is_repacked_float(*last_written_surface);
    const bool can_clamp = state.surface_sync_clamp_rt && !sync_from_raw
        && !needs_copy_buffer && is_swizzle_identity;

    int32_t sync_x0 = 0;
    int32_t sync_y0 = 0;
    int32_t sync_x1 = last_written_surface->original_width;
    int32_t sync_y1 = last_written_surface->original_height;
    if (can_clamp) {
        if (last_written_surface->rendered_w > 0)
            sync_x1 = std::min(sync_x1, static_cast<int32_t>(last_written_surface->rendered_w));
        if (last_written_surface->rendered_h > 0)
            sync_y1 = std::min(sync_y1, static_cast<int32_t>(last_written_surface->rendered_h));

        sync_x0 = std::max(sync_x0, last_written_surface->written_x0);
        sync_y0 = std::max(sync_y0, last_written_surface->written_y0);
        sync_x1 = std::min(sync_x1, last_written_surface->written_x1);
        sync_y1 = std::min(sync_y1, last_written_surface->written_y1);
        if (sync_x1 <= sync_x0 || sync_y1 <= sync_y0)
            return nullptr;
    }

    const uint32_t sync_width = static_cast<uint32_t>(sync_x1 - sync_x0);
    const uint32_t sync_height = static_cast<uint32_t>(sync_y1 - sync_y0);
    const bool clamp_sync = sync_x0 != 0 || sync_y0 != 0
        || sync_width != last_written_surface->original_width
        || sync_height != last_written_surface->original_height;

    barrier_render_to_transfer_read(cmd_buffer, image_to_copy);

    if (state.res_multiplier != 1.0f) {
        // scale back the image using a blit command first

        if (!last_written_surface->blit_image)
            last_written_surface->blit_image = std::make_unique<vkutil::Image>();

        vkutil::Image &blit_image = *last_written_surface->blit_image;
        if (blit_image.image && blit_image.format != sync_format)
            state.frame().destroy_queue.add_image(blit_image);

        if (!blit_image.image) {
            blit_image.format = sync_format;
            blit_image.width = last_written_surface->original_width;
            blit_image.height = last_written_surface->original_height;

            blit_image.init_image(vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst);
            blit_image.transition_to(cmd_buffer, vkutil::ImageLayout::TransferDst);
        } else {
            blit_image.transition_to_discard(cmd_buffer, vkutil::ImageLayout::TransferDst);
        }

        int32_t src_x0 = 0;
        int32_t src_y0 = 0;
        int32_t src_x1 = last_written_surface->width;
        int32_t src_y1 = last_written_surface->height;
        if (clamp_sync) {
            src_x0 = static_cast<int32_t>(std::floor(sync_x0 * state.res_multiplier));
            src_y0 = static_cast<int32_t>(std::floor(sync_y0 * state.res_multiplier));
            src_x1 = static_cast<int32_t>(std::ceil(sync_x1 * state.res_multiplier));
            src_y1 = static_cast<int32_t>(std::ceil(sync_y1 * state.res_multiplier));
            src_x0 = std::clamp(src_x0, 0, static_cast<int32_t>(last_written_surface->width));
            src_y0 = std::clamp(src_y0, 0, static_cast<int32_t>(last_written_surface->height));
            src_x1 = std::clamp(src_x1, 0, static_cast<int32_t>(last_written_surface->width));
            src_y1 = std::clamp(src_y1, 0, static_cast<int32_t>(last_written_surface->height));
        }

        vk::ImageBlit blit{
            .srcSubresource = vkutil::color_subresource_layer,
            .srcOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ src_x0, src_y0, 0 }, vk::Offset3D{ src_x1, src_y1, 1 } },
            .dstSubresource = vkutil::color_subresource_layer,
            .dstOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ sync_x0, sync_y0, 0 }, vk::Offset3D{ sync_x1, sync_y1, 1 } },
        };
        // Apply nearest filter for the time being, linear might be better if we have no data in the texture tho
        cmd_buffer.blitImage(image_to_copy, image_layout, blit_image.image, vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);

        blit_image.transition_to(cmd_buffer, vkutil::ImageLayout::TransferSrc);
        image_to_copy = blit_image.image;
        image_layout = vk::ImageLayout::eTransferSrcOptimal;
    }

    vk::Buffer buffer;
    uint32_t offset;
    if (needs_copy_buffer) {
        if (!last_written_surface->copy_buffer)
            last_written_surface->copy_buffer = std::make_unique<vkutil::Buffer>();

        vkutil::Buffer &copy_buffer = *last_written_surface->copy_buffer;

        if (!copy_buffer.buffer) {
            // Size for the wider host format, before guest-format packing.
            copy_buffer.size = static_cast<vk::DeviceSize>(pixel_stride) * last_written_surface->original_height
                * vk::blockSize(last_written_surface->texture.format);
            copy_buffer.init_buffer(vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc_cached);
        }

        buffer = copy_buffer.buffer;
        offset = 0;

        last_written_surface->need_buffer_sync = false;
        last_written_surface->need_post_surface_sync = true;
    } else {
        const auto mapping = state.mapped_memories.lower_bound(last_written_surface->data.address());
        if (mapping == state.mapped_memories.end()
            || !renderer::texture::readback_range_fits(mapping->first, mapping->second.size, last_written_surface->data.address(), *readback_size)) {
            LOG_ERROR("Color surface readback is not fully mapped");
            return nullptr;
        }
        last_written_surface->need_buffer_sync = !last_written_surface->gpu_read_sync_only;
        last_written_surface->need_post_surface_sync = !is_swizzle_identity;
        std::tie(buffer, offset) = state.get_matching_mapping(last_written_surface->data);
    }
    vk::BufferImageCopy copy{
        .bufferOffset = offset,
        .bufferRowLength = pixel_stride,
        .bufferImageHeight = last_written_surface->original_height,
        .imageSubresource = vkutil::color_subresource_layer,
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { last_written_surface->original_width, last_written_surface->original_height, 1 }
    };
    if (clamp_sync) {
        copy.bufferOffset += static_cast<uint32_t>(sync_y0) * last_written_surface->stride_bytes
            + static_cast<uint32_t>(sync_x0) * guest_bpp;
        copy.imageOffset = vk::Offset3D{ sync_x0, sync_y0, 0 };
        copy.imageExtent = vk::Extent3D{ sync_width, sync_height, 1 };
    }
    cmd_buffer.copyImageToBuffer(image_to_copy, image_layout, buffer, copy);

    if (last_written_surface->gpu_read_needs_barrier) {
        const vk::BufferMemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = offset,
            .size = *readback_size
        };
        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader,
            {}, {}, barrier, {});
    }

    ColorSurfaceCacheInfo *return_value = last_written_surface;
    last_written_surface = nullptr;

    return return_value;
}

template <typename T>
static void swizzle_text_T_2(T *pixels, uint32_t nb_pixel) {
    for (uint32_t i = 0; i < nb_pixel; i++) {
        std::swap(pixels[2 * i], pixels[2 * i + 1]);
    }
}

template <typename T, size_t type>
static void swizzle_text_T_4(T *pixels, uint32_t nb_pixel) {
    for (uint32_t i = 0; i < nb_pixel; i++) {
        if constexpr (type == 0) {
            // BGRA
            std::swap(pixels[4 * i], pixels[4 * i + 2]);
        } else if constexpr (type == 1) {
            // ABGR
            std::swap(pixels[4 * i], pixels[4 * i + 3]);
            std::swap(pixels[4 * i + 1], pixels[4 * i + 2]);
        } else {
            // ARGB
            T copy[] = { pixels[4 * i],
                pixels[4 * i + 1],
                pixels[4 * i + 2],
                pixels[4 * i + 3] };
            pixels[4 * i] = copy[3];
            pixels[4 * i + 1] = copy[0];
            pixels[4 * i + 2] = copy[1];
            pixels[4 * i + 3] = copy[2];
        }
    }
}

template <typename T>
static void swizzle_text_T(T *pixels, uint32_t nb_pixel, ColorSurfaceCacheInfo *surface) {
    // there can only be 2 or 4 component textures here
    if (vk::componentCount(surface->texture.format) == 2) {
        swizzle_text_T_2<T>(pixels, nb_pixel);
    } else {
        // find the swizzle
        // swizzles are inversed
        switch (surface->swizzle.r) {
        case vk::ComponentSwizzle::eB:
            // BGRA
            swizzle_text_T_4<T, 0>(pixels, nb_pixel);
            break;
        case vk::ComponentSwizzle::eA:
            // ABGR
            swizzle_text_T_4<T, 1>(pixels, nb_pixel);
            break;
        case vk::ComponentSwizzle::eG:
            // ARGB
            swizzle_text_T_4<T, 2>(pixels, nb_pixel);
            break;
        }
    }
}

void VKSurfaceCache::perform_post_surface_sync(const MemState &mem, ColorSurfaceCacheInfo *surface) {
    if (surface == nullptr)
        return;

    const uint32_t pixel_stride = (surface->stride_bytes * 8) / gxm::bits_per_pixel(surface->format);
    uint8_t *pixels = surface->data.cast<uint8_t>().get(mem);

    if (surface_is_repacked_float(*surface)) {
        surface->copy_buffer->invalidate(0, VK_WHOLE_SIZE);
        pack_float_surface(pixels, static_cast<const uint8_t *>(surface->copy_buffer->mapped_data), *surface);
        return;
    }

    if (surface_is_repacked_u4u4u4u4(*surface)) {
        surface->copy_buffer->invalidate(0, VK_WHOLE_SIZE);
        pack_rgba8_to_r4g4b4a4(pixels, static_cast<const uint8_t *>(surface->copy_buffer->mapped_data), pixel_stride, surface->original_width, surface->original_height, surface->swizzle);
        return;
    }

    if (format_need_additional_memory(surface->format)) {
        surface->copy_buffer->invalidate(0, VK_WHOLE_SIZE);

        // special case, use a custom function
        const bool is_swizzle_identity = surface->swizzle.r == vk::ComponentSwizzle::eR;
        if (!surface->sws_context) {
            const AVPixelFormat dst_fmt = is_swizzle_identity ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_BGR24;
            surface->sws_context = sws_getContext(surface->original_width, surface->original_height, AV_PIX_FMT_RGB0, surface->original_width, surface->original_height, dst_fmt, 0, nullptr, nullptr, nullptr);
            assert(surface->sws_context != NULL);
        }

        int src_stride = pixel_stride * 4;
        int dst_stride = pixel_stride * 3;
        sws_scale(surface->sws_context, reinterpret_cast<const uint8_t *const *>(&surface->copy_buffer->mapped_data), &src_stride, 0, surface->original_height, &pixels, &dst_stride);
        return;
    }

    for (uint32_t row = 0; row < surface->original_height; ++row) {
        auto *row_pixels = pixels + size_t(row) * surface->stride_bytes;
        switch (vk::componentBits(surface->texture.format, 0)) {
        case 8:
            swizzle_text_T<uint8_t>(row_pixels, surface->original_width, surface);
            break;
        case 16:
            swizzle_text_T<uint16_t>(reinterpret_cast<uint16_t *>(row_pixels), surface->original_width, surface);
            break;
        case 32:
            swizzle_text_T<uint32_t>(reinterpret_cast<uint32_t *>(row_pixels), surface->original_width, surface);
            break;
        }
    }
}

void VKSurfaceCache::destroy_associated_framebuffers(const VKRenderTarget *render_target) {
    if (!render_target)
        return;

    destroy_framebuffers(render_target->color.view);
    destroy_framebuffers(render_target->depthstencil.view);
}

vk::ImageView VKSurfaceCache::sourcing_color_surface_for_presentation(Ptr<const void> address, uint32_t pitch, Viewport &viewport) {
    // get closest surface with an address below address
    auto ite = color_address_lookup.upper_bound(address.address());
    if (ite == color_address_lookup.begin()) {
        return nullptr;
    }
    --ite;

    ColorSurfaceCacheInfo &info = *ite->second;
    if (info.data.address() + info.total_bytes <= address.address())
        // they do not overlap
        return nullptr;

    if (info.stride_bytes == pitch * 4) {
        // In assumption the format is RGBA8
        const size_t data_delta = address.address() - ite->first;
        uint32_t limited_height = viewport.height;
        if ((data_delta % (pitch * 4)) == 0) {
            uint32_t start_sourced_line = static_cast<uint32_t>((data_delta / (pitch * 4)) * state.res_multiplier);
            if ((start_sourced_line + viewport.height) > info.height) {
                // Sometimes the surface is just missing a little bit of lines
                if (start_sourced_line < info.height) {
                    // Just limit the height and display it
                    limited_height = info.height - start_sourced_line;
                } else {
                    LOG_ERROR("Trying to present non-existent segment in cached color surface!");
                    return nullptr;
                }
            }

            // Compute position in texture
            viewport.offset_x = 0;
            viewport.offset_y = start_sourced_line;
            viewport.width = std::min(viewport.width, static_cast<uint32_t>(info.width));
            viewport.height = limited_height;
            viewport.texture_width = info.width;
            viewport.texture_height = info.height;

            if (info.swizzle == vkutil::rgba_mapping && info.texture.format == vk::Format::eR8G8B8A8Unorm)
                return info.texture.view;

            if (!info.alternate_view) {
                // create a view with the right swizzle and without gamma correction
                vk::ImageViewCreateInfo view_info{
                    .image = info.texture.image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = vk::Format::eR8G8B8A8Unorm,
                    .components = vkutil::color_to_texture_swizzle(info.swizzle, vkutil::rgba_mapping),
                    .subresourceRange = vkutil::color_subresource_range
                };
                info.alternate_view = state.device.createImageView(view_info);
            }

            return info.alternate_view;
        }
    }

    return nullptr;
}

std::vector<uint32_t> VKSurfaceCache::dump_frame(Ptr<const void> address, uint32_t width, uint32_t height, uint32_t pitch) {
    // get closest surface with an address below address
    auto ite = color_address_lookup.upper_bound(address.address());
    if (ite == color_address_lookup.begin()) {
        return {};
    }
    --ite;

    const ColorSurfaceCacheInfo &info = *ite->second;

    const uint32_t data_delta = address.address() - ite->first;
    const uint32_t pitch_byte = pitch * 4;
    if (info.stride_bytes != pitch_byte || data_delta % pitch_byte != 0)
        return {};

    const uint32_t line_delta = static_cast<uint32_t>((data_delta / pitch_byte) * state.res_multiplier);
    if (line_delta >= info.height)
        return {};

    const uint32_t real_height = std::min(height, info.height - line_delta);

    std::vector<uint32_t> frame(width * height, 0);

    // we need a temporary buffer and command buffer for this
    // this is a raii buffer, it will be destroyed at the end of this function
    vkutil::Buffer temp_buff(width * height * 4);
    temp_buff.init_buffer(vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);
    auto one_time_command = state.create_one_time_command();
    const vk::CommandBuffer cmd_buffer = one_time_command.buffer;

    // layout is general, we can directly copy from it
    vk::BufferImageCopy image_copy{
        .bufferOffset = 0,
        .bufferRowLength = width,
        .bufferImageHeight = height,
        .imageSubresource = vkutil::color_subresource_layer,
        .imageOffset = { 0, static_cast<int>(line_delta), 0 },
        .imageExtent = { width, real_height, 1 }
    };
    cmd_buffer.copyImageToBuffer(info.texture.image, vk::ImageLayout::eGeneral, temp_buff.buffer, image_copy);

    // this will cause a waitIdle, not an issue
    state.submit_one_time_command(std::move(one_time_command));

    memcpy(frame.data(), temp_buff.mapped_data, frame.size() * 4);

    return frame;
}

} // namespace renderer::vulkan
