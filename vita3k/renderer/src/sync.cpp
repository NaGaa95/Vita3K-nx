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

#include <chrono>
#include <future>
#include <renderer/commands.h>
#include <renderer/driver_functions.h>
#include <renderer/state.h>
#include <renderer/types.h>

#include <display/state.h>
#include <renderer/gl/functions.h>
#include <renderer/gl/state.h>
#include <renderer/vulkan/functions.h>
#include <renderer/vulkan/state.h>
#include <renderer/vulkan/types.h>

#include <renderer/functions.h>
#include <util/tracy.h>

namespace renderer {
static void wait_for_submitted_work(State &state) {
    if (state.render_abort.load(std::memory_order_relaxed))
        return;

    if (state.current_backend == Backend::OpenGL) {
        glFinish();
        return;
    }

    if (state.current_backend != Backend::Vulkan || state.features.enable_memory_mapping)
        return;

    auto &vk_state = static_cast<vulkan::VKState &>(state);
    // Only the render thread may wait here, since it also resets these fences.
    for (const auto &frame : vk_state.frames) {
        if (frame.rendered_fences.empty())
            continue;

        while (!state.render_abort.load(std::memory_order_relaxed)
            && !vk_state.device_lost.load(std::memory_order_acquire)) {
            vk::Result result;
            try {
                result = vk_state.device.waitForFences(frame.rendered_fences, VK_TRUE, 100'000'000ULL);
            } catch (const vk::SystemError &) {
                result = vk::Result::eErrorDeviceLost;
            }
            if (result == vk::Result::eSuccess)
                break;
            if (result == vk::Result::eTimeout)
                continue;

            LOG_CRITICAL("Could not wait for Vulkan completion: {}", vk::to_string(result));
            vk_state.device_lost.store(true, std::memory_order_release);
            return;
        }
    }
}

COMMAND(handle_nop) {
    TRACY_FUNC_COMMANDS(handle_nop);
    int code_to_finish = helper.pop<int>();
    if (helper.pop<bool>())
        wait_for_submitted_work(renderer);

    // An aborted waiter may already have released its status storage.
    const std::lock_guard<std::mutex> lock(renderer.command_finish_one_mutex);
    if (!renderer.render_abort.load(std::memory_order_relaxed))
        helper.complete(code_to_finish);
    renderer.command_finish_one.notify_all();
}

COMMAND(handle_signal_sync_object) {
    TRACY_FUNC_COMMANDS(handle_signal_sync_object);
    SceGxmSyncObject *sync = helper.pop<Ptr<SceGxmSyncObject>>().get(mem);
    const uint32_t timestamp = helper.pop<uint32_t>();

    if (features.enable_memory_mapping && config.current_config.high_accuracy) {
        assert(renderer.current_backend == renderer::Backend::Vulkan);
        vulkan::signal_sync_object(dynamic_cast<vulkan::VKState &>(renderer), sync, timestamp);
    } else {
        renderer::subject_done(sync, timestamp);
    }
}

COMMAND(handle_wait_sync_object) {
    TRACY_FUNC_COMMANDS(handle_wait_sync_object);
    SceGxmSyncObject *sync = helper.pop<Ptr<SceGxmSyncObject>>().get(mem);
    const uint32_t timestamp = helper.pop<uint32_t>();

#ifdef __SWITCH__
    // Guest threads are suspended while the quick menu is open, so nothing can
    // signal this object. Give the wait up while paused so the render thread can
    // draw the menu; the guest resubmits on resume.
    while (renderer::wishlist(sync, timestamp, 2000) == renderer::SyncWaitResult::TimedOut) {
        if (renderer.paused.load(std::memory_order_relaxed)
            || renderer.render_abort.load(std::memory_order_relaxed))
            break;
    }
#else
    renderer::wishlist(sync, timestamp);
#endif
}

COMMAND(handle_notification) {
    TRACY_FUNC_COMMANDS(handle_notification);
    SceGxmNotification notif = helper.pop<SceGxmNotification>();

    {
        std::unique_lock<std::mutex> lock(renderer.notification_mutex);
        uint32_t *val = notif.address.get(mem);
        if (val) // Ratchet and clank Trilogy request this
            *val = notif.value;
    }
    renderer.notification_ready.notify_all();
}

COMMAND(handle_set_screen_filter) {
    TRACY_FUNC_COMMANDS(handle_set_screen_filter);
    std::unique_ptr<std::string> filter(helper.pop<std::string *>());

    switch (renderer.current_backend) {
    case Backend::OpenGL:
        dynamic_cast<gl::GLState &>(renderer).set_screen_filter(*filter);
        break;

    case Backend::Vulkan:
        dynamic_cast<vulkan::VKState &>(renderer).screen_renderer.set_filter(*filter);
        break;
    }
}

COMMAND(new_frame) {
    TRACY_FUNC_COMMANDS(new_frame);
    DisplayFrameInfo *next_frame = helper.pop<DisplayFrameInfo *>();
    DisplayState *display = helper.pop<DisplayState *>();

    if (next_frame) {
        // set the predicted frame as the next one to render
        std::lock_guard<std::mutex> guard(display->display_info_mutex);
        display->next_rendered_frame = *next_frame;
        delete next_frame;

        renderer.should_display = true;
    }

    if (renderer.current_backend == Backend::Vulkan) {
        renderer::Context *active_context = helper.pop<renderer::Context *>();
        if (active_context) {
            vulkan::new_frame(*reinterpret_cast<vulkan::VKContext *>(active_context));
        }
    }
}

// Client side function
void finish(State &state, Context *context, bool wait_for_gpu) {
    // Add NOP then wait for it
    renderer::send_single_command(state, context, renderer::CommandOpcode::Nop, true, 1, wait_for_gpu);

    // unblock game threads if shutting down
    if (state.render_abort.load(std::memory_order_relaxed))
        return;

    // Wait for the VK wait thread to finish processing all pending requests.
    // Push a callback request on the queue and wait for it to be treated
    if (state.current_backend == Backend::Vulkan && state.features.enable_memory_mapping) {
        auto &vk_state = static_cast<vulkan::VKState &>(state);
        // The callback may outlive this wait if rendering aborts.
        const auto promise = std::make_shared<std::promise<void>>();
        auto callback = [promise]() {
            promise->set_value();
        };
        vk_state.request_queue.push(vulkan::CallbackRequest{ new vulkan::CallbackRequestFunction(callback), wait_for_gpu });

        // The wait thread can stop after the abort check above.
        auto future = promise->get_future();
        while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
            if (state.render_abort.load(std::memory_order_relaxed) || vk_state.request_queue.is_aborted())
                break;
        }
    }
}

int wait_for_status(State &state, int *status, int signal, bool wake_on_equal) {
    std::unique_lock<std::mutex> lock(state.command_finish_one_mutex);
    const bool wake_on_unequal = !wake_on_equal;
    if ((*status == signal) ^ wake_on_unequal) {
        // Signaled, return
        return *status;
    }

    // unblock threads if shutting down
    state.command_finish_one.wait(lock, [&]() {
        return state.render_abort.load(std::memory_order_relaxed)
            || ((*status == signal) ^ wake_on_unequal);
    });
    return *status;
}

SyncWaitResult wishlist(SceGxmSyncObject *sync_object, const uint32_t timestamp, const int32_t timeout_micros) {
    std::unique_lock<std::mutex> lock(sync_object->lock);
    if (sync_object->timestamp_current < timestamp) {
        const auto &pred = [&]() {
            return sync_object->being_deleted || sync_object->timestamp_current >= timestamp;
        };

        if (timeout_micros == -1) {
            sync_object->cond.wait(lock, pred);
        } else if (!sync_object->cond.wait_for(lock, std::chrono::microseconds(timeout_micros), pred)) {
            return SyncWaitResult::TimedOut;
        }
    }
    if (sync_object->being_deleted)
        return SyncWaitResult::Shutdown;
    return SyncWaitResult::Ready;
}

void subject_done(SceGxmSyncObject *sync_object, const uint32_t timestamp) {
    assert(sync_object->timestamp_ahead >= timestamp);
    {
        std::unique_lock<std::mutex> lock(sync_object->lock);
        sync_object->timestamp_current = std::max(sync_object->timestamp_current.load(), timestamp);
    }
    // maybe notify_one is enough
    sync_object->cond.notify_all();
}

void submit_command_list(State &state, renderer::Context *context, CommandList &command_list) {
#ifdef __SWITCH__
    if (context)
        context->clear_emission_dedup();
#endif
    command_list.context = context;
    state.command_buffer_queue.push(std::move(command_list));
}
} // namespace renderer
