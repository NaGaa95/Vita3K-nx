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

#include <renderer/vulkan/types.h>

#include <renderer/vulkan/functions.h>
#include <renderer/vulkan/gxm_to_vulkan.h>
#include <renderer/vulkan/state.h>

#include <gxm/functions.h>
#include <mem/functions.h>
#include <renderer/functions.h>
#include <shader/spirv_recompiler.h>

#include <util/log.h>
#ifdef __SWITCH__
extern "C" void armDCacheFlush(void *addr, size_t size);
#endif
#include <util/overloaded.h>
#include <util/switch_thread.h>

#include <algorithm>

namespace renderer::vulkan {

#ifdef __SWITCH__
static void copy_readback_to_guest(const MemState &mem, Address address, const uint8_t *source,
    uint32_t size, const renderer::texture::ReadbackRows &rows) {
    const uint32_t row_count = rows.count ? rows.count : 1;
    const uint32_t row_stride = rows.count ? rows.stride : size;
    const uint32_t row_bytes = rows.count ? rows.row_bytes : size;
    for (uint32_t row = 0; row < row_count; ++row) {
        const uint32_t row_offset = row * row_stride;
        uint32_t copied = 0;
        while (copied < row_bytes) {
            const Address destination = address + row_offset + copied;
            const uint32_t bytes = std::min<uint32_t>(row_bytes - copied, KiB(4) - destination % KiB(4));
            memcpy(Ptr<uint8_t>(destination).get(mem), source + row_offset + copied, bytes);
            copied += bytes;
        }
    }
}
#endif

void VKContext::wait_thread_function(const MemState &mem) {
    switch_pin_to_hardware_priority("Vulkan wait thread", 45);

    // try to wait for multiple fences at the same time if possible
    std::vector<vk::Fence> fences;

    auto wait_for_fences = [&]() {
        while (!fences.empty()) {
            if (state.device_lost.load(std::memory_order_acquire)) {
                fences.clear();
                return;
            }
            // timeout so we can check for shutdown
            vk::Result result;
            try {
                result = state.device.waitForFences(fences, VK_TRUE, 100'000'000ULL);
            } catch (const vk::SystemError &) {
                result = vk::Result::eErrorDeviceLost;
            }
            if (result == vk::Result::eSuccess) {
                // don't reset them
                fences.clear();
                return;
            }
            if (result == vk::Result::eTimeout) {
                if (state.request_queue.is_aborted()) {
                    fences.clear();
                    return;
                }
                continue;
            }
            // Device lost: no fence will ever signal again. Keep this thread
            // alive so queued requests still complete and unblock the guest;
            // the frontend closes the session once it sees the flag.
            LOG_CRITICAL("Vulkan wait thread: the device was lost.");
            state.device_lost.store(true, std::memory_order_release);
            fences.clear();
            return;
        }
    };

    while (true) {
        auto wait_request = state.request_queue.pop();

        if (!wait_request)
            break;

        std::visit(overloaded{
                       [&](FenceWaitRequest &request) {
                           fences.push_back(request.fence);
                       },
                       [&](NotificationRequest &request) {
                           if (request.notifications[0].address || request.notifications[1].address) {
                               wait_for_fences();

                               // same as in handle_sync_surface_data
                               std::unique_lock<std::mutex> lock(state.notification_mutex);

                               if (request.notifications[0].address)
                                   *request.notifications[0].address.get(mem) = request.notifications[0].value;
                               if (request.notifications[1].address)
                                   *request.notifications[1].address.get(mem) = request.notifications[1].value;

                               // unlocking before a notify should be faster
                               lock.unlock();
                               state.notification_ready.notify_all();
                           }
                       },
                       [&](FrameDoneRequest &request) {
                           wait_for_fences();

                           // don't reset them, the reset will be done in the new_frame function
                           // and these fences can still be waited for during texture uploading
                           std::unique_lock<std::mutex> lock(new_frame_mutex);
                           last_frame_waited = request.frame_timestamp;
                           lock.unlock();
                           new_frame_condv.notify_one();
                       },
                       [&](BufferSyncRequest &request) {
                           wait_for_fences();
                           const auto span = request.rows.count ? request.rows.span_size() : std::optional<uint32_t>(request.size);
                           if (!span || *span != request.size || !is_valid_addr_range_size(mem, request.location, request.size)) {
                               LOG_ERROR("Invalid surface readback range");
                               return;
                           }
                           auto mem_it = state.mapped_memories.lower_bound(request.location);
                           if (mem_it == state.mapped_memories.end()
                               || !renderer::texture::readback_range_fits(mem_it->first, mem_it->second.size, request.location, request.size)) {
                               LOG_ERROR("Surface readback range is not fully mapped");
                               return;
                           }
#ifdef __SWITCH__
                           if (std::holds_alternative<ExternalBuffer>(mem_it->second.buffer_impl)) {
                               // The GPU wrote guest memory directly; drop
                               // the stale CPU cache lines.
                               armDCacheFlush(Ptr<void>(request.location).get(mem), request.size);
                               return;
                           }
#endif
                           auto *mapped_buffer = std::get_if<vkutil::Buffer>(&mem_it->second.buffer_impl);
                           const uint64_t buffer_offset = uint64_t(mem_it->second.buffer_offset) + request.location - mem_it->first;
                           if (!mapped_buffer || !mapped_buffer->mapped_data
                               || !renderer::texture::readback_range_fits(0, mapped_buffer->size, buffer_offset, request.size)) {
                               LOG_ERROR("Surface readback exceeds its host buffer");
                               return;
                           }
                           mapped_buffer->invalidate(buffer_offset, request.size);
                           const auto *src = static_cast<const uint8_t *>(mapped_buffer->mapped_data) + buffer_offset;
#ifdef __SWITCH__
                           copy_readback_to_guest(mem, request.location, src, request.size, request.rows);
#else
                           auto *dst = Ptr<uint8_t>(request.location).get(mem);
                           if (request.rows.count)
                               renderer::texture::copy_readback_rows({ dst, request.size }, { src, request.size }, request.rows);
                           else
                               memcpy(dst, src, request.size);
#endif
                       },
                       [&](PostSurfaceSyncRequest &request) {
                           wait_for_fences();

                           state.surface_cache.perform_post_surface_sync(mem, request.cache_info);
                       },
                       [&](SyncSignalRequest &request) {
                           wait_for_fences();

                           renderer::subject_done(request.sync, request.timestamp);
                       },
                       [&](CallbackRequest &request) {
                           if (request.wait_for_gpu)
                               wait_for_fences();
                           if (request.callback) {
                               (*request.callback)();
                               delete request.callback;
                           }
                       } },
            *wait_request);
    }
}

void set_context(VKContext &context, MemState &mem, VKRenderTarget *rt, const FeatureState &features) {
    context.state.surface_cache.resolve_ds_scene_end(context.scene_wrote_depth);
    context.scene_wrote_depth = false;
    context.scene_has_unsynced_color_draw = false;
    context.draw_rect_x0 = INT32_MAX;
    context.draw_rect_y0 = INT32_MAX;
    context.draw_rect_x1 = 0;
    context.draw_rect_y1 = 0;

    context.render_target = rt;
    context.scene_timestamp++;
    context.gxm_scene_id++;
    context.state.texture_cache.current_scene_timestamp = context.scene_timestamp;
#ifdef __SWITCH__
    context.state.texture_cache.memo_scene = context.scene_timestamp;
#endif

    SceGxmColorSurface *color_surface_fin = &context.record.color_surface;
    // set these values for the pipeline cache
    context.record.color_base_format = gxm::get_base_format(color_surface_fin->colorFormat);
    context.record.is_gamma_corrected = static_cast<bool>(color_surface_fin->gamma);
    vk::Format vk_format = color::translate_surface_format(context.record.color_base_format,
        context.state.support_native_rgba4_surface, features.support_shader_interlock);

    if (color_surface_fin->gamma && vk_format == vk::Format::eR8G8B8A8Unorm) {
        vk_format = vk::Format::eR8G8B8A8Srgb;
    }

    if (color_surface_fin->data.address() == 0) {
        color_surface_fin = nullptr;

        // set back default values
        vk_format = vk::Format::eR8G8B8A8Unorm;
        context.record.color_surface.downscale = static_cast<bool>(rt->multisample_mode);
        context.record.is_gamma_corrected = false;
        context.record.is_maskupdate = false;
        context.record.color_base_format = SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8;
    }
    context.current_color_format = vk_format;

    rt->width = rt->base_width;
    rt->height = rt->base_height;
    if (rt->multisample_mode && !context.record.color_surface.downscale) {
        // using MSAA without downscaling, emulate this as best as we can by multiplying the width and height of the render target by 2
        rt->width *= 2;
        rt->height *= 2;
    }

    // Downscaled surfaces use half the target extent, viewport and clip coordinates.
    const float previous_downscale = context.surface_downscale;
    context.surface_downscale = 1.0f;
    if (color_surface_fin != nullptr && context.record.color_surface.downscale
        && color_surface_fin->width > 0 && color_surface_fin->height > 0) {
        const uint32_t color_width = static_cast<uint32_t>(color_surface_fin->width * context.state.res_multiplier);
        const uint32_t color_height = static_cast<uint32_t>(color_surface_fin->height * context.state.res_multiplier);
        if (color_width > 0 && color_height > 0
            && rt->base_width >= color_width * 2 && rt->base_height >= color_height * 2) {
            context.surface_downscale = 0.5f;
            rt->width /= 2;
            rt->height /= 2;
        }
    }
    if (context.surface_downscale != previous_downscale)
        refresh_viewport_and_clipping(context);

    SceGxmDepthStencilSurface *ds_surface_fin = &context.record.depth_stencil_surface;
    if (!ds_surface_fin->depth_data && !ds_surface_fin->stencil_data) {
        ds_surface_fin = nullptr;
    }

    VKState &state = context.state;
    state.surface_cache.set_render_target(rt);

    context.start_recording(true);

    if (features.use_mask_bit) {
        const bool initialized = rt->mask.layout != vkutil::ImageLayout::Undefined;
        vk::ImageMemoryBarrier mask_barrier{
            .srcAccessMask = initialized ? vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = initialized ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rt->mask.image,
            .subresourceRange = vkutil::color_subresource_range
        };
        context.render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, mask_barrier);
        const float initial_mask = context.record.depth_stencil_surface.mask ? 1.0f : 0.0f;
        rt->mask_may_reject = initial_mask == 0.0f;
        const vk::ClearColorValue mask_clear{
            .float32 = std::array<float, 4>{ initial_mask, initial_mask, initial_mask, initial_mask }
        };
        context.render_cmd.clearColorImage(rt->mask.image, vk::ImageLayout::eTransferDstOptimal, mask_clear, vkutil::color_subresource_range);
        mask_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        mask_barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
        mask_barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        mask_barrier.newLayout = vk::ImageLayout::eGeneral;
        context.render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, mask_barrier);
        rt->mask.layout = vkutil::ImageLayout::StorageImage;
    }

    bool force_load = context.record.depth_stencil_surface.force_load;
    bool force_store = context.record.depth_stencil_surface.force_store;
    if (ds_surface_fin == nullptr) {
        // no memory backing the depth stencil
        force_load = false;
        force_store = false;
    }
    // GXM force_store controls guest writeback, not render-pass preservation.
    force_store = true;

    bool depth_load = force_load;
    const bool stencil_load = force_load;
    if (ds_surface_fin != nullptr) {
        const bool game_stores = context.record.depth_stencil_surface.force_store;
        const bool depth_content_valid = state.surface_cache.begin_ds_scene_depth_check(
            *ds_surface_fin, game_stores, context.record.color_surface.data.address());
        if (game_stores && !depth_content_valid)
            depth_load = false;
    }
    const bool color_has_raw = state.surface_cache.color_surface_has_raw_alias(context.record.color_surface.data.address());
    context.current_render_pass = context.state.pipeline_cache.retrieve_render_pass(vk_format, depth_load, stencil_load, force_store, color_surface_fin == nullptr, false, color_has_raw);
    context.initial_render_pass = context.current_render_pass;
    context.initial_depth_stencil_clear = (depth_load ? vk::ImageAspectFlags() : vk::ImageAspectFlagBits::eDepth)
        | (stencil_load ? vk::ImageAspectFlags() : vk::ImageAspectFlagBits::eStencil);
    if (context.state.features.support_shader_interlock)
        // also retrieve / create the shader interlock pass
        context.current_shader_interlock_pass = context.state.pipeline_cache.retrieve_render_pass(vk_format, true, true, true, color_surface_fin == nullptr, true);

    Framebuffer &framebuffer = state.surface_cache.retrieve_framebuffer_handle(mem, color_surface_fin, ds_surface_fin, context.current_render_pass,
        context.current_shader_interlock_pass, context.current_color_view, context.current_color_storage_view, context.current_ds_view);
    context.current_framebuffer = framebuffer.standard;
    context.current_shader_interlock_framebuffer = framebuffer.shader_interlock;
    context.current_color_base_image = framebuffer.base_image;
    context.current_fb_width = framebuffer.width;
    context.current_fb_height = framebuffer.height;
    context.current_color_raw_view = framebuffer.raw_image ? framebuffer.raw_image->view : state.default_raw_image.view;

    // make sure we are not keeping any texture from the previous pass
    // (textures can be still bound even though they are not used)
    context.last_vert_texture_count = ~0;
    context.last_frag_texture_count = ~0;
    for (int i = 0; i < 16; i++) {
        context.vertex_textures[i].sampler = nullptr;
        context.fragment_textures[i].sampler = nullptr;
    }

    context.is_first_scene_draw = true;
    context.initialized_macroblocks = 0;
    context.last_macroblock_x = ~0;
    context.last_macroblock_y = ~0;
    context.ignore_macroblock = false;
}

void VKContext::start_recording(bool first_in_scene) {
    if (is_recording) {
        LOG_ERROR("Attempt to start recording while already recording");
        return;
    }

    if (render_target == nullptr) {
        LOG_ERROR("Recording started without a set command buffer");
        return;
    }

    if (render_target->last_used_frame != frame_timestamp) {
        // reset idx if we are in a new frame
        render_target->cmd_buffer_idx = 0;
        render_target->last_used_frame = frame_timestamp;
    }

    // safety check
    if (render_target->cmd_buffer_idx == render_target->cmd_buffers[state.current_frame_idx].size()) {
        LOG_WARN_ONCE("Render Target is using more scenes per frame than what was planned!");

        // add additional cmd buffers, fences and semaphores
        vk::CommandBufferAllocateInfo cmd_buffer_info{
            .commandPool = state.frame().render_pool,
            .commandBufferCount = 1
        };
        render_target->cmd_buffers[state.current_frame_idx].push_back(state.device.allocateCommandBuffers(cmd_buffer_info)[0]);

        cmd_buffer_info.commandPool = state.frame().prerender_pool;
        render_target->pre_cmd_buffers[state.current_frame_idx].push_back(state.device.allocateCommandBuffers(cmd_buffer_info)[0]);

        // we only use one fence per scene anyway
        vk::FenceCreateInfo fence_info{};
        // make sure the next fence used is the one we created (but only if this is the first recording of the scene)
        auto fence_insert_it = render_target->fences.begin() + render_target->fence_idx;
        if (!first_in_scene)
            fence_insert_it++;
        render_target->fences.insert(fence_insert_it, state.device.createFence(fence_info));
    }

    if (next_fence == nullptr) {
        next_fence = render_target->fences[render_target->fence_idx];
        // only increase the fence index if we used the previous one
        render_target->fence_idx = (render_target->fence_idx + 1) % render_target->fences.size();
    }

    render_cmd = render_target->cmd_buffers[state.current_frame_idx][render_target->cmd_buffer_idx];
    prerender_cmd = render_target->pre_cmd_buffers[state.current_frame_idx][render_target->cmd_buffer_idx];
    render_target->cmd_buffer_idx++;

    vk::CommandBufferBeginInfo begin_info{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
    };
    render_cmd.begin(begin_info);
    prerender_cmd.begin(begin_info);

    is_recording = true;

    // set all the dynamic state here
    render_cmd.setViewport(0, viewport);
    render_cmd.setScissor(0, scissor);
    sync_depth_bias(*this);
    sync_point_line_width(*this, true);
    sync_stencil_func(*this, false);
    if (record.two_sided == SCE_GXM_TWO_SIDED_ENABLED) {
        sync_stencil_func(*this, true);
    }
}

// we only need one descriptor per scene, so this does not need to be too big
static constexpr uint32_t DESCRIPTOR_PACK_SIZE = 16;

static vk::DescriptorSet retrieve_color_descriptor(VKState &state, FrameDescriptor &frame_descriptor) {
    if (frame_descriptor.descriptors_idx < frame_descriptor.sets.size())
        return frame_descriptor.sets[frame_descriptor.descriptors_idx++];

    uint32_t storage_images_per_set = state.features.support_shader_interlock ? 1 : 0;
    storage_images_per_set += state.features.use_mask_bit ? 1 : 0;
    storage_images_per_set += state.features.preserve_f16_nan_as_u16 ? 1 : 0;

    std::vector<vk::DescriptorPoolSize> pool_sizes;
    if (!state.features.support_shader_interlock)
        pool_sizes.push_back({ vk::DescriptorType::eInputAttachment, DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING });
    if (storage_images_per_set)
        pool_sizes.push_back({ vk::DescriptorType::eStorageImage, storage_images_per_set * DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING });
    if (state.features.use_mask_bit && state.features.support_shader_interlock)
        pool_sizes.push_back({ vk::DescriptorType::eInputAttachment, DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING });

    vk::DescriptorPoolCreateInfo descriptor_pool_info{
        .maxSets = DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING
    };
    descriptor_pool_info.setPoolSizes(pool_sizes);

    vk::DescriptorPool descriptor_pool = state.device.createDescriptorPool(descriptor_pool_info);
    state.frame_descriptor_pools.push_back(descriptor_pool);

    // allocate all the descriptor sets
    const vk::DescriptorSetLayout set_layout = state.pipeline_cache.attachments_layout;
    std::vector<vk::DescriptorSetLayout> layouts(DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING, set_layout);
    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool
    };
    descr_set_info.setSetLayouts(layouts);
    auto descriptor_sets = state.device.allocateDescriptorSets(descr_set_info);

    // distribute them among all frames
    for (int frame_idx = 0; frame_idx < MAX_FRAMES_RENDERING; frame_idx++) {
        FrameDescriptor &frame_descr = state.frames[frame_idx].color_descriptor;

        // insert DESCRIPTOR_PACK_SIZE in each frame descriptor
        auto descr_it = descriptor_sets.begin() + frame_idx * DESCRIPTOR_PACK_SIZE;
        frame_descr.sets.insert(frame_descr.sets.end(), descr_it, descr_it + DESCRIPTOR_PACK_SIZE);
    }

    return frame_descriptor.sets[frame_descriptor.descriptors_idx++];
}

void VKContext::start_render_pass(bool create_descriptor_set, bool maskupdate) {
    if (in_renderpass) {
        LOG_ERROR("Starting render pass while already in render pass");
        return;
    }

    if (!is_recording)
        start_recording();

    vk::ImageAspectFlags macroblock_clear;
    if (!maskupdate && state.features.use_mask_bit && render_target->has_macroblock_sync && !ignore_macroblock) {
        // Only the first pass may discard the attachment; later new blocks need a local clear.
        const uint16_t macroblock_bit = 1U << (last_macroblock_x + 4 * last_macroblock_y);
        is_first_scene_draw = (initialized_macroblocks & macroblock_bit) == 0;
        const bool first_macroblock = initialized_macroblocks == 0;
        current_render_pass = first_macroblock ? initial_render_pass : state.pipeline_cache.retrieve_render_pass(current_color_format, true, true, true, !record.color_surface.data, false, state.surface_cache.color_surface_has_raw_alias(record.color_surface.data.address()));
        if (is_first_scene_draw && !first_macroblock)
            macroblock_clear = initial_depth_stencil_clear;
        initialized_macroblocks |= macroblock_bit;
    }

    curr_renderpass_info = vk::RenderPassBeginInfo{
        .renderPass = maskupdate ? state.pipeline_cache.retrieve_mask_render_pass() : current_render_pass,
        .framebuffer = maskupdate ? render_target->mask_framebuffer : current_framebuffer
    };

    if (render_target->has_macroblock_sync && !ignore_macroblock) {
        // set the render area to the correct macroblock
        curr_renderpass_info.renderArea = vk::Rect2D{
            .offset = {
                last_macroblock_x * render_target->macroblock_width,
                last_macroblock_y * render_target->macroblock_height },
            .extent = { render_target->macroblock_width, render_target->macroblock_height }
        };
    } else {
        curr_renderpass_info.renderArea = vk::Rect2D{
            .offset = { 0, 0 },
            .extent = { render_target->width, render_target->height }
        };
    }

    // Zero width means no framebuffer; it can otherwise be smaller than the target.
    if (current_fb_width != 0) {
        const uint32_t offset_x = static_cast<uint32_t>(curr_renderpass_info.renderArea.offset.x);
        const uint32_t offset_y = static_cast<uint32_t>(curr_renderpass_info.renderArea.offset.y);
        const uint32_t max_width = current_fb_width - std::min(current_fb_width, offset_x);
        const uint32_t max_height = current_fb_height - std::min(current_fb_height, offset_y);
        curr_renderpass_info.renderArea.extent.width = std::min(curr_renderpass_info.renderArea.extent.width, max_width);
        curr_renderpass_info.renderArea.extent.height = std::min(curr_renderpass_info.renderArea.extent.height, max_height);
    }

    // The raw color alias shifts the depth-stencil attachment from index 1 to 2.
    std::array<vk::ClearValue, 3> curr_clear_values{};
    const vk::ClearDepthStencilValue ds_clear{
        .depth = record.depth_stencil_surface.background_depth,
        .stencil = record.depth_stencil_surface.stencil
    };
    curr_clear_values[1].depthStencil = ds_clear;
    curr_clear_values[2].depthStencil = ds_clear;
    if (!maskupdate)
        curr_renderpass_info.setClearValues(curr_clear_values);
    render_cmd.beginRenderPass(curr_renderpass_info, vk::SubpassContents::eInline);
    if (macroblock_clear && curr_renderpass_info.renderArea.extent.width && curr_renderpass_info.renderArea.extent.height) {
        const vk::ClearAttachment clear{ .aspectMask = macroblock_clear, .clearValue = curr_clear_values[1] };
        const vk::ClearRect rect{ .rect = curr_renderpass_info.renderArea, .layerCount = 1 };
        render_cmd.clearAttachments(clear, rect);
    }

    // set the renderpass info ready in case we need to switch between classic and framebuffer fetch usage
    curr_renderpass_info.setClearValues(nullptr);
    last_draw_was_framebuffer_fetch = false;

    refresh_pipeline = true;
    current_pipeline = nullptr;
    in_renderpass = true;
    in_maskupdate_pass = maskupdate;

    if (!create_descriptor_set)
        return;

    // update the rendertarget descriptor set
    rendertarget_set = retrieve_color_descriptor(state, state.frame().color_descriptor);

    // update descriptor set for the whole scene with the color attachment
    vk::DescriptorImageInfo descr_color_info{
        .sampler = nullptr,
        .imageView = state.features.support_shader_interlock ? current_color_storage_view : current_color_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };

    const vk::DescriptorType input_type = state.features.support_shader_interlock ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eInputAttachment;
    vk::WriteDescriptorSet write_descr{
        .dstSet = rendertarget_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = input_type,
    };
    write_descr.setImageInfo(descr_color_info);

    if (state.features.preserve_f16_nan_as_u16) {
        const vk::DescriptorImageInfo descr_raw_info{
            .sampler = nullptr,
            .imageView = current_color_raw_view ? current_color_raw_view : state.default_raw_image.view,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        vk::WriteDescriptorSet write_raw_descr{
            .dstSet = rendertarget_set,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorType = vk::DescriptorType::eStorageImage,
        };
        write_raw_descr.setImageInfo(descr_raw_info);
        const std::array<vk::WriteDescriptorSet, 2> writes = { write_descr, write_raw_descr };
        state.device.updateDescriptorSets(writes, {});
    } else {
        state.device.updateDescriptorSets(write_descr, {});
    }

    if (state.features.use_mask_bit) {
        vk::DescriptorImageInfo mask_info{
            .imageView = render_target->mask.view,
            .imageLayout = vk::ImageLayout::eGeneral
        };
        write_descr.dstBinding = 1;
        write_descr.descriptorType = vk::DescriptorType::eStorageImage;
        write_descr.setImageInfo(mask_info);
        state.device.updateDescriptorSets(write_descr, {});
        if (state.features.support_shader_interlock) {
            const vk::DescriptorImageInfo input_info{
                .imageView = current_color_view,
                .imageLayout = vk::ImageLayout::eGeneral
            };
            write_descr.dstBinding = 3;
            write_descr.descriptorType = vk::DescriptorType::eInputAttachment;
            write_descr.setImageInfo(input_info);
            state.device.updateDescriptorSets(write_descr, {});
        }
    }
}

void VKContext::stop_render_pass() {
    if (!in_renderpass) {
        LOG_ERROR("Stopping render pass while not in render pass");
        return;
    }

    // do this before ending the render pass
    end_visibility_query();

    render_cmd.endRenderPass();

    in_renderpass = false;
    in_maskupdate_pass = false;
}

void VKContext::begin_visibility_query() {
    constexpr uint32_t queries_per_pool = 256;
    const uint32_t slot = static_cast<uint32_t>(visibility_queries.size() % queries_per_pool);
    if (slot == 0) {
        const vk::QueryPoolCreateInfo info{ .queryType = vk::QueryType::eOcclusion, .queryCount = queries_per_pool };
        visibility_query_pools.push_back(state.device.createQueryPool(info));
        prerender_cmd.resetQueryPool(visibility_query_pools.back(), 0, queries_per_pool);
    }
    const auto pool = visibility_query_pools.back();
    visibility_queries.push_back({ current_visibility_buffer, static_cast<uint32_t>(current_query_idx), is_query_op_increment, pool, slot });
    const vk::QueryControlFlags flags = (is_query_op_increment && state.physical_device_features.occlusionQueryPrecise)
        ? vk::QueryControlFlagBits::ePrecise
        : vk::QueryControlFlags();
    render_cmd.beginQuery(pool, slot, flags);
    is_in_query = true;
}

void VKContext::end_visibility_query() {
    if (!is_in_query)
        return;
    const auto &query = visibility_queries.back();
    render_cmd.endQuery(query.pool, query.slot);
    is_in_query = false;
}

void VKContext::resolve_visibility_queries() {
    if (visibility_queries.empty())
        return;

    if (!visibility_pipeline) {
        const auto code = shader::create_visibility_reduce_shader();
        const vk::ShaderModuleCreateInfo shader_info{ .codeSize = code.size() * sizeof(uint32_t), .pCode = code.data() };
        visibility_shader = state.device.createShaderModule(shader_info);
        const vk::DescriptorSetLayoutBinding binding{ 0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute };
        vk::DescriptorSetLayoutCreateInfo descriptor_info{};
        descriptor_info.setBindings(binding);
        visibility_descriptor_layout = state.device.createDescriptorSetLayout(descriptor_info);
        const vk::PushConstantRange constants{ vk::ShaderStageFlagBits::eCompute, 0, 2 * sizeof(uint32_t) };
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(visibility_descriptor_layout);
        layout_info.setPushConstantRanges(constants);
        visibility_pipeline_layout = state.device.createPipelineLayout(layout_info);
        const vk::ComputePipelineCreateInfo pipeline_info{
            .stage = { .stage = vk::ShaderStageFlagBits::eCompute, .module = visibility_shader, .pName = "main" },
            .layout = visibility_pipeline_layout
        };
        const auto result = state.device.createComputePipeline(nullptr, pipeline_info);
        if (result.result != vk::Result::eSuccess) {
            LOG_ERROR("Cannot create visibility reduction pipeline: {}", vk::to_string(result.result));
            throw std::runtime_error("Cannot create visibility reduction pipeline");
        }
        visibility_pipeline = result.value;
    }

    std::map<std::pair<Address, uint32_t>, std::vector<const VisibilityQuery *>> groups;
    for (const auto &query : visibility_queries)
        groups[{ query.buffer->address, query.index }].push_back(&query);

    const vk::DeviceSize result_offset = visibility_queries.size() * 2 * sizeof(uint32_t);
    const vk::DeviceSize size = result_offset + groups.size() * 4 * sizeof(uint32_t);
    if (size > state.physical_device_properties.limits.maxStorageBufferRange) {
        LOG_ERROR("Visibility reduction requires {} bytes, exceeding the GPU storage buffer range", size);
        throw std::runtime_error("Visibility reduction exceeds GPU storage buffer range");
    }
    vkutil::Buffer scratch(size);
    scratch.init_buffer(vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc);

    const vk::DescriptorPoolSize pool_size{ vk::DescriptorType::eStorageBuffer, 1 };
    vk::DescriptorPoolCreateInfo pool_info{ .maxSets = 1 };
    pool_info.setPoolSizes(pool_size);
    auto descriptor_pool = state.device.createDescriptorPool(pool_info);
    vk::DescriptorSetAllocateInfo allocation{ .descriptorPool = descriptor_pool };
    allocation.setSetLayouts(visibility_descriptor_layout);
    const auto descriptor = state.device.allocateDescriptorSets(allocation)[0];
    const vk::DescriptorBufferInfo buffer_info{ scratch.buffer, 0, size };
    vk::WriteDescriptorSet write{ .dstSet = descriptor, .dstBinding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer };
    write.setBufferInfo(buffer_info);
    state.device.updateDescriptorSets(write, {});

    uint32_t cursor = 0;
    vk::DeviceSize header_offset = result_offset;
    for (const auto &[key, queries] : groups) {
        const std::array<uint32_t, 2> header{ cursor, static_cast<uint32_t>(queries.size()) };
        render_cmd.updateBuffer(scratch.buffer, header_offset, sizeof(header), header.data());
        auto &buffer = *queries.front()->buffer;
        const auto previous_scene = buffer.result_scenes.find(key.second);
        if (previous_scene != buffer.result_scenes.end() && previous_scene->second == gxm_scene_id) {
            const vk::BufferCopy copy{ buffer.buffer_offset + key.second * sizeof(uint32_t), header_offset + 8, sizeof(uint32_t) };
            render_cmd.copyBuffer(buffer.gpu_buffer, scratch.buffer, copy);
        } else {
            const uint32_t zero = 0;
            render_cmd.updateBuffer(scratch.buffer, header_offset + 8, sizeof(zero), &zero);
        }
        buffer.result_scenes[key.second] = gxm_scene_id;
        header_offset += 4 * sizeof(uint32_t);
        for (const auto *query : queries) {
            const uint32_t increment = query->increment;
            render_cmd.updateBuffer(scratch.buffer, cursor * 8 + 4, sizeof(increment), &increment);
            render_cmd.copyQueryPoolResults(query->pool, query->slot, 1, scratch.buffer, cursor * 8,
                sizeof(uint32_t), vk::QueryResultFlagBits::eWait);
            ++cursor;
        }
    }
    const vk::MemoryBarrier input_barrier{ .srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite };
    render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader, {}, input_barrier, {}, {});
    render_cmd.bindPipeline(vk::PipelineBindPoint::eCompute, visibility_pipeline);
    render_cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, visibility_pipeline_layout, 0, descriptor, {});
    const std::array<uint32_t, 2> constants{ static_cast<uint32_t>(result_offset / sizeof(uint32_t)), static_cast<uint32_t>(groups.size()) };
    render_cmd.pushConstants(visibility_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(constants), constants.data());
    render_cmd.dispatch((constants[1] + 63) / 64, 1, 1);
    const vk::MemoryBarrier output_barrier{ .srcAccessMask = vk::AccessFlagBits::eShaderWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead };
    render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, {}, output_barrier, {}, {});
    vk::DeviceSize offset = result_offset + 3 * sizeof(uint32_t);
    for (const auto &[key, queries] : groups) {
        const auto &buffer = *queries.front()->buffer;
        const vk::BufferCopy copy{ offset, buffer.buffer_offset + key.second * sizeof(uint32_t), sizeof(uint32_t) };
        render_cmd.copyBuffer(scratch.buffer, buffer.gpu_buffer, copy);
        visibility_sync_ranges.push_back({ buffer.address + key.second * uint32_t{ 4 }, sizeof(uint32_t) });
        offset += 4 * sizeof(uint32_t);
    }
    const vk::MemoryBarrier destination_barrier{ .srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite };
    render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, destination_barrier, {}, {});
    state.frame().destroy_queue.add_buffer(scratch);
    state.frame().destroy_queue.add(descriptor_pool);
    for (auto &pool : visibility_query_pools)
        state.frame().destroy_queue.add(pool);
    visibility_query_pools.clear();
    visibility_queries.clear();
}

void VKContext::stop_recording(const SceGxmNotification &notif1, const SceGxmNotification &notif2, bool submit) {
    if (!is_recording) {
        LOG_ERROR("Stopping recording while not recording");
        return;
    }

    if (in_renderpass)
        stop_render_pass();

    resolve_visibility_queries();

    state.surface_cache.note_scene_draw_rect(draw_rect_x0, draw_rect_y0, draw_rect_x1, draw_rect_y1);
    ColorSurfaceCacheInfo *surface_info = nullptr;
    if (state.features.enable_memory_mapping && !state.disable_surface_sync && submit)
        surface_info = state.surface_cache.perform_surface_sync();
    if (surface_info)
        scene_has_unsynced_color_draw = false;

#ifdef __SWITCH__
    // Flush before submit: a dirty CPU line could overwrite these GPU writes.
    const auto flush_external_write_dst = [this](Address addr, uint32_t size) {
        auto mem_it = state.mapped_memories.lower_bound(addr);
        if (mem_it == state.mapped_memories.end()
            || static_cast<uint64_t>(mem_it->first) + mem_it->second.size < static_cast<uint64_t>(addr) + size)
            return;
        auto *const ext = std::get_if<ExternalBuffer>(&mem_it->second.buffer_impl);
        if (ext && ext->extra)
            armDCacheFlush(static_cast<uint8_t *>(ext->extra) + (addr - mem_it->first), size);
    };
    if (state.need_cpu_buffer_sync() && current_visibility_buffer) {
        for (const auto &range : visibility_sync_ranges)
            flush_external_write_dst(range.location, range.size);
    }
    if (surface_info && surface_info->need_buffer_sync) {
        const renderer::texture::ReadbackRows rows{
            surface_info->stride_bytes,
            uint32_t(surface_info->original_width) * (gxm::bits_per_pixel(surface_info->format) / 8),
            surface_info->original_height
        };
        if (const auto size = rows.span_size())
            flush_external_write_dst(surface_info->data.address(), *size);
    }
#endif

    prerender_cmd.end();
    render_cmd.end();

    // the prerender cmd must be submitted before the render cmd, the pipeline barriers do the rest
    cmdbuffers_to_submit.push_back(prerender_cmd);
    cmdbuffers_to_submit.push_back(render_cmd);

    render_cmd = nullptr;
    prerender_cmd = nullptr;
    is_recording = false;

    if (!submit)
        return;

    vk::Fence fence = next_fence;
    next_fence = nullptr;

#ifdef __SWITCH__
    state.submit_general_sequence(cmdbuffers_to_submit, fence);
#else
    vk::SubmitInfo submit_info{};
    submit_info.setCommandBuffers(cmdbuffers_to_submit);
    state.submit_general(submit_info, fence);
#endif
    cmdbuffers_to_submit.clear();
    state.frame().rendered_fences.push_back(fence);

    if (state.features.enable_memory_mapping) {
        // send it to the wait queue
        state.request_queue.push(FenceWaitRequest{ fence });

        if (state.need_cpu_buffer_sync()) {
            // sync all the visibility buffers
            for (const auto &range : visibility_sync_ranges)
                state.request_queue.push(range);

            // we must sync the two buffers
            if (surface_info && surface_info->need_buffer_sync) {
                const renderer::texture::ReadbackRows rows{
                    surface_info->stride_bytes,
                    uint32_t(surface_info->original_width) * (gxm::bits_per_pixel(surface_info->format) / 8),
                    surface_info->original_height
                };
                if (const auto size = rows.span_size())
                    state.request_queue.push(BufferSyncRequest{ surface_info->data.address(), *size, rows });
            }
        }

        if (surface_info && surface_info->need_post_surface_sync) {
            state.request_queue.push(PostSurfaceSyncRequest{ surface_info });
        }

        if (notif1.address || notif2.address) {
            // notifications last
            NotificationRequest request = {
                .notifications = { notif1, notif2 },
            };
            state.request_queue.push(request);
        }
    }
    visibility_sync_ranges.clear();
}

void VKContext::check_for_macroblock_change(bool is_draw) {
    if (!render_target->has_macroblock_sync)
        return;

    if (!ignore_macroblock && (scissor.extent.width > render_target->macroblock_width || scissor.extent.height > render_target->macroblock_height)) {
        // flower does not specify a scissor adapted to the current macroblock
        // so fallback to the slow path (one scene per draw, can't really do better)
        // TODO: with the feedback loop extension we can do better
        ignore_macroblock = true;
        // in this case we must load and store the depth stencil each time
        current_render_pass = state.pipeline_cache.retrieve_render_pass(current_color_format, true, true, true, !record.color_surface.data, false,
            state.surface_cache.color_surface_has_raw_alias(record.color_surface.data.address()));
    }

    // use the scissor to know in which macroblock we are
    uint16_t curr_macroblock_x = scissor.offset.x / render_target->macroblock_width;
    uint16_t curr_macroblock_y = scissor.offset.y / render_target->macroblock_height;

    if ((ignore_macroblock && is_draw) || curr_macroblock_x != last_macroblock_x || curr_macroblock_y != last_macroblock_y) {
        // we changed the current macroblock, restart the renderpass
        last_macroblock_x = curr_macroblock_x;
        last_macroblock_y = curr_macroblock_y;

        if (in_renderpass) {
            if (state.features.use_texture_viewport) {
                // no need to copy the texture after this renderpass
                stop_render_pass();
            } else {
                SceGxmNotification empty_notification{};
                stop_recording(empty_notification, empty_notification, false);
                start_recording();
                scene_timestamp++;
#ifdef __SWITCH__
                state.texture_cache.memo_scene = scene_timestamp;
#endif
            }
        }
    }
}

void new_frame(VKContext &context) {
    const auto abort_requested = [&]() {
        if (!context.state.render_abort.load(std::memory_order_relaxed)
            && !context.state.request_queue.is_aborted()
            && !context.state.device_lost.load(std::memory_order_acquire))
            return false;
        {
            const std::lock_guard<std::mutex> lock(context.state.command_finish_one_mutex);
            context.state.render_abort.store(true, std::memory_order_relaxed);
        }
        context.state.command_finish_one.notify_all();
        return true;
    };
    if (abort_requested())
        return;

    if (context.state.features.enable_memory_mapping) {
        FrameDoneRequest request = { context.frame_timestamp };
        context.state.request_queue.push(request);

        context.state.surface_cache.clear_surfaces_changed();
    }

    context.frame_timestamp++;
    context.state.current_frame_idx = context.frame_timestamp % MAX_FRAMES_RENDERING;

    vk::Device device = context.state.device;
    FrameObject &frame = context.state.frame();

    // wait on all fences still present to make sure
    if (!frame.rendered_fences.empty()) {
        // wait for the fences, then reset them

        if (context.state.features.enable_memory_mapping) {
            // this will underflow for the first MAX_FRAMES_RENDERING frames
            // but that's not an issue as frame.rendered_fences will be empty
            uint64_t previous_frame_timestamp = context.frame_timestamp - MAX_FRAMES_RENDERING;

            // the wait is done by the wait thread
            std::unique_lock<std::mutex> lock(context.new_frame_mutex);
            while (!abort_requested() && context.last_frame_waited < previous_frame_timestamp)
                context.new_frame_condv.wait_for(lock, std::chrono::milliseconds(100));
        } else {
            while (!abort_requested()) {
                const auto result = device.waitForFences(frame.rendered_fences, VK_TRUE, 100'000'000ULL);
                if (result == vk::Result::eSuccess)
                    break;
                if (result == vk::Result::eTimeout)
                    continue;
                LOG_ERROR("Could not wait for fences.");
                context.state.device_lost.store(true, std::memory_order_release);
            }
        }

        // Shutdown may acknowledge a frame without completing its GPU work.
        if (abort_requested())
            return;

        // reset the fences in both case (the wait thread does not do that as they can still be used)
        device.resetFences(frame.rendered_fences);
        frame.rendered_fences.clear();
    }

    if (abort_requested())
        return;
    device.resetCommandPool(frame.prerender_pool);
    device.resetCommandPool(frame.render_pool);

    // set the position in the used descriptor queue back to the beginning
    for (int i = 0; i < 16; i++) {
        frame.vert_descriptors[i].descriptors_idx = 0;
        frame.frag_descriptors[i].descriptors_idx = 0;
    }
    frame.color_descriptor.descriptors_idx = 0;

    // deferred destruction of the objects
    frame.destroy_queue.destroy_objects();

    context.last_vert_texture_count = ~0;
    context.last_frag_texture_count = ~0;

    frame.frame_timestamp = context.frame_timestamp;
}

void signal_sync_object(VKState &state, SceGxmSyncObject *sync_object, uint32_t timestamp) {
    assert(state.features.enable_memory_mapping);

    SyncSignalRequest request{
        .sync = sync_object,
        .timestamp = timestamp
    };
    state.request_queue.push(request);
}

} // namespace renderer::vulkan
