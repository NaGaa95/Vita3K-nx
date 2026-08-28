// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <renderer/vulkan/lsfg.h>
#include <renderer/vulkan/state.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>

#include "lsfg_bridge.h"

#ifdef __SWITCH__
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
#endif

namespace renderer::vulkan::lsfg
{
namespace
{
struct SessionState
{
  std::mutex mutex;
  std::atomic_bool prepared{false};
  std::atomic_bool available{false};
  std::atomic_bool enabled{false};

  bool initialization_attempted = false;
  std::string status = "Disabled in the launcher";

  float flow_scale = 0.25f;
  bool performance_mode = true;
  VKState* state = nullptr;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent{};
  std::vector<VkImage> images;
  LsfgNxRuntime* runtime = nullptr;

  std::uint64_t last_source_present_ns = 0;
  double source_interval_ns = 0.0;
  unsigned source_samples = 0;
  unsigned high_fps_slow_samples = 0;
  bool previous_requested = false;
  std::atomic_int rate_decision{-1};
};

SessionState s_state;

void DestroyRuntimeLocked()
{
  if (!s_state.runtime)
    return;

  lsfg_nx_destroy(s_state.runtime);
  s_state.runtime = nullptr;
}

void DisableSessionLocked(const char* reason)
{
  DestroyRuntimeLocked();
  s_state.enabled.store(false, std::memory_order_release);
  s_state.available.store(false, std::memory_order_release);
  s_state.prepared.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.status = reason && *reason ? reason : "Frame generation is unavailable";
}

vk::Result PresentNormally(VKState& state, const vk::PresentInfoKHR& present_info)
{
  return state.general_queue.presentKHR(&present_info);
}

void ResetRateTrackingLocked()
{
  s_state.last_source_present_ns = 0;
  s_state.source_interval_ns = 0.0;
  s_state.source_samples = 0;
  s_state.high_fps_slow_samples = 0;
  s_state.previous_requested = false;
  s_state.rate_decision = -1;
}

std::uint64_t ObserveSourcePresentLocked()
{
  const std::uint64_t now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  std::uint64_t interval = 0;
  if (s_state.last_source_present_ns != 0 && now > s_state.last_source_present_ns)
  {
    interval = now - s_state.last_source_present_ns;
    if (interval >= 4'000'000 && interval <= 100'000'000)
    {
      if (s_state.source_samples == 0)
        s_state.source_interval_ns = static_cast<double>(interval);
      else
        s_state.source_interval_ns =
            s_state.source_interval_ns * 0.875 + static_cast<double>(interval) * 0.125;
      if (s_state.source_samples < 120)
        ++s_state.source_samples;
    }
    else
    {
      interval = 0;
    }
  }
  s_state.last_source_present_ns = now;
  return interval;
}

bool SourceIsHighRateLocked()
{
  return s_state.source_samples >= 8 && s_state.source_interval_ns > 0.0 &&
         s_state.source_interval_ns < 30'000'000.0;
}
}  // namespace

bool is_dll_installed()
{
  std::FILE* file = std::fopen(DLL_PATH, "rb");
  if (!file)
    return false;
  std::fclose(file);
  return true;
}

void begin_session(bool requested, float flow_scale, bool performance_mode)
{
  std::lock_guard lock{s_state.mutex};

  DestroyRuntimeLocked();
  s_state.prepared.store(false, std::memory_order_release);
  s_state.available.store(false, std::memory_order_release);
  s_state.enabled.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.extent = {};
  s_state.images.clear();
  s_state.state = nullptr;
  s_state.flow_scale = flow_scale == 0.5f ? 0.5f : 0.25f;
  s_state.performance_mode = performance_mode;
  ResetRateTrackingLocked();

  const bool dll_installed = is_dll_installed();
  if (!requested)
  {
    s_state.status = "Disabled in the launcher";
    return;
  }
  if (!dll_installed)
  {
    s_state.status = "Lossless.dll is missing";
    return;
  }


  s_state.prepared.store(true, std::memory_order_release);
  s_state.status = "Prepared; enable it from the in-game quick menu";
}

void end_session()
{
  std::lock_guard lock{s_state.mutex};
  DestroyRuntimeLocked();
  s_state.enabled.store(false, std::memory_order_release);
  s_state.available.store(false, std::memory_order_release);
  s_state.prepared.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.extent = {};
  s_state.images.clear();
  s_state.state = nullptr;
  ResetRateTrackingLocked();
  s_state.status = "Disabled in the launcher";
}

bool is_session_prepared()
{
  return s_state.prepared.load(std::memory_order_acquire);
}

void disable_session(const char* reason)
{
  std::lock_guard lock{s_state.mutex};
  DisableSessionLocked(reason);
}

bool register_swapchain(VKState& state, vk::SwapchainKHR swapchain,
                        vk::Extent2D extent, const std::vector<vk::Image>& images)
{
  std::lock_guard lock{s_state.mutex};
  DestroyRuntimeLocked();
  s_state.initialization_attempted = false;
  s_state.available.store(false, std::memory_order_release);
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.images.clear();

  if (!s_state.prepared.load(std::memory_order_acquire))
    return false;
  if (!state.device || !state.general_queue || !swapchain || extent.width == 0 ||
      extent.height == 0 || images.size() < 3)
  {
    DisableSessionLocked("The Vulkan swapchain is not compatible with LSFG");
    return false;
  }
  s_state.state = &state;
  s_state.swapchain = static_cast<VkSwapchainKHR>(swapchain);
  s_state.extent = static_cast<VkExtent2D>(extent);
  s_state.images.clear();
  s_state.images.reserve(images.size());
  for (vk::Image image : images)
    s_state.images.push_back(static_cast<VkImage>(image));
  ResetRateTrackingLocked();
  s_state.available.store(true, std::memory_order_release);
  s_state.status = s_state.enabled.load(std::memory_order_acquire) ?
                       "Available; frame generation will resume" :
                       "Available; currently Off";
  return true;
}

void unregister_swapchain()
{
  std::lock_guard lock{s_state.mutex};
  DestroyRuntimeLocked();
  s_state.available.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.extent = {};
  s_state.images.clear();
  s_state.state = nullptr;
  ResetRateTrackingLocked();
  if (s_state.prepared.load(std::memory_order_acquire))
    s_state.status = "Waiting for the Vulkan swapchain";
}

vk::Result present(VKState& state, const vk::PresentInfoKHR& present_info)
{
  std::lock_guard lock{s_state.mutex};

  const VkQueue queue = static_cast<VkQueue>(state.general_queue);
  const VkPresentInfoKHR& raw_present_info =
      *reinterpret_cast<const VkPresentInfoKHR*>(&present_info);

  const bool enabled = s_state.enabled.load(std::memory_order_acquire);
  const bool compatible = s_state.available.load(std::memory_order_acquire) &&
                          s_state.state == &state &&
                          raw_present_info.swapchainCount == 1 && raw_present_info.pSwapchains &&
                          raw_present_info.pSwapchains[0] == s_state.swapchain;
  std::uint64_t source_interval = 0;
  if (compatible && (!enabled || s_state.rate_decision != 0))
    source_interval = ObserveSourcePresentLocked();

  if (compatible && enabled != s_state.previous_requested)
  {
    if (enabled)
    {
      s_state.rate_decision = s_state.source_samples >= 8 ?
                                  (SourceIsHighRateLocked() ? 1 : 0) :
                                  -1;
      s_state.high_fps_slow_samples = 0;
    }
    else
    {
      ResetRateTrackingLocked();
    }
    s_state.previous_requested = enabled;
  }

  if (!enabled || !compatible)
  {
    if (!enabled && s_state.runtime)
    {
      DestroyRuntimeLocked();
      s_state.initialization_attempted = false;
      if (s_state.available.load(std::memory_order_acquire))
        s_state.status = "Available; currently Off";
    }
    return PresentNormally(state, present_info);
  }

  if (s_state.rate_decision < 0 && s_state.source_samples >= 8)
    s_state.rate_decision = SourceIsHighRateLocked() ? 1 : 0;

  if (s_state.rate_decision == 1)
  {
    s_state.status = "Frame generation On (native 50/60 FPS protected)";
    if (source_interval >= 31'500'000)
      ++s_state.high_fps_slow_samples;
    else if (source_interval != 0)
      s_state.high_fps_slow_samples = 0;

    if (s_state.high_fps_slow_samples < 16)
      return PresentNormally(state, present_info);

    s_state.rate_decision = 0;
    s_state.high_fps_slow_samples = 0;
  }

  if (s_state.rate_decision < 0)
  {
    s_state.status = "Measuring the game's native frame rate";
    return PresentNormally(state, present_info);
  }

  if (!s_state.runtime)
  {
    if (s_state.initialization_attempted || s_state.state != &state)
    {
      s_state.enabled.store(false, std::memory_order_release);
      return PresentNormally(state, present_info);
    }
    s_state.initialization_attempted = true;

    float flow_scale = s_state.flow_scale;
    if (flow_scale != 0.25f && flow_scale != 0.5f)
      flow_scale = 0.25f;

    const LsfgNxCreateInfo create_info{
        .instance = static_cast<VkInstance>(state.instance),
        .physical_device = static_cast<VkPhysicalDevice>(state.physical_device),
        .device = static_cast<VkDevice>(state.device),
        .queue = queue,
        .queue_family_index = state.general_family_index,
        .get_instance_proc_addr = &::vk_icdGetInstanceProcAddr,
        .swapchain = s_state.swapchain,
        .extent = s_state.extent,
        .swapchain_images = s_state.images.data(),
        .swapchain_image_count = static_cast<std::uint32_t>(s_state.images.size()),
        .shader_dll_path = DLL_PATH,
        .flow_scale = flow_scale,
        .performance_mode = s_state.performance_mode,
    };
    s_state.runtime = lsfg_nx_create(&create_info);
    if (!s_state.runtime)
    {
      s_state.enabled.store(false, std::memory_order_release);
      s_state.available.store(false, std::memory_order_release);
      s_state.status = "LSFG initialization failed";
      // Creation did not consume the render-finished semaphore.
      return PresentNormally(state, present_info);
    }
    s_state.status = "Frame generation On";
  }

  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  if (!lsfg_nx_present(s_state.runtime, queue, &raw_present_info, &result))
  {
    s_state.status = "LSFG rejected the active swapchain";
    s_state.enabled.store(false, std::memory_order_release);
    s_state.available.store(false, std::memory_order_release);
    DestroyRuntimeLocked();
    // The bridge did not consume this presentation.
    return PresentNormally(state, present_info);
  }

  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
  {
    s_state.status = "LSFG presentation failed; recreating the swapchain";
    s_state.enabled.store(false, std::memory_order_release);
    s_state.available.store(false, std::memory_order_release);
    DestroyRuntimeLocked();
  }
  return static_cast<vk::Result>(result);
}

bool is_available()
{
  return s_state.available.load(std::memory_order_acquire);
}

bool is_enabled()
{
  return s_state.enabled.load(std::memory_order_acquire);
}

bool is_high_fps_passthrough()
{
  return s_state.enabled.load(std::memory_order_acquire) &&
         s_state.rate_decision.load(std::memory_order_acquire) == 1;
}

bool request_enabled(bool enabled)
{
  std::lock_guard lock{s_state.mutex};
  if (enabled && (!s_state.prepared.load(std::memory_order_acquire) ||
                  !s_state.available.load(std::memory_order_acquire)))
  {
    return false;
  }

  s_state.enabled.store(enabled, std::memory_order_release);
  s_state.status = enabled ? "Measuring the game's native frame rate" :
                             "Available; currently Off";
  return true;
}

std::string get_status()
{
  std::lock_guard lock{s_state.mutex};
  return s_state.status;
}
} // namespace renderer::vulkan::lsfg
