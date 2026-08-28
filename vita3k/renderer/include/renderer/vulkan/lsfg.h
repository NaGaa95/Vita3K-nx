// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Vita3K consistently uses Vulkan-Hpp as aggregate C-style structures so its
// designated initializers remain valid. This header can be the first Vulkan
// include in Switch-only translation units, so establish the same mode here.
#ifndef VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_CONSTRUCTORS
#endif
#include <vulkan/vulkan.hpp>

#include <string>
#include <vector>

namespace renderer::vulkan {

struct VKState;

namespace lsfg {

// Lossless Scaling ships these shaders separately. Vita3K never bundles the
// proprietary DLL; users provide it beside the Switch port's data files.
constexpr const char *DLL_PATH = "/switch/vita3k/lsfg/Lossless.dll";
constexpr const char *DLL_DISPLAY_PATH = "sdmc:/switch/vita3k/lsfg/Lossless.dll";

void begin_session(bool requested, float flow_scale, bool performance_mode);
void end_session();

bool is_session_prepared();
void disable_session(const char *reason);

// Swapchain handles remain owned by Vita3K.
bool register_swapchain(VKState &state, vk::SwapchainKHR swapchain,
    vk::Extent2D extent, const std::vector<vk::Image> &images);
void unregister_swapchain();

vk::Result present(VKState &state, const vk::PresentInfoKHR &present_info);

bool is_dll_installed();
bool is_available();
bool is_enabled();
bool is_high_fps_passthrough();
bool request_enabled(bool enabled);
std::string get_status();

} // namespace lsfg
} // namespace renderer::vulkan
