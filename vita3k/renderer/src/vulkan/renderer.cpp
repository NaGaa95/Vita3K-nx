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

#ifdef __ANDROID__
// must be first
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
#endif

#include <renderer/functions.h>
#include <renderer/types.h>
#include <renderer/vulkan/functions.h>
#ifdef __SWITCH__
#endif
#ifdef __SWITCH__
#include <renderer/vulkan/lsfg.h>
#endif
#include <renderer/vulkan/state.h>

#include <config/state.h>
#include <config/version.h>
#include <display/state.h>
#include <shader/spirv_recompiler.h>
#include <util/align.h>
#include <util/android_driver.h>
#include <util/log.h>
#include <util/warning.h>
#include <vkutil/vkutil.h>

#include <mem/functions.h>
#include <overlay/display_manager.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_set>

#ifdef __APPLE__
#include <MoltenVK/mvk_vulkan.h>
#endif

#ifdef __SWITCH__
// Enter native NVK through its ICD entry point, as the other Switch ports do.
// Mesa's public loaderless vkGetInstanceProcAddr is the Zink-facing path and
// links the lite Vulkan instance runtime, which assumes that Gallium already
// owns the GLSL type singleton. Native Vita3K does not have that owner.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
#include <xxhash.h>
// libnx cache maintenance, declared directly to keep <switch.h> macros out.
extern "C" {
void armDCacheClean(void *addr, size_t size);
void armDCacheFlush(void *addr, size_t size);
}
#endif

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <util/float_to_half.h>

#ifdef USE_ADRENO_TOOLS
#include <adrenotools/bcenabler.h>
#include <adrenotools/driver.h>
#endif

#include <android/hardware_buffer.h>

typedef struct native_handle {
    int version; /* sizeof(native_handle_t) */
    int numFds; /* number of file-descriptors at &data[0] */
    int numInts; /* number of ints at &data[numFds] */
    DISABLE_CLANG_WARNING_BEGIN("-Wzero-length-array")
    int data[0]; /* numFds + numInts ints */
    DISABLE_CLANG_WARNING_END
} native_handle_t;

typedef const native_handle_t *buffer_handle_t;
// this function is exported by libandroid.so but not defined in the header
const native_handle_t *(*_AHardwareBuffer_getNativeHandle)(const AHardwareBuffer *buffer);

// functions defined in hardware_buffer.h that are dynamically load
decltype(AHardwareBuffer_allocate) *_AHardwareBuffer_allocate;
decltype(AHardwareBuffer_lock) *_AHardwareBuffer_lock;
decltype(AHardwareBuffer_unlock) *_AHardwareBuffer_unlock;
decltype(AHardwareBuffer_release) *_AHardwareBuffer_release;
#endif

#ifdef __SWITCH__
// Small enough to avoid clobbering unrelated GPU-written data when the CPU
// changes another part of the same allocation, while keeping checksum metadata
// modest (3.125% of the mapped range).
// Measured at 1024: the dirty-tracking path did get ~35% faster per frame, but
// the render thread it runs on never exceeds 35% of core 3, so none of that
// reaches the frame rate - while the coarser granularity uploaded ~30% more
// bytes per frame, competing for memory bandwidth with the guest cores that
// are the actual constraint. Kept fine-grained.
static constexpr uint32_t SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE = 256;
#endif

static void debug_log_message(std::string_view msg) {
    static const char *ignored_errors[] = {
        "VUID-vkCmdDrawIndexed-None-02721", // using r8g8b8a8 with non-multiple of 4 stride
        "VUID-VkImageViewCreateInfo-usage-02275", // srgb does not support the storage format
        "VUID-VkImageCreateInfo-imageCreateMaxMipLevels-02251", // srgb does not support the storage format
        "VUID-vkCmdPipelineBarrier-pDependencies-02285", // shader write -> vertex input read self-dependency, wrong error
        "VUID-vkCmdDrawIndexed-None-09003", // reading from color attachment, works on most GPUs with a general layout
        "VUID-vkCmdDrawIndexed-None-06538", // reading from color attachment
        "VUID-vkCmdDrawIndexed-None-09000", // reading from color attachment
        "VKDBGUTILWARN003", // Some Adreno warning
        "VK_FORMAT_BC", // BCn patch
        "VUID-vkCmdCopyBufferToImage-dstImage-01997" // BCn patch
    };

    bool log_error = true;
    for (auto ignored_error : ignored_errors) {
        if (msg.contains(ignored_error)) {
            log_error = false;
            break;
        }
    }

    if (log_error) {
#ifdef __SWITCH__
        LOG_ERROR("Vulkan driver: {}", msg);
#else
        LOG_ERROR("Validation layer: {}", msg);
#endif
    }
}

static vk::DebugUtilsMessengerEXT debug_messenger;
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_util_callback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
    vk::DebugUtilsMessageTypeFlagsEXT message_type,
    const vk::DebugUtilsMessengerCallbackDataEXT *callback_data,
    void *pUserData) {
    if (message_severity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
        // for now we are not interested by performance warnings
        && (message_type & ~vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)) {
        debug_log_message(callback_data->pMessage);
    }
#ifdef __SWITCH__
    // The driver's own lifetime diagnostics arrive at Info severity; there is
    // no validation layer here to bury them in noise.
    else if (message_severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
        LOG_INFO("Vulkan driver: {}", callback_data->pMessage);
    }
#endif
    return VK_FALSE;
}

static vk::DebugReportCallbackEXT debug_report;
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report_callback(
    vk::DebugReportFlagsEXT flags,
    vk::DebugReportObjectTypeEXT objectType,
    uint64_t object,
    size_t location,
    int32_t messageCode,
    const char *layerPrefix,
    const char *message,
    void *pUserData) {
    std::string msg = fmt::format(
        "Validation layer: Vk{}:{}[0x{:X}]:I{}:L{}: {}",
        layerPrefix,
        vk::to_string(objectType),
        object,
        messageCode,
        location,
        message);

    debug_log_message(msg);

    return VK_FALSE;
}

const static std::vector<const char *> required_device_extensions = {
    vk::KHRSwapchainExtensionName,
    // needed in order to use storage buffers
    vk::KHRStorageBufferStorageClassExtensionName,
    // needed in order to use negative viewport height
    vk::KHRMaintenance1ExtensionName
};

namespace renderer::vulkan {

#if defined(__ANDROID__) && defined(USE_ADRENO_TOOLS)
// need to avoid patching bcn per custom driver more than once
static bool patch_bcn_once(void *function_to_patch) {
    static std::unordered_set<void *> patched_functions;

    if (patched_functions.find(function_to_patch) != patched_functions.end()) {
        LOG_INFO("BCeNabler patch already applied");
        return true;
    }

    if (!adrenotools_patch_bcn(function_to_patch))
        return false;

    patched_functions.insert(function_to_patch);
    return true;
}

static bool detect_patch_bcn(bool *support_dxt) {
    // some Adreno GPUs support BCn textures even though they say they don't
    // and we might need to patch a function for it to work

    // create an instance to get the patch address
    vk::ApplicationInfo application_info{
        .apiVersion = VK_API_VERSION_1_0
    };
    vk::InstanceCreateInfo instance_info{
        .pApplicationInfo = &application_info
    };

    vk::UniqueInstance instance = vk::createInstanceUnique(instance_info);
    // we need these 2 functions for the following part of the code
    VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(instance.get(), "vkEnumeratePhysicalDevices"));
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(instance.get(), "vkGetPhysicalDeviceProperties"));
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(instance.get(), "vkDestroyInstance"));

    // assume there is only one gpu
    vk::PhysicalDevice gpu = instance->enumeratePhysicalDevices().front();
    vk::PhysicalDeviceProperties properties = gpu.getProperties();

    const auto type = adrenotools_get_bcn_type(VK_VERSION_MAJOR(properties.driverVersion), VK_VERSION_MINOR(properties.driverVersion), properties.vendorID);
    if (type == ADRENOTOOLS_BCN_PATCH) {
        void *function_to_patch = reinterpret_cast<void *>(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(instance.get(), "vkGetPhysicalDeviceFormatProperties"));
        if (patch_bcn_once(function_to_patch)) {
            LOG_INFO("Applied BCeNabler patch");
        } else {
            LOG_INFO("Failed to apply BCeNabler");
            return false;
        }
        *support_dxt = true;
    } else if (type == ADRENOTOOLS_BCN_BLOB) {
        LOG_INFO("BCeNabler skipped, blob BCN support is present");
        *support_dxt = true;
    }

    return true;
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
static bool has_instance_extension(const std::vector<vk::ExtensionProperties> &available_extensions, const std::string_view target_name) {
    return std::find_if(available_extensions.begin(), available_extensions.end(), [&](const vk::ExtensionProperties &ext) {
        return std::string_view(ext.extensionName.data()) == target_name;
    }) != available_extensions.end();
}

static bool select_linux_surface_extension(VKState &vk_state, const renderer::DisplayHandle &display_handle, std::vector<const char *> &instance_extensions) {
    const auto available_extensions = vk::enumerateInstanceExtensionProperties();

#if defined(HAVE_WAYLAND)
    if (std::holds_alternative<renderer::WaylandDisplayHandle>(display_handle)) {
        if (!has_instance_extension(available_extensions, vk::KHRWaylandSurfaceExtensionName)) {
            LOG_ERROR("Could not find Vulkan instance extension {}", vk::KHRWaylandSurfaceExtensionName);
            return false;
        }
        vk_state.linux_surface_type = LinuxSurfaceType::Wayland;
        instance_extensions.push_back(vk::KHRWaylandSurfaceExtensionName);
        return true;
    }
#endif

#if defined(HAVE_X11)
    if (const auto *x11 = std::get_if<renderer::X11DisplayHandle>(&display_handle)) {
        if (has_instance_extension(available_extensions, vk::KHRXlibSurfaceExtensionName)) {
            vk_state.linux_surface_type = LinuxSurfaceType::Xlib;
            instance_extensions.push_back(vk::KHRXlibSurfaceExtensionName);
            return true;
        }
#if defined(VK_USE_PLATFORM_XCB_KHR)
        if (x11->connection && has_instance_extension(available_extensions, "VK_KHR_xcb_surface")) {
            vk_state.linux_surface_type = LinuxSurfaceType::Xcb;
            instance_extensions.push_back("VK_KHR_xcb_surface");
            LOG_INFO("Falling back to XCB Vulkan surface extension");
            return true;
        }
#endif
        LOG_ERROR("Could not find a supported Vulkan surface extension for X11 (tried xlib then xcb)");
        return false;
    }
#endif

    LOG_ERROR("Unsupported display handle on Linux");
    return false;
}
#endif

static bool device_is_compatible(const vk::PhysicalDevice &device) {
    const std::vector<vk::ExtensionProperties> available_extensions = device.enumerateDeviceExtensionProperties();

    std::set<std::string> required_extensions(required_device_extensions.begin(), required_device_extensions.end());
    for (const auto &extension : available_extensions)
        required_extensions.erase(extension.extensionName);

    return required_extensions.empty();
}

static bool select_queues(VKState &vk_state,
    std::vector<vk::DeviceQueueCreateInfo> &queue_infos, std::vector<std::vector<float>> &queue_priorities) {
    // TODO: Better queue allocation.

    /**
     * Here's the idea:
     *  - Dedicated queues to a task (e.g. with only graphics bit set) are faster.
     *  - Queues that appear first in the list are faster.
     *  - We really just need a queue for Graphics and Transfer right now afaik.
     *  - Multiple queues families can do the same thing.
     *  - Multiple different queues should be chosen if available.
     * The current algorithm only picks the first one it finds, a new algorithm should be made that takes everything into account.
     */

    bool found_graphics = false, found_transfer = false;

    for (uint32_t i = 0; i < vk_state.physical_device_queue_families.size(); i++) {
        const auto &queue_family = vk_state.physical_device_queue_families[i];

        // MoltenVK does not accept nullptr a pPriorities for some reason.
        std::vector<float> &priorities = queue_priorities.emplace_back(queue_family.queueCount, 1.0f);

        // Only one DeviceQueueCreateInfo should be created per family.
        if (!found_graphics && (queue_family.queueFlags & vk::QueueFlagBits::eGraphics)
#ifndef __ANDROID__
            && (queue_family.queueFlags & vk::QueueFlagBits::eTransfer)
#endif
            && vk_state.physical_device.getSurfaceSupportKHR(i, vk_state.screen_renderer.surface)) {
            vk::DeviceQueueCreateInfo queue_create_info{
                .queueFamilyIndex = i,
                .queueCount = queue_family.queueCount,
                .pQueuePriorities = priorities.data()
            };
            queue_infos.emplace_back(std::move(queue_create_info));
            vk_state.general_family_index = i;
            vk_state.transfer_family_index = i;
            found_graphics = true;
            found_transfer = true;
        }
        // for now use the same queue for graphics and transfer, to be improved on later
        /* else if (!found_transfer && queue_family.queueFlags&vk::QueueFlagBits::eTransfer) {
            vk::DeviceQueueCreateInfo queue_create_info{
                .queueFamilyIndex = i,
                .queueCount = queue_family.queueCount,
                .pQueuePriorities = priorities.data()
            };
            queue_infos.emplace_back(std::move(queue_create_info));
            vk_state.transfer_family_index = i;
            found_transfer = true;
        }
        */

        if (found_graphics && found_transfer)
            break;
    }

    return found_graphics && found_transfer;
}

// Adapted from https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/master/includes/functions.php
static std::string get_driver_version(uint32_t vendor_id, uint32_t version_raw) {
    // NVIDIA
    if (vendor_id == 4318)
        return fmt::format("{}.{}.{}.{}", (version_raw >> 22) & 0x3ff, (version_raw >> 14) & 0x0ff, (version_raw >> 6) & 0x0ff, version_raw & 0x003f);

#ifdef _WIN32
    // Intel drivers on Windows
    if (vendor_id == 0x8086)
        return fmt::format("{}.{}", version_raw >> 14, version_raw & 0x3fff);
#endif

    // Use Vulkan version conventions if vendor mapping is not available
    return fmt::format("{}.{}.{}", (version_raw >> 22) & 0x3ff, (version_raw >> 12) & 0x3ff, version_raw & 0xfff);
}

bool create(std::unique_ptr<renderer::State> &state, const Config &config) {
    auto &vk_state = dynamic_cast<VKState &>(*state);

    return vk_state.create(state, config);
}

VKState::VKState(int gpu_idx)
    : gpu_idx(gpu_idx)
    , surface_cache(*this)
    , pipeline_cache(*this)
    , texture_cache(*this)
    , screen_renderer(*this)
    , buffer_trapping(*this) {
}

void VKState::submit_general(const vk::SubmitInfo &submit_info, vk::Fence fence, const char *stage) {
    const std::lock_guard<std::mutex> lock(general_queue_mutex);
    try {
        general_queue.submit(submit_info, fence);
    } catch (const vk::SystemError &error) {
        LOG_CRITICAL("Vulkan {} failed: {}", stage, error.what());
        throw;
    }
}

void VKState::submit_general_pair(const vk::SubmitInfo &first_submit_info,
    const vk::SubmitInfo &second_submit_info, vk::Fence second_fence) {
    // Keep dependent submissions adjacent on the queue. In particular, the
    // Switch NVK path needs the pre-render upload work submitted separately
    // from, but immediately before, the render work that consumes it.
    const std::lock_guard<std::mutex> lock(general_queue_mutex);
    try {
        general_queue.submit(first_submit_info);
    } catch (const vk::SystemError &error) {
        LOG_CRITICAL("Vulkan scene pre-render submission failed: {}", error.what());
        throw;
    }
    try {
        general_queue.submit(second_submit_info, second_fence);
    } catch (const vk::SystemError &error) {
        LOG_CRITICAL("Vulkan scene render submission failed: {}", error.what());
        throw;
    }
}

vk::Result VKState::present_general(const vk::PresentInfoKHR &present_info) {
    const std::lock_guard<std::mutex> lock(general_queue_mutex);
#ifdef __SWITCH__
    return lsfg::present(*this, present_info);
#else
    return general_queue.presentKHR(&present_info);
#endif
}

vk::Result VKState::submit_and_present_general(const vk::SubmitInfo &submit_info,
    vk::Fence fence, const vk::PresentInfoKHR &present_info) {
    // Present waits on a semaphore signalled by this submission. Keep the pair
    // atomic with respect to worker-thread uploads and surface synchronisation.
    const std::lock_guard<std::mutex> lock(general_queue_mutex);
    try {
        general_queue.submit(submit_info, fence);
    } catch (const vk::SystemError &error) {
        LOG_CRITICAL("Vulkan swapchain frame submission failed: {}", error.what());
        throw;
    }
#ifdef __SWITCH__
    return lsfg::present(*this, present_info);
#else
    return general_queue.presentKHR(&present_info);
#endif
}

void VKState::wait_device_idle() {
    const std::lock_guard<std::mutex> lock(general_queue_mutex);
    // A lost device has no work left to wait for and answers every call with the
    // same error. Teardown runs through here on the way out of a lost session, so
    // letting it throw turns a device loss the frontend already handled into an
    // abort - the process dies instead of returning to the launcher.
    if (device_lost.load(std::memory_order_acquire))
        return;
    try {
        device.waitIdle();
    } catch (const vk::SystemError &error) {
        LOG_ERROR("Vulkan device wait failed: {}", error.what());
        device_lost.store(true, std::memory_order_release);
    }
}

VKState::OneTimeCommand VKState::create_one_time_command() {
    std::unique_lock<std::mutex> command_pool_lock(one_time_command_pool_mutex);
    vk::CommandBuffer buffer = vkutil::create_single_time_command(device, one_time_command_pool);
    return OneTimeCommand{ buffer, std::move(command_pool_lock) };
}

void VKState::submit_one_time_command(OneTimeCommand command, const char *stage) {
    // command.command_pool_lock stays held until vkutil has submitted, waited,
    // and freed the command buffer from the shared pool.
    const std::lock_guard<std::mutex> queue_lock(general_queue_mutex);
    try {
        vkutil::end_single_time_command(device, general_queue, one_time_command_pool, command.buffer);
    } catch (const vk::SystemError &error) {
        LOG_CRITICAL("Vulkan {} failed: {}", stage, error.what());
        throw;
    }
}

bool VKState::init() {
    shader_version = fmt::format("v{}", shader::CURRENT_VERSION);
    return true;
}

bool VKState::create(std::unique_ptr<renderer::State> &state, const Config &config) {
#ifdef __ANDROID__
    const bool custom_driver_requested = !config.current_config.custom_driver_name.empty();
#endif

    // Create Instance
    {
#if defined(__ANDROID__) && defined(USE_ADRENO_TOOLS)
        PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr = android_driver::resolve_vk_get_instance_proc_addr(config.current_config.custom_driver_name);
        if (!vk_get_instance_proc_addr)
            return false;

        VULKAN_HPP_DEFAULT_DISPATCHER.init(vk_get_instance_proc_addr);

        if (!detect_patch_bcn(&texture_cache.support_dxt))
            return false;
#elif defined(__SWITCH__)
        // The Mesa NVK ICD is statically linked (no shared object to dlopen).
        VULKAN_HPP_DEFAULT_DISPATCHER.init(&::vk_icdGetInstanceProcAddr);
#else
        VULKAN_HPP_DEFAULT_DISPATCHER.init();
#endif

        vk::ApplicationInfo app_info{
            .pApplicationName = app_name, // App Name
            .applicationVersion = VK_MAKE_API_VERSION(0, 0, 0, 1), // App Version
            .pEngineName = org_name, // Engine Name, using org instead.
            .engineVersion = VK_MAKE_API_VERSION(0, 0, 0, 1), // Engine Version
            .apiVersion = VK_API_VERSION_1_0
        };

        std::vector<const char *> instance_extensions;
        instance_extensions.reserve(8);
        instance_extensions.push_back(vk::KHRSurfaceExtensionName);
#ifdef _WIN32
        instance_extensions.push_back(vk::KHRWin32SurfaceExtensionName);
#elif defined(__APPLE__)
        instance_extensions.push_back(vk::EXTMetalSurfaceExtensionName);
#elif defined(__ANDROID__)
        instance_extensions.push_back(vk::KHRAndroidSurfaceExtensionName);
#elif defined(__SWITCH__)
        instance_extensions.push_back(vk::NNViSurfaceExtensionName);
#else
        auto *frame_host = this->renderer::State::frame;
        if (!select_linux_surface_extension(*this, frame_host->handle(), instance_extensions))
            return false;
#endif

        const std::set<std::string> optional_instance_extensions = {
            vk::KHRGetPhysicalDeviceProperties2ExtensionName,
            vk::KHRExternalMemoryCapabilitiesExtensionName,
            vk::KHRDeviceGroupCreationExtensionName,
#ifdef __APPLE__
            vk::KHRPortabilityEnumerationExtensionName,
            vk::EXTLayerSettingsExtensionName,
#endif
        };
        bool has_layer_settings_extension = false;
        for (const vk::ExtensionProperties &prop : vk::enumerateInstanceExtensionProperties()) {
            auto ite = optional_instance_extensions.find(prop.extensionName);
            if (ite != optional_instance_extensions.end()) {
                instance_extensions.push_back(ite->c_str());
#ifdef __APPLE__
                if (*ite == vk::EXTLayerSettingsExtensionName)
                    has_layer_settings_extension = true;
#endif
            }
        }

        // look if we can use the validation layer
        bool has_validation_layer = false;
        const std::array<const std::string, 2> debug_extensions = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
            VK_EXT_DEBUG_REPORT_EXTENSION_NAME };
        // use a string, not a string view, on some mali devices the memory gets modified
        std::string found_debug_extension;
        for (const vk::ExtensionProperties &prop : vk::enumerateInstanceExtensionProperties()) {
            const std::string_view extension(prop.extensionName.data());
            for (const auto &debug_ext : debug_extensions) {
                if (debug_ext == extension)
                    found_debug_extension = extension;
            }
        }
        const std::string validation_layer = "VK_LAYER_KHRONOS_validation";
        for (const vk::LayerProperties &layer : vk::enumerateInstanceLayerProperties()) {
            if (std::string_view(layer.layerName.data()) == validation_layer) {
                has_validation_layer = true;
                break;
            }
        }

#ifdef __SWITCH__
        // There is no validation layer on Horizon, but NVK implements
        // VK_EXT_debug_utils itself, so the driver's own diagnostics - shader
        // compiler faults above all - are still reachable. Without this they are
        // dropped and a failed pipeline reaches the log as a bare vk::Result.
        const bool driver_debug_messages = !has_validation_layer
            && found_debug_extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        if (driver_debug_messages)
            instance_extensions.push_back(found_debug_extension.data());
#endif

        std::vector<const char *> instance_layers;
        if (has_validation_layer && !found_debug_extension.empty()) {
            if (config.validation_layer) {
                LOG_INFO("Enabling vulkan validation layers (has a performance impact but allows better error messages)");
                instance_layers.push_back(validation_layer.c_str());
                instance_extensions.push_back(found_debug_extension.data());
            } else {
                LOG_INFO("Disabling Vulkan validation layers (may improve performance but provides limited error messages)");
            }
        }

#ifdef __APPLE__
        const VkBool32 full_image_swizzle = VK_TRUE;
        const VkBool32 resume_lost_device = VK_TRUE;
#ifndef NDEBUG
        const VkBool32 debug = VK_TRUE;
        const int32_t log_level = 4;
#endif
        vk::LayerSettingEXT layer_settings[] = {
            { kMVKMoltenVKDriverLayerName, "MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE", vk::LayerSettingTypeEXT::eBool32, 1,
                &full_image_swizzle },
            { kMVKMoltenVKDriverLayerName, "MVK_CONFIG_RESUME_LOST_DEVICE", vk::LayerSettingTypeEXT::eBool32, 1,
                &resume_lost_device },
#ifndef NDEBUG
            { kMVKMoltenVKDriverLayerName, "MVK_CONFIG_DEBUG", vk::LayerSettingTypeEXT::eBool32, 1, &debug },
            { kMVKMoltenVKDriverLayerName, "MVK_CONFIG_LOG_LEVEL", vk::LayerSettingTypeEXT::eInt32, 1, &log_level },
#endif
        };

        vk::LayerSettingsCreateInfoEXT layer_settings_info = {
            .pNext = nullptr,
            .settingCount = static_cast<uint32_t>(std::size(layer_settings)),
            .pSettings = layer_settings,
        };
        const void *instance_create_pnext = has_layer_settings_extension ? &layer_settings_info : nullptr;
#endif

        vk::InstanceCreateInfo instance_info{
#ifdef __APPLE__
            .flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
            .pNext = instance_create_pnext,
#endif
            .pApplicationInfo = &app_info,
        };
        instance_info.setPEnabledLayerNames(instance_layers);
        instance_info.setPEnabledExtensionNames(instance_extensions);

#ifdef __SWITCH__
        lsfg::begin_session(config.current_config.switch_lsfg_enabled,
            config.current_config.switch_lsfg_flow_scale,
            config.current_config.switch_lsfg_performance);
#endif
        instance = vk::createInstance(instance_info);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

        bool create_debug_messenger = has_validation_layer && !found_debug_extension.empty() && config.validation_layer;
#ifdef __SWITCH__
        create_debug_messenger |= driver_debug_messages;
#endif
        if (create_debug_messenger) {
            // we support two debugging extensions
            if (found_debug_extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME) {
                vk::DebugUtilsMessengerCreateInfoEXT debug_info{
                    .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
#ifdef __SWITCH__
                        // NVK reports its lifetime statistics at Info severity.
                        | vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo
#endif
                        | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
                    .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
                        | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
                    .pfnUserCallback = debug_util_callback
                };
                debug_messenger = instance.createDebugUtilsMessengerEXT(debug_info);

            } else if (found_debug_extension == VK_EXT_DEBUG_REPORT_EXTENSION_NAME) {
                vk::DebugReportCallbackCreateInfoEXT report_info{
                    .flags = vk::DebugReportFlagBitsEXT::eError,
                    .pfnCallback = debug_report_callback
                };
                debug_report = instance.createDebugReportCallbackEXT(report_info);
            }
        }
    }

    // Create Surface
    if (!screen_renderer.create())
        return false;

    // Select Physical Device
    {
        std::vector<vk::PhysicalDevice> physical_devices = instance.enumeratePhysicalDevices();

        if (gpu_idx > 0 && gpu_idx <= physical_devices.size()) {
            // force choose the gpu
            physical_device = physical_devices[gpu_idx - 1];
        } else {
            // choose a suitable gpu
            for (const auto &device : physical_devices) {
                if (!device_is_compatible(device))
                    continue;

                using enum vk::PhysicalDeviceType;
                const auto device_type = device.getProperties().deviceType;
                if (!physical_device)
                    physical_device = device;
                else if (physical_device.getProperties().deviceType != device_type) {
                    if (device_type == eDiscreteGpu || device_type == eIntegratedGpu)
                        physical_device = device;
                }

                // if it is not a discrete gpu, try to find a discrete one
                if (device_type == eDiscreteGpu)
                    break;
            }
        }

        if (!physical_device) {
            LOG_ERROR("Failed to select Vulkan physical device.");
            return false;
        }

        physical_device_properties = physical_device.getProperties();
        physical_device_features = physical_device.getFeatures();
        physical_device_memory = physical_device.getMemoryProperties();
        physical_device_queue_families = physical_device.getQueueFamilyProperties();

#ifdef __ANDROID__
        if (custom_driver_requested) {
            if (android_driver::is_custom_driver_loaded(
                    config.current_config.custom_driver_name,
                    physical_device_properties.vendorID,
                    physical_device_properties.driverVersion,
                    physical_device_properties.deviceName.data()))
                LOG_INFO("Custom Adreno driver {} injected successfully", config.current_config.custom_driver_name);
            else
                LOG_WARN("Custom Adreno driver {} fell back to the system Vulkan loader", config.current_config.custom_driver_name);
        }
#endif

        LOG_INFO("Vulkan device: {}", physical_device_properties.deviceName.data());
        LOG_INFO("Driver version: {}", get_driver_version(physical_device_properties.vendorID, physical_device_properties.driverVersion));
    }

#ifdef __ANDROID__
    if (support_custom_drivers()) {
        // First I was looking for "Turnip" in the device name, however some turnip driver do not have it in their name for whatever reason....
        // so as a ugly workaround, say it is a turnip driver if the major driver version is less than 100
        uint32_t major_driver_version = physical_device_properties.driverVersion >> 22;
        is_adreno_stock = major_driver_version >= 100;
        is_adreno_turnip = major_driver_version < 100;
    }
#endif

    bool support_dedicated_allocations = false;
    // Create Device
    {
        std::vector<vk::DeviceQueueCreateInfo> queue_infos;
        std::vector<std::vector<float>> queue_priorities;
        if (!select_queues(*this, queue_infos, queue_priorities)) {
            LOG_ERROR("Failed to select proper Vulkan queues. This is likely a bug.");
            return false;
        }

        if (!physical_device.getSurfaceSupportKHR(
                general_family_index, screen_renderer.surface)) {
            LOG_ERROR("Failed to select a Vulkan queue that supports presentation. This is likely a bug.");
            return false;
        }

        features.support_clip_distance = enable_depth_clamp
            && static_cast<bool>(physical_device_features.depthClamp)
            && static_cast<bool>(physical_device_features.shaderClipDistance);

        // use these features (because they are used by the vita GPU) if they are available
        vk::PhysicalDeviceFeatures enabled_features{
            .independentBlend = physical_device_features.independentBlend,
            .depthClamp = features.support_clip_distance ? VK_TRUE : VK_FALSE,
            .fillModeNonSolid = physical_device_features.fillModeNonSolid,
            .wideLines = physical_device_features.wideLines,
            .samplerAnisotropy = physical_device_features.samplerAnisotropy,
            .occlusionQueryPrecise = physical_device_features.occlusionQueryPrecise,
            .fragmentStoresAndAtomics = physical_device_features.fragmentStoresAndAtomics,
            .shaderStorageImageExtendedFormats = physical_device_features.shaderStorageImageExtendedFormats,
            .shaderClipDistance = features.support_clip_distance ? VK_TRUE : VK_FALSE,
            .shaderInt16 = physical_device_features.shaderInt16,
        };

        // look for optional extensions
        std::vector<const char *> device_extensions(required_device_extensions);
        bool temp_bool;
        bool support_global_priority = false;
        bool support_buffer_device_address = false;
        bool support_external_memory = false;
        bool support_shader_interlock = false;
#ifdef __SWITCH__
        bool support_timeline_semaphore = false;
#endif
        const std::map<std::string_view, bool *> optional_extensions = {
            { vk::KHRGetMemoryRequirements2ExtensionName, &temp_bool },
            // can be used by vma to improve performance
            { vk::KHRDedicatedAllocationExtensionName, &support_dedicated_allocations },
            // used to tell the driver this application is high priority
            { vk::EXTGlobalPriorityExtensionName, &support_global_priority },
            // can be used to specify which format will be used by mutable images
            { vk::KHRImageFormatListExtensionName, &surface_cache.support_image_format_specifier },
            { vk::KHRExternalMemoryExtensionName, &temp_bool },
            { vk::KHRDeviceGroupExtensionName, &temp_bool },
            // can host memory directly be used for gxm memory
            { vk::EXTExternalMemoryHostExtensionName, &support_external_memory },
            // also needed for reading mapped memory in the shader
            { vk::KHRBufferDeviceAddressExtensionName, &support_buffer_device_address },
            // needed for uniform uvec2 arrays not to take twice the size
            { vk::KHRUniformBufferStandardLayoutExtensionName, &support_standard_layout },
#ifdef __SWITCH__
            // LSFG's internal scheduling uses timeline semaphores. On Vulkan
            // 1.0/1.1 drivers this extension must be enabled explicitly.
            { vk::KHRTimelineSemaphoreExtensionName, &support_timeline_semaphore },
#endif
            // needed for FSR
            { vk::KHRShaderFloat16Int8ExtensionName, &support_fsr },
            // used for accurate programmable blending on desktop GPUs
            { vk::EXTFragmentShaderInterlockExtensionName, &support_shader_interlock },
#ifdef __APPLE__
            // Needed to create the MoltenVK device
            { vk::KHRPortabilitySubsetExtensionName, &temp_bool },
#endif
            // used for coherent framebuffer fetch
            { VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, &support_rasterized_order_access },
#ifdef __ANDROID__
            // dependencies of VK_ANDROID_external_memory_android_hardware_buffer
            { VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, &temp_bool },
            { VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, &temp_bool },
            { VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, &temp_bool },
            // used for memory trapping in android
            { VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, &support_android_buffer_import },
            { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, &support_unix_fd_import },
#endif
        };

        for (const vk::ExtensionProperties &ext : physical_device.enumerateDeviceExtensionProperties()) {
            auto it = optional_extensions.find(ext.extensionName.data());
            if (it != optional_extensions.end()) {
                // this extension is available on the GPU
                *it->second = true;
                device_extensions.push_back(it->first.data());
            }
        }

#ifdef __SWITCH__
        support_timeline_semaphore = support_timeline_semaphore
            || physical_device_properties.apiVersion >= VK_API_VERSION_1_2;
        if (lsfg::is_session_prepared()) {
            if (!support_timeline_semaphore) {
                lsfg::disable_session("Timeline semaphores are unavailable on this Vulkan device");
            } else {
                const auto timeline_features = physical_device.getFeatures2<
                    vk::PhysicalDeviceFeatures2,
                    vk::PhysicalDeviceTimelineSemaphoreFeatures>();
                if (!timeline_features.get<vk::PhysicalDeviceTimelineSemaphoreFeatures>().timelineSemaphore)
                    lsfg::disable_session("Timeline semaphores are unavailable on this Vulkan device");
            }
        }
#endif

        bool support_memory_mapping = true;
        if (support_buffer_device_address) {
            auto features = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceBufferDeviceAddressFeatures>();
            support_buffer_device_address &= static_cast<bool>(features.get<vk::PhysicalDeviceBufferDeviceAddressFeatures>().bufferDeviceAddress);
        }
        support_memory_mapping &= support_buffer_device_address;

        if (support_standard_layout) {
            auto features = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceUniformBufferStandardLayoutFeatures>();
            support_standard_layout &= static_cast<bool>(features.get<vk::PhysicalDeviceUniformBufferStandardLayoutFeatures>().uniformBufferStandardLayout);
        }
        support_memory_mapping &= support_standard_layout;

#ifdef __APPLE__
        // we need to make a copy of the vertex buffer for moltenvk, so disable memory mapping
        support_memory_mapping = false;
#endif

#ifdef __ANDROID__
        support_android_buffer_import &= SDL_GetAndroidSDKVersion() >= 26;
        support_unix_fd_import &= SDL_GetAndroidSDKVersion() >= 26;
#endif

        // Find which memory mapping methods are supported by the GPU
        supported_mapping_methods_mask = (1 << static_cast<int>(MappingMethod::Disabled));
        if (support_memory_mapping) {
            // No additional check needed for these methods
            mapping_method = MappingMethod::DoubleBuffer;
            supported_mapping_methods_mask |= (1 << static_cast<int>(MappingMethod::DoubleBuffer));
#ifndef __SWITCH__
            // PageTable requires HOST_COHERENT, the one type NVK never
            // cache-maintains on Tegra; External Host covers Switch instead.
            supported_mapping_methods_mask |= (1 << static_cast<int>(MappingMethod::PageTable));
#endif

            if (support_external_memory) {
                // disable this extension on GPUs with an alignment requirement higher than 4096 (should only
                // concern a few intel iGPUs)
                auto props = physical_device.getProperties2KHR<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>();
                support_external_memory = (props.get<vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>().minImportedHostPointerAlignment <= 4096);
            }

            if (support_external_memory)
                supported_mapping_methods_mask |= (1 << static_cast<int>(MappingMethod::ExernalHost));

#ifdef __ANDROID__
            if (support_android_buffer_import || support_unix_fd_import)
                supported_mapping_methods_mask |= (1 << static_cast<int>(MappingMethod::NativeBuffer));
#endif
        }

        if (physical_device_properties.vendorID == 4318) {
            // Nvidia does not allow us to set the device priority higher than normal
            // no need to remove the priority extension
            support_global_priority = false;
        }
        // this is an emulator, tell the system it should have a high priority
        const vk::DeviceQueueGlobalPriorityCreateInfoEXT queue_priority{
            .globalPriority = vk::QueueGlobalPriorityEXT::eHigh
        };
        if (support_global_priority) {
            // add queue_priority to each queue creation info
            for (auto &queue_info : queue_infos) {
                queue_info.pNext = &queue_priority;
            }
        }

        support_fsr &= static_cast<bool>(physical_device_features.shaderInt16);
        if (support_fsr) {
            // double check for FP16 support
            auto props = physical_device.getFeatures2KHR<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceShaderFloat16Int8Features>();
            support_fsr = static_cast<bool>(props.get<vk::PhysicalDeviceShaderFloat16Int8Features>().shaderFloat16);
        }

        if (support_rasterized_order_access) {
            auto props = physical_device.getFeatures2KHR<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT>();
            support_rasterized_order_access = static_cast<bool>(props.get<vk::PhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT>().rasterizationOrderColorAttachmentAccess);
            // although both should never be supported at the same time, rasterized order access is far better than shader interlock
            support_shader_interlock = false;
        }

        // An integer attachment preserves F16 NaN payloads lost during float conversion.
        // Its blend state must be independent of the float attachment.
        features.preserve_f16_nan_as_u16 = static_cast<bool>(physical_device_features.independentBlend);

        support_shader_interlock &= static_cast<bool>(physical_device_features.fragmentStoresAndAtomics);
        if (support_shader_interlock) {
            auto props = physical_device.getFeatures2KHR<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT>();
            support_shader_interlock = static_cast<bool>(props.get<vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT>().fragmentShaderSampleInterlock);
            features.support_shader_interlock = support_shader_interlock;
        }

        vk::StructureChain<vk::DeviceCreateInfo,
            vk::PhysicalDeviceBufferDeviceAddressFeatures,
            vk::PhysicalDeviceUniformBufferStandardLayoutFeatures,
            vk::PhysicalDeviceShaderFloat16Int8Features,
            vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT,
            vk::PhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT,
            vk::PhysicalDeviceTimelineSemaphoreFeatures>
            device_info{
                vk::DeviceCreateInfo{
                    .pEnabledFeatures = &enabled_features },
                vk::PhysicalDeviceBufferDeviceAddressFeatures{
                    .bufferDeviceAddress = VK_TRUE },
                vk::PhysicalDeviceUniformBufferStandardLayoutFeatures{
                    .uniformBufferStandardLayout = VK_TRUE },
                vk::PhysicalDeviceShaderFloat16Int8Features{
                    // FSR uses float16
                    .shaderFloat16 = VK_TRUE },
                vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT{
                    .fragmentShaderSampleInterlock = VK_TRUE },
                vk::PhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT{
                    .rasterizationOrderColorAttachmentAccess = VK_TRUE },
                vk::PhysicalDeviceTimelineSemaphoreFeatures{
                    .timelineSemaphore = VK_TRUE }
            };
        device_info.get().setQueueCreateInfos(queue_infos);
        device_info.get().setPEnabledExtensionNames(device_extensions);

        if (!support_memory_mapping)
            device_info.unlink<vk::PhysicalDeviceBufferDeviceAddressFeatures>();

        if (!support_standard_layout)
            device_info.unlink<vk::PhysicalDeviceUniformBufferStandardLayoutFeatures>();

        if (!support_rasterized_order_access)
            device_info.unlink<vk::PhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT>();

        if (!support_fsr)
            device_info.unlink<vk::PhysicalDeviceShaderFloat16Int8Features>();

        if (!support_shader_interlock)
            device_info.unlink<vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT>();

#ifdef __SWITCH__
        if (!lsfg::is_session_prepared())
#endif
            device_info.unlink<vk::PhysicalDeviceTimelineSemaphoreFeatures>();

        try {
            device = physical_device.createDevice(device_info.get());
        } catch (vk::NotPermittedError &) {
            // according to the vk spec, when using a priority higher than medium
            // we can get this error (although I think it will only possibly happen
            // for realtime priority)
            for (auto &queue_info : queue_infos) {
                queue_info.pNext = nullptr;
            }
            device = physical_device.createDevice(device_info.get());
        }
        VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
    }

    // Get Queues
    general_queue = device.getQueue(general_family_index, 0);
    transfer_queue = device.getQueue(transfer_family_index, 0);

    // Create Command Pools
    {
        vk::CommandPoolCreateInfo general_pool_info{
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer, // Flags
            .queueFamilyIndex = general_family_index // Queue Family Index
        };

        vk::CommandPoolCreateInfo transfer_pool_info{
            .flags = vk::CommandPoolCreateFlagBits::eTransient, // Flags
            .queueFamilyIndex = transfer_family_index // Queue Family Index
        };

        general_command_pool = device.createCommandPool(general_pool_info);
        transfer_command_pool = device.createCommandPool(transfer_pool_info);

        general_pool_info.flags |= vk::CommandPoolCreateFlagBits::eTransient;
        one_time_command_pool = device.createCommandPool(general_pool_info);
        multithread_command_pool = device.createCommandPool(general_pool_info);
    }

    // Allocate Memory for Images and Buffers
    {
        vma::VulkanFunctions vulkan_functions{
            .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr
        };

        vma::AllocatorCreateInfo allocator_info = {
            // everything vma-related is done on one thread, no need for thread safety
            .flags = vma::AllocatorCreateFlagBits::eExternallySynchronized,
            .physicalDevice = physical_device,
            .device = device,
            .pVulkanFunctions = &vulkan_functions,
            .instance = instance,
            .vulkanApiVersion = VK_API_VERSION_1_0,
        };

        if (support_dedicated_allocations)
            allocator_info.flags |= vma::AllocatorCreateFlagBits::eKhrDedicatedAllocation;

        // if memory mapping is supported
        if (supported_mapping_methods_mask > 1)
            allocator_info.flags |= vma::AllocatorCreateFlagBits::eBufferDeviceAddress;

        allocator = vma::createAllocator(allocator_info);
        vkutil::init(allocator);
    }

    // create the default image and buffer
    {
        default_buffer = vkutil::Buffer(KiB(4));
        default_buffer.init_buffer(vk::BufferUsageFlagBits::eVertexBuffer);

        // create the default image, it must be cleared then transitioned
        default_image = vkutil::Image(1, 1, vk::Format::eR8G8B8A8Unorm);

        default_image.init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
        auto one_time_command = create_one_time_command();
        const vk::CommandBuffer cmd_buffer = one_time_command.buffer;
        default_image.transition_to(cmd_buffer, vkutil::ImageLayout::TransferDst);
        // make it white
        vk::ClearColorValue white{
            .float32 = std::array<float, 4>{ 1.0f, 1.0f, 1.0f, 1.0f }
        };
        cmd_buffer.clearColorImage(default_image.image, vk::ImageLayout::eTransferDstOptimal, white, vkutil::color_subresource_range);
        default_image.transition_to(cmd_buffer, vkutil::ImageLayout::StorageImage);

        if (features.preserve_f16_nan_as_u16) {
            default_raw_image = vkutil::Image(1, 1, vk::Format::eR16G16B16A16Uint);
            default_raw_image.init_image(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst);
            default_raw_image.transition_to(cmd_buffer, vkutil::ImageLayout::StorageImage);
        }
        submit_one_time_command(std::move(one_time_command));

        // create the default sampler
        vk::SamplerCreateInfo sampler_info{
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = vk::SamplerAddressMode::eRepeat,
            .addressModeV = vk::SamplerAddressMode::eRepeat,
            .addressModeW = vk::SamplerAddressMode::eRepeat,
            .minLod = 0.0f,
            .maxLod = 0.0f,
        };
        default_image.sampler = device.createSampler(sampler_info);
    }

    // create the frame objects
    for (int i = 0; i < MAX_FRAMES_RENDERING; i++) {
        FrameObject &frame = frames[i];

        vk::CommandPoolCreateInfo pool_info{
            .queueFamilyIndex = general_family_index
        };

        frame.render_pool = device.createCommandPool(pool_info);
        pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        frame.prerender_pool = device.createCommandPool(pool_info);

        frame.destroy_queue.init(device);
    }

    if (!screen_renderer.setup())
        return false;

    init_overlay_font_dirs();

    if (!overlay_renderer.init(*this)) {
        LOG_WARN("Failed to initialize Vulkan overlay renderer, overlays will be disabled");
    }

    support_fsr &= screen_renderer.swapchain_supports_fsr;

    return true;
}

void VKState::late_init(const Config &cfg, const std::string_view game_id, MemState &mem) {
    this->mem = &mem;
    shader_debug_dump = cfg.current_config.shader_debug_dump;

    bool use_high_accuracy = cfg.current_config.high_accuracy;

    // shader interlock is more accurate but slower
    if (features.support_shader_interlock && use_high_accuracy) {
        LOG_INFO("Using shader interlock for accurate framebuffer fetch emulation");
    } else {
        // We use subpass input to get something similar to direct fragcolor access (there is no difference for the shader)
        features.direct_fragcolor = true;
        features.support_shader_interlock = false;
    }

    // texture viewport is faster but not entirely accurate
    if (support_standard_layout && !use_high_accuracy) {
        LOG_INFO("The Vulkan renderer is using texture viewport for better performance");
        features.use_texture_viewport = true;
    }

    // parse the mapping method
    auto &config_mapping = cfg.current_config.memory_mapping;
    MappingMethod request_mapping = MappingMethod::Disabled;
    if (config_mapping == "double-buffer")
        request_mapping = MappingMethod::DoubleBuffer;
    else if (config_mapping == "external-host")
        request_mapping = MappingMethod::ExernalHost;
    else if (config_mapping == "page-table")
        request_mapping = MappingMethod::PageTable;
#ifdef __ANDROID__
    else if (config_mapping == "native-buffer")
        request_mapping = MappingMethod::NativeBuffer;
#endif
    const std::string_view mapping_string[] = { "Disabled", "Double buffer", "External Host", "Page Table", "Native Buffer" };

    if ((1 << static_cast<int>(request_mapping)) & supported_mapping_methods_mask) {
        // we support the requested mapping method
        mapping_method = request_mapping;
    } else {
        LOG_WARN("Requested memory mapping method {} is not supported by {}; using {} instead",
            mapping_string[static_cast<int>(request_mapping)],
            physical_device_properties.deviceName.data(),
            mapping_string[static_cast<int>(mapping_method)]);
    }

    features.enable_memory_mapping = mapping_method != MappingMethod::Disabled;

#ifdef __ANDROID__
    if (mapping_method == MappingMethod::NativeBuffer) {
        // dynamically load the symbols
        void *libandroid = dlopen("libandroid.so", RTLD_LAZY);
        _AHardwareBuffer_getNativeHandle = reinterpret_cast<decltype(_AHardwareBuffer_getNativeHandle)>(dlsym(libandroid, "AHardwareBuffer_getNativeHandle"));
        _AHardwareBuffer_allocate = reinterpret_cast<decltype(_AHardwareBuffer_allocate)>(dlsym(libandroid, "AHardwareBuffer_allocate"));
        _AHardwareBuffer_lock = reinterpret_cast<decltype(_AHardwareBuffer_lock)>(dlsym(libandroid, "AHardwareBuffer_lock"));
        _AHardwareBuffer_unlock = reinterpret_cast<decltype(_AHardwareBuffer_unlock)>(dlsym(libandroid, "AHardwareBuffer_unlock"));
        _AHardwareBuffer_release = reinterpret_cast<decltype(_AHardwareBuffer_release)>(dlsym(libandroid, "AHardwareBuffer_release"));
    }
#endif

#ifdef __SWITCH__
    // Keep the effective settings visible in normal Switch crash logs. The
    // launcher's default warning log level otherwise filters the corresponding
    // renderer and pipeline-cache informational messages.
    LOG_WARN("Switch Vulkan runtime: memory mapping={}, async pipeline compilation={}, surface sync={}",
        mapping_string[static_cast<int>(mapping_method)],
        cfg.current_config.async_pipeline_compilation ? "enabled" : "disabled",
        cfg.current_config.disable_surface_sync ? "disabled" : "enabled");
#else
    LOG_INFO("Using the following memory mapping method: {}", mapping_string[static_cast<int>(mapping_method)]);
#endif

#if defined(__linux__) && !defined(__ANDROID__) // According to my tests (Macdu), mprotect on buffers (mapped with external memory host) only works with Nvidia drivers
    surface_cache.can_mprotect_mapped_memory = mapping_method == MappingMethod::DoubleBuffer
        || std::string_view(physical_device_properties.deviceName).contains("NVIDIA");
#endif

    pipeline_cache.init(support_rasterized_order_access);

#ifdef __SWITCH__
    // Horizon cannot resume page faults from generated guest code, so retain the
    // established hash-based texture invalidation path on Switch.
    texture_cache.init(false, texture_folder(), game_id);
#else
    texture_cache.init(true, texture_folder(), game_id);
#endif
}

void VKState::cleanup() {
    const auto release_descriptor_sets = [](FrameDescriptor &descriptor) {
        std::vector<vk::DescriptorSet>().swap(descriptor.sets);
        descriptor.descriptors_idx = 0;
    };

    wait_device_idle();

#ifdef __SWITCH__
    // LSFG owns Vulkan objects borrowed from this device. Do not release them
    // until every generated/original presentation has completed.
    lsfg::end_session();
#endif

    request_queue.abort();

    context = nullptr;

    for (int i = 0; i < MAX_FRAMES_RENDERING; i++) {
        frames[i].rendered_fences.clear();
        for (auto &descriptor : frames[i].vert_descriptors)
            release_descriptor_sets(descriptor);
        for (auto &descriptor : frames[i].frag_descriptors)
            release_descriptor_sets(descriptor);
        release_descriptor_sets(frames[i].color_descriptor);
    }

    pipeline_cache.cleanup();

    for (int i = 0; i < MAX_FRAMES_RENDERING; i++)
        frames[i].destroy_queue.destroy_objects();

    screen_renderer.cleanup();

    overlay_renderer.destroy();

    surface_cache.cleanup();

    texture_cache.cleanup();

    for (auto &[addr, mapping] : mapped_memories) {
        if (auto *ext = std::get_if<ExternalBuffer>(&mapping.buffer_impl)) {
            device.destroyBuffer(mapping.buffer);
            device.freeMemory(ext->memory);
#ifdef __ANDROID__
            if (mapping_method == MappingMethod::NativeBuffer && ext->extra) {
                AHardwareBuffer *hardware_buffer = reinterpret_cast<AHardwareBuffer *>(ext->extra);
                _AHardwareBuffer_unlock(hardware_buffer, nullptr);
                if (support_android_buffer_import)
                    _AHardwareBuffer_release(hardware_buffer);
            }
#endif
        }
    }
    mapped_memories.clear();
    buffer_trapping.trapped_buffers.clear();

    default_image.destroy();
    default_raw_image.destroy();
    default_buffer.destroy();

    for (auto &pool : frame_descriptor_pools)
        device.destroy(pool);
    frame_descriptor_pools.clear();

    for (int i = 0; i < MAX_FRAMES_RENDERING; i++) {
        device.destroy(frames[i].render_pool);
        frames[i].render_pool = nullptr;
        device.destroy(frames[i].prerender_pool);
        frames[i].prerender_pool = nullptr;
    }

    device.destroy(general_command_pool);
    general_command_pool = nullptr;
    device.destroy(one_time_command_pool);
    one_time_command_pool = nullptr;
    device.destroy(transfer_command_pool);
    transfer_command_pool = nullptr;
    device.destroy(multithread_command_pool);
    multithread_command_pool = nullptr;

    allocator.destroy();

    vkutil::deinit();

    device.destroy();

    if (debug_messenger) {
        instance.destroyDebugUtilsMessengerEXT(debug_messenger);
        debug_messenger = nullptr;
    }
    if (debug_report) {
        instance.destroyDebugReportCallbackEXT(debug_report);
        debug_report = nullptr;
    }

    instance.destroy();

    gxp_ptr_map.clear();
    shaders_cache_hashs.clear();
    request_queue.reset();
    current_frame_idx = 1;
    last_scene_id = 0;
    shaders_count_compiled = 0;
    programs_count_pre_compiled = 0;
    should_display = false;
    render_abort = false;
}

void VKState::render_frame(DisplayState &display, const GxmState &gxm, MemState &mem) {
    // we are displaying this frame, wait for a new one
    should_display = false;

    DisplayFrameInfo frame;
    {
        std::lock_guard<std::mutex> guard(display.display_info_mutex);
        frame = display.next_rendered_frame;
    }

    update_overlays();
    bool has_overlays = false;
    if (overlay_manager) {
        overlay_manager->lock_shared();
        has_overlays = overlay_manager->has_visible();
        overlay_manager->unlock_shared();
    }

    if (!frame.base && !has_overlays)
        return;

    if (!screen_renderer.acquire_swapchain_image())
        return;

    // store viewport for touch
    {
        const float fb_w = static_cast<float>(screen_renderer.extent.width);
        const float fb_h = static_cast<float>(screen_renderer.extent.height);
        display.viewport_drawable_w = static_cast<int>(fb_w);
        display.viewport_drawable_h = static_cast<int>(fb_h);
        if (fb_h > 0.0f) {
            const float window_aspect = fb_w / fb_h;
            constexpr float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
            const bool pixel_perfect = fullscreen_hd_res_pixel_perfect && fullscreen
                && !(screen_renderer.extent.width % DEFAULT_RES_WIDTH)
                && !(screen_renderer.extent.height % (DEFAULT_RES_HEIGHT - 4));
            if (stretch_the_display_area && !pixel_perfect) {
                display.viewport_x = 0.0f;
                display.viewport_y = 0.0f;
                display.viewport_w = fb_w;
                display.viewport_h = fb_h;
            } else if ((window_aspect > vita_aspect) && !pixel_perfect) {
                display.viewport_w = fb_h * vita_aspect;
                display.viewport_h = fb_h;
                display.viewport_x = (fb_w - display.viewport_w) / 2.0f;
                display.viewport_y = 0.0f;
            } else {
                display.viewport_w = fb_w;
                display.viewport_h = fb_w / vita_aspect;
                display.viewport_x = 0.0f;
                display.viewport_y = (fb_h - display.viewport_h) / 2.0f;
            }
        }
    }

    if (has_overlays && screen_renderer.current_cmd_buffer) {
        overlay_renderer.prepare(screen_renderer.current_cmd_buffer,
            *overlay_manager,
            display.viewport_w, display.viewport_h,
            screen_renderer.swapchain_image_idx);
    }

    if (frame.base) {
        // Check if the surface exists
        Viewport viewport;
        viewport.width = static_cast<uint32_t>(frame.image_size.x * res_multiplier);
        viewport.height = static_cast<uint32_t>(frame.image_size.y * res_multiplier);

        vk::ImageLayout layout = vk::ImageLayout::eGeneral;
        vk::ImageView surface_handle = surface_cache.sourcing_color_surface_for_presentation(
            frame.base, frame.pitch, viewport);

        if (!surface_handle) {
            vkutil::Image &vita_surface = screen_renderer.vita_surface[screen_renderer.swapchain_image_idx];
            if (frame.image_size.x != vita_surface.width || frame.image_size.y != vita_surface.height) {
                // re-create the image
                vita_surface.destroy();
                vita_surface = vkutil::Image(frame.image_size.x, frame.image_size.y, vk::Format::eR8G8B8A8Unorm);
                vita_surface.init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
            }

            // copy surface to staging buffer
            const vk::DeviceSize texture_data_size = frame.pitch * frame.image_size.y * 4;
            memcpy(screen_renderer.vita_surface_staging_info.pMappedData, frame.base.get(mem), texture_data_size);

            // copy staging buffer to image
            auto &cmd_buffer = screen_renderer.current_cmd_buffer;
            vita_surface.transition_to_discard(cmd_buffer, vkutil::ImageLayout::TransferDst);
            vk::BufferImageCopy region{
                .bufferOffset = 0,
                .bufferRowLength = frame.pitch,
                .bufferImageHeight = static_cast<uint32_t>(frame.image_size.y),
                .imageSubresource = vkutil::color_subresource_layer,
                .imageOffset = { 0, 0, 0 },
                .imageExtent = { static_cast<uint32_t>(frame.image_size.x), static_cast<uint32_t>(frame.image_size.y), 1 }
            };
            cmd_buffer.copyBufferToImage(screen_renderer.vita_surface_staging, vita_surface.image, vk::ImageLayout::eTransferDstOptimal, region);

            vita_surface.transition_to(cmd_buffer, vkutil::ImageLayout::SampledImage);

            surface_handle = vita_surface.view;
            viewport = {
                .offset_x = 0,
                .offset_y = 0,
                .width = static_cast<uint32_t>(frame.image_size.x),
                .height = static_cast<uint32_t>(frame.image_size.y),
                .texture_width = static_cast<uint32_t>(frame.image_size.x),
                .texture_height = static_cast<uint32_t>(frame.image_size.y)
            };
            layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        screen_renderer.render(surface_handle, layout, viewport);
    } else if (has_overlays) {
        screen_renderer.begin_default_render_pass();
    }

    if (has_overlays && screen_renderer.current_cmd_buffer) {
        overlay_renderer.render(screen_renderer.current_cmd_buffer,
            screen_renderer.default_render_pass,
            screen_renderer.extent,
            display.viewport_x, display.viewport_y,
            display.viewport_w, display.viewport_h);
    }
}

void VKState::swap_window() {
    screen_renderer.swap_window();

    // look once a frame if we need to save the pipeline cache
    const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (time_s >= pipeline_cache.next_pipeline_cache_save) {
        pipeline_cache.save_pipeline_cache();

        pipeline_cache.next_pipeline_cache_save = std::numeric_limits<uint64_t>::max();
    }
}

std::vector<uint32_t> VKState::dump_frame(DisplayState &display, uint32_t &width, uint32_t &height) {
    DisplayFrameInfo frame;
    {
        std::lock_guard<std::mutex> guard(display.display_info_mutex);
        frame = display.next_rendered_frame;
    }

    width = static_cast<uint32_t>(frame.image_size.x * res_multiplier);
    height = static_cast<uint32_t>(frame.image_size.y * res_multiplier);
    return surface_cache.dump_frame(frame.base, width, height, frame.pitch);
}

uint32_t VKState::get_features_mask() {
    return (uint32_t(features.support_shader_interlock) << 0)
        | (uint32_t(features.use_texture_viewport) << 1)
        | (uint32_t(features.enable_memory_mapping) << 2)
        | (uint32_t(features.support_rgb_attributes) << 3)
        | (uint32_t(pipeline_cache.support_scaled_vertex_attribute) << 4)
        | (uint32_t(features.preserve_f16_nan_as_u16) << 5)
        | (uint32_t(features.use_mask_bit) << 6)
        | (uint32_t(features.direct_fragcolor) << 7)
        | (uint32_t(features.support_texture_barrier) << 8)
        | (uint32_t(features.support_unknown_format) << 9)
        | (uint32_t(features.support_clip_distance) << 10);
}

int VKState::get_supported_filters() {
    int filters = static_cast<int>(Filter::NEAREST) | static_cast<int>(Filter::BILINEAR) | static_cast<int>(Filter::BICUBIC) | static_cast<int>(Filter::FXAA);
    if (support_fsr)
        filters |= static_cast<int>(Filter::FSR);
    return filters;
}

void VKState::set_screen_filter(const std::string_view &filter) {
    if (filter == "FSR" && !support_fsr) {
        LOG_WARN("Trying to enable FSR but the GPU does not support it");
        renderer::send_single_command(*this, nullptr, renderer::CommandOpcode::SetScreenFilter, false, new std::string());
        return;
    }

    renderer::send_single_command(*this, nullptr, renderer::CommandOpcode::SetScreenFilter, false, new std::string(filter));
}

#ifdef __SWITCH__
// A mapping spanning several pool runs (not one contiguous host range) must not be imported.
static bool switch_range_alias_is_flat(const MemState &mem, Address addr, uint32_t size) {
    const uint8_t *const base = Ptr<uint8_t>(addr).get(mem);
    if (!base)
        return false;
    // 64-bit so a mapping ending at the 4 GiB boundary does not wrap.
    const uint64_t end = static_cast<uint64_t>(addr) + size;
    for (uint64_t page = align(static_cast<uint64_t>(addr) + 1, KiB(4)); page < end; page += KiB(4)) {
        if (Ptr<uint8_t>(static_cast<Address>(page)).get(mem) != base + (page - addr))
            return false;
    }
    return true;
}
#endif

bool VKState::map_memory(MemState &mem, Ptr<void> address, uint32_t size) {
    assert(features.enable_memory_mapping);
    // the address should be 4K aligned
    assert((address.address() & 4095) == 0);
    constexpr vk::BufferUsageFlags mapped_memory_flags = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst;

    auto find_mem_type_with_flag = [&](const vk::MemoryPropertyFlags flags, uint32_t hardware_types) {
        while (hardware_types != 0) {
            // try to find a cached memory type
            int mapped_memory_type = std::countr_zero(hardware_types);
            hardware_types -= (1 << mapped_memory_type);

            if ((physical_device_memory.memoryTypes[mapped_memory_type].propertyFlags & flags) == flags)
                return mapped_memory_type;
        }
        return -1;
    };

    auto find_suitable_mapped_type = [&](uint32_t hardware_types) {
        // first try to find a memory that is both coherent and cached
        int mapped_memory_type = find_mem_type_with_flag(vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eHostCached, hardware_types);
        if (mapped_memory_type == -1)
            // then only coherent (lower performance)
            mapped_memory_type = find_mem_type_with_flag(vk::MemoryPropertyFlagBits::eHostCoherent, hardware_types);

        if (mapped_memory_type == -1) {
            static bool has_happened = false;
            LOG_CRITICAL_IF(!has_happened, "No coherent memory available for memory mapping!");
            has_happened = true;
            mapped_memory_type = std::countr_zero(hardware_types);
        }

        return static_cast<uint32_t>(mapped_memory_type);
    };

    switch (mapping_method) {
    case MappingMethod::NativeBuffer: {
#ifdef __ANDROID__
        // if we get there, this means we support the hardware buffer extension
        AHardwareBuffer_Desc buffer_desc{
            .width = static_cast<uint32_t>(size + KiB(4)),
            .height = 1,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_BLOB,
            .usage = AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_MASK | AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK,
        };
        AHardwareBuffer *buffer;
        int err = _AHardwareBuffer_allocate(&buffer_desc, &buffer);
        if (err != 0) {
            LOG_ERROR("Failed to allocate Android hardware buffer, error {}", err);
            return false;
        }
        void *mapped_location;
        err = _AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_MASK | AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK, -1, nullptr, &mapped_location);
        if (err != 0) {
            LOG_ERROR("Failed to lock Android hardware buffer, error {}", err);
            return false;
        }

        vk::DeviceMemory device_memory;
        // prefer this extension
        if (support_android_buffer_import) {
            const vk::AndroidHardwareBufferPropertiesANDROID hardware_props = device.getAndroidHardwareBufferPropertiesANDROID(*buffer);

            uint32_t mapped_memory_type = find_suitable_mapped_type(hardware_props.memoryTypeBits);
            vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportAndroidHardwareBufferInfoANDROID, vk::MemoryAllocateFlagsInfo> alloc_info{
                vk::MemoryAllocateInfo{
                    .allocationSize = size + KiB(4),
                    .memoryTypeIndex = mapped_memory_type },
                vk::ImportAndroidHardwareBufferInfoANDROID{
                    .buffer = buffer },
                vk::MemoryAllocateFlagsInfo{
                    .flags = vk::MemoryAllocateFlagBits::eDeviceAddress }
            };
            device_memory = device.allocateMemory(alloc_info.get());
        } else {
            const native_handle_t *handle = _AHardwareBuffer_getNativeHandle(buffer);
            if (handle == nullptr || handle->numFds == 0 || handle->data[0] == -1) {
                LOG_ERROR("Failed to get native handle");
                return false;
            }

            int fd = handle->data[0];
            const vk::MemoryFdPropertiesKHR fd_props = device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd, fd);
            uint32_t mapped_memory_type = find_suitable_mapped_type(fd_props.memoryTypeBits);
            vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryFdInfoKHR, vk::MemoryAllocateFlagsInfo> alloc_info{
                vk::MemoryAllocateInfo{
                    .allocationSize = size + KiB(4),
                    .memoryTypeIndex = mapped_memory_type },
                vk::ImportMemoryFdInfoKHR{
                    .handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd,
                    .fd = fd },
                vk::MemoryAllocateFlagsInfo{
                    .flags = vk::MemoryAllocateFlagBits::eDeviceAddress }
            };
            device_memory = device.allocateMemory(alloc_info.get());
        }

        vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfoKHR> buffer_info{
            vk::BufferCreateInfo{
                .size = size + KiB(4),
                .usage = mapped_memory_flags,
                .sharingMode = vk::SharingMode::eExclusive },
            vk::ExternalMemoryBufferCreateInfoKHR{
                .handleTypes = support_android_buffer_import ? vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID : vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd }
        };
        const vk::Buffer mapped_buffer = device.createBuffer(buffer_info.get());
        device.bindBufferMemory(mapped_buffer, device_memory, 0);

        vk::BufferDeviceAddressInfoKHR address_info{
            .buffer = mapped_buffer
        };
        const uint64_t buffer_address = device.getBufferAddress(address_info);

        add_external_mapping(mem, address.address(), size, reinterpret_cast<uint8_t *>(mapped_location));
        mapped_memories[address.address()] = { address.address(), ExternalBuffer{ device_memory, buffer }, mapped_buffer, size, buffer_address };
#else
        LOG_CRITICAL("Native buffer is only supported on Android!\n");
#endif
        break;
    }
    case MappingMethod::PageTable: {
        // add 4 KiB because we can as an easy way to prevent crashes due to memory accesses right after the memory boundary
        // also make sure later the mapped address is 4K aligned
        vkutil::Buffer buffer(size + KiB(4));
        constexpr vma::AllocationCreateInfo memory_mapped_alloc = {
            .flags = vma::AllocationCreateFlagBits::eMapped | vma::AllocationCreateFlagBits::eHostAccessSequentialWrite,
            .usage = vma::MemoryUsage::eAutoPreferHost,
            .requiredFlags = vk::MemoryPropertyFlagBits::eHostCoherent,
            .preferredFlags = vk::MemoryPropertyFlagBits::eHostCached,
        };
        buffer.init_buffer(mapped_memory_flags, memory_mapped_alloc);
        const uint64_t buffer_ptr_val = std::bit_cast<uint64_t>(buffer.mapped_data);
        const uint64_t buffer_offset = align(buffer_ptr_val, KiB(4)) - buffer_ptr_val;
        auto *const mapped_start = std::bit_cast<uint8_t *>(buffer_ptr_val + buffer_offset);

        vk::BufferDeviceAddressInfoKHR address_info{
            .buffer = buffer.buffer
        };
        const uint64_t buffer_address = device.getBufferAddress(address_info) + buffer_offset;
        const vk::Buffer mapped_buffer = buffer.buffer;

        add_external_mapping(mem, address.address(), size, mapped_start);
        mapped_memories[address.address()] = { address.address(), std::move(buffer), mapped_buffer, size, buffer_address, static_cast<uint32_t>(buffer_offset) };
        break;
    }

    case MappingMethod::ExernalHost: {
        void *host_address = address.get(mem);
#ifdef __SWITCH__
        // The import pins these exact pages; anything not flat committed
        // guest memory gets the Double Buffer treatment instead.
        if (switch_external_mapping_broken
            || !is_valid_addr_range_size(mem, address.address(), size)
            || !switch_range_alias_is_flat(mem, address.address(), size)) {
            LOG_WARN("Mapping at {} ({} KiB) is not flat committed guest memory; double-buffering it",
                log_hex(address.address()), size / 1024);
            goto switch_double_buffer_fallback;
        }
        vk::DeviceMemory switch_import_memory{};
        vk::Buffer switch_import_buffer{};
        try {
#endif
        auto host_mem_props = device.getMemoryHostPointerPropertiesEXT(vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, host_address);
        assert(host_mem_props.memoryTypeBits != 0);

        int mapped_memory_type = -1;
        auto find_mem_type_with_flag = [&](const vk::MemoryPropertyFlags flags) {
            uint32_t host_mem_types = host_mem_props.memoryTypeBits;
            while (host_mem_types != 0) {
                // try to find a cached memory type
                mapped_memory_type = std::countr_zero(host_mem_types);
                host_mem_types -= (1 << mapped_memory_type);

                if ((physical_device_memory.memoryTypes[mapped_memory_type].propertyFlags & flags) == flags)
                    return;
            }

            mapped_memory_type = -1;
        };

        // first try to find a memory that is both coherent and cached
        find_mem_type_with_flag(vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eHostCached);
        if (mapped_memory_type == -1)
            // then only coherent (lower performance)
            find_mem_type_with_flag(vk::MemoryPropertyFlagBits::eHostCoherent);
#ifdef __SWITCH__
        // NVK reports imported host memory as cached/non-coherent.
        if (mapped_memory_type == -1)
            find_mem_type_with_flag(vk::MemoryPropertyFlagBits::eHostCached);
#endif

        if (mapped_memory_type == -1) {
            LOG_CRITICAL_ONCE("No coherent memory available for memory mapping, this may be caused by an old driver!");
            mapped_memory_type = std::countr_zero(host_mem_props.memoryTypeBits);
        }

        vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryHostPointerInfoEXT, vk::MemoryAllocateFlagsInfo> alloc_info{
            vk::MemoryAllocateInfo{
                .allocationSize = size,
                .memoryTypeIndex = static_cast<uint32_t>(mapped_memory_type) },
            vk::ImportMemoryHostPointerInfoEXT{
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
                .pHostPointer = host_address },
            vk::MemoryAllocateFlagsInfo{
                .flags = vk::MemoryAllocateFlagBits::eDeviceAddress }
        };
        const vk::DeviceMemory device_memory = device.allocateMemory(alloc_info.get());
#ifdef __SWITCH__
        switch_import_memory = device_memory;
#endif

        vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfoKHR> buffer_info{
            vk::BufferCreateInfo{
                .size = size,
                .usage = mapped_memory_flags,
                .sharingMode = vk::SharingMode::eExclusive },
            vk::ExternalMemoryBufferCreateInfoKHR{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT }
        };
        const vk::Buffer mapped_buffer = device.createBuffer(buffer_info.get());
#ifdef __SWITCH__
        switch_import_buffer = mapped_buffer;
#endif
        device.bindBufferMemory(mapped_buffer, device_memory, 0);

        vk::BufferDeviceAddressInfoKHR address_info{
            .buffer = mapped_buffer
        };
        const uint64_t buffer_address = device.getBufferAddress(address_info);

        mapped_memories[address.address()] = { address.address(), ExternalBuffer{ device_memory, host_address }, mapped_buffer, size, buffer_address };
#ifdef __SWITCH__
        static std::atomic_flag logged_import;
        if (!logged_import.test_and_set()) {
            LOG_INFO("External Host import active: guest={} size={} KiB memory_type={}; explicit CPU cache maintenance enabled",
                log_hex(address.address()), size / 1024, mapped_memory_type);
            if (auto logger = spdlog::default_logger())
                logger->flush();
        }
        } catch (const vk::SystemError &err) {
            if (switch_import_buffer)
                device.destroyBuffer(switch_import_buffer);
            if (switch_import_memory)
                device.freeMemory(switch_import_memory);
            // One refusal is proof enough; stop attempting imports.
            switch_external_mapping_broken = true;
            LOG_WARN("Host import of the mapping at {} failed ({}); double-buffering guest mappings from now on",
                log_hex(address.address()), err.what());
            goto switch_double_buffer_fallback;
        }
#endif
        break;
    }

#ifdef __SWITCH__
    switch_double_buffer_fallback:
#endif
    case MappingMethod::DoubleBuffer: {
        vkutil::Buffer buffer(size + KiB(4));
#ifdef __SWITCH__
        // NVK's two Tegra memory types are cached/non-coherent and
        // coherent-but-never-maintained. Double Buffer has explicit upload and
        // readback points, so use the cached type and synchronize those points
        // with vkFlush/InvalidateMappedMemoryRanges.
        constexpr vma::AllocationCreateInfo double_buffer_alloc = {
            .flags = vma::AllocationCreateFlagBits::eMapped | vma::AllocationCreateFlagBits::eHostAccessSequentialWrite,
            .usage = vma::MemoryUsage::eAutoPreferHost,
            .requiredFlags = vk::MemoryPropertyFlagBits::eHostVisible,
            .preferredFlags = vk::MemoryPropertyFlagBits::eHostCached,
        };
#else
        constexpr vma::AllocationCreateInfo double_buffer_alloc = {
            .flags = vma::AllocationCreateFlagBits::eMapped | vma::AllocationCreateFlagBits::eHostAccessSequentialWrite,
            .usage = vma::MemoryUsage::eAutoPreferHost,
            .requiredFlags = vk::MemoryPropertyFlagBits::eHostCoherent,
            .preferredFlags = vk::MemoryPropertyFlagBits::eHostCached,
        };
#endif
        buffer.init_buffer(mapped_memory_flags, double_buffer_alloc);

#ifdef __SWITCH__
        LOG_WARN_ONCE("Switch Double Buffer selected {} host memory; explicit cache synchronization is active",
            buffer.is_host_coherent() ? "coherent/unmaintained" : "cached/non-coherent");
#endif

        vk::BufferDeviceAddressInfoKHR address_info{
            .buffer = buffer.buffer
        };
        const uint64_t buffer_address = device.getBufferAddress(address_info);
        const vk::Buffer mapped_buffer = buffer.buffer;
        mapped_memories[address.address()] = { address.address(), std::move(buffer), mapped_buffer, size, buffer_address };
#ifdef __SWITCH__
        MappedMemory &mapping = mapped_memories.at(address.address());
        const size_t hash_block_count = (size + SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE - 1) / SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE;
        mapping.cpu_block_hashes.resize(hash_block_count);
        mapping.cpu_block_hash_valid.resize((hash_block_count + 63) / 64);
        mapping.cpu_block_scene.resize(hash_block_count);
#endif
        break;
    }

    default:
        LOG_CRITICAL("Mapping method not handled, report it to the devs!");
        break;
    }

    return true;
}

void VKState::unmap_memory(MemState &mem, Ptr<void> address) {
    assert(features.enable_memory_mapping);

    auto ite = mapped_memories.find(address.address());
    if (ite == mapped_memories.end()) {
        LOG_CRITICAL("Could not find mapped memory to erase");
        return;
    }

    // we need to wait in case the buffer is being used
    wait_device_idle();

    switch (mapping_method) {
    case MappingMethod::ExernalHost:
        if (auto *ext = std::get_if<ExternalBuffer>(&ite->second.buffer_impl)) {
            device.destroyBuffer(ite->second.buffer);
            device.freeMemory(ext->memory);
        }
#ifdef __SWITCH__
        else
            // A fallback mapping only has trapping state to clear here.
            buffer_trapping.remove_range(address.address(), address.address() + ite->second.size);
#endif
        break;

    case MappingMethod::DoubleBuffer:
#ifndef __SWITCH__
        // Desktop Double Buffer may have active mprotect ranges. Horizon uses
        // checksums instead and never registered an external page-table mapping;
        // calling remove_external_mapping there would assert on a missing entry.
        remove_external_mapping(mem, address.cast<uint8_t>().get(mem), ite->second.size);
#endif
        // remove all the trapping related to these locations
        buffer_trapping.remove_range(address.address(), address.address() + ite->second.size);
        break;

#ifdef __ANDROID__
    case MappingMethod::NativeBuffer: {
        remove_external_mapping(mem, address.cast<uint8_t>().get(mem), ite->second.size);
        device.destroyBuffer(ite->second.buffer);
        ExternalBuffer &buffer = std::get<ExternalBuffer>(ite->second.buffer_impl);
        device.freeMemory(buffer.memory);

        AHardwareBuffer *hardware_buffer = reinterpret_cast<AHardwareBuffer *>(buffer.extra);
        _AHardwareBuffer_unlock(hardware_buffer, nullptr);
        // When using external fd, it takes ownership of the handle, so don't release it in this case
        if (support_android_buffer_import)
            _AHardwareBuffer_release(hardware_buffer);
        break;
    }
#endif

    case MappingMethod::PageTable:
        remove_external_mapping(mem, address.cast<uint8_t>().get(mem), ite->second.size);
        break;

    default:
        LOG_CRITICAL("Mapping method not handled, report it to the devs!");
        break;
    }
    mapped_memories.erase(ite);
}

std::tuple<vk::Buffer, uint32_t> VKState::get_matching_mapping(const Ptr<void> address) {
    auto mapped_memory = mapped_memories.lower_bound(address.address());
    if (mapped_memory == mapped_memories.end()
        || mapped_memory->first + mapped_memory->second.size <= address.address()) {
        LOG_ERROR("Could not find matching mapped buffer for vertex stream");
        return { nullptr, 0 };
    }

    return std::make_tuple(mapped_memory->second.buffer,
        mapped_memory->second.buffer_offset + address.address() - mapped_memory->first);
}

std::tuple<uint64_t, int32_t, int32_t> VKState::get_matching_device_address(const Address address) {
    auto mapped_memory = mapped_memories.lower_bound(address);
    if (mapped_memory == mapped_memories.end()
        || static_cast<uint64_t>(mapped_memory->first) + mapped_memory->second.size <= address) {
        LOG_ERROR("Could not find matching mapped buffer for vertex stream");
        return { 0, 0, 0 };
    }

    const int64_t offset = address - mapped_memory->first;
    const int64_t lower_bound = -offset;
    const int64_t upper_bound = static_cast<int64_t>(mapped_memory->second.size) - offset;
    constexpr int64_t min_bound = std::numeric_limits<int32_t>::min();
    constexpr int64_t max_bound = std::numeric_limits<int32_t>::max();

    return {
        mapped_memory->second.buffer_address + offset,
        static_cast<int32_t>(std::clamp(lower_bound, min_bound, max_bound)),
        static_cast<int32_t>(std::clamp(upper_bound, min_bound, max_bound))
    };
}

int VKState::get_max_anisotropic_filtering() {
    return static_cast<int>(physical_device_properties.limits.maxSamplerAnisotropy);
}

void VKState::set_anisotropic_filtering(int anisotropic_filtering) {
    texture_cache.anisotropic_filtering = anisotropic_filtering;
}

int VKState::get_max_2d_texture_width() {
    return static_cast<int>(physical_device_properties.limits.maxImageDimension2D);
}

void VKState::set_async_compilation(bool enable) {
    pipeline_cache.set_async_compilation(enable);
}

uint32_t VKState::get_gpu_version() {
    return physical_device_properties.driverVersion;
}

std::string_view VKState::get_gpu_name() {
    return physical_device_properties.deviceName.data();
}

} // namespace renderer::vulkan

static int get_supported_mapping_methods_mask(const vk::PhysicalDevice &gpu, const bool has_properties2, const vk::detail::DispatchLoaderDynamic &dispatch) {
    int mask = (1 << static_cast<int>(MappingMethod::Disabled));

#ifndef __APPLE__
    if (has_properties2) {
        bool support_buffer_device_address = false;
        bool support_standard_layout = false;
        bool support_external_memory = false;
#ifdef __ANDROID__
        bool support_android_buffer_import = false;
        bool support_unix_fd_import = false;
#endif
        for (const vk::ExtensionProperties &ext : gpu.enumerateDeviceExtensionProperties(nullptr, dispatch)) {
            const std::string_view name(ext.extensionName.data());
            if (name == vk::KHRBufferDeviceAddressExtensionName)
                support_buffer_device_address = true;
            else if (name == vk::KHRUniformBufferStandardLayoutExtensionName)
                support_standard_layout = true;
            else if (name == vk::EXTExternalMemoryHostExtensionName)
                support_external_memory = true;
#ifdef __ANDROID__
            else if (name == VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)
                support_android_buffer_import = true;
            else if (name == VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)
                support_unix_fd_import = true;
#endif
        }

        bool support_memory_mapping = true;
        if (support_buffer_device_address) {
            auto features = gpu.getFeatures2KHR<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceBufferDeviceAddressFeatures>(dispatch);
            support_buffer_device_address = static_cast<bool>(features.get<vk::PhysicalDeviceBufferDeviceAddressFeatures>().bufferDeviceAddress);
        }
        support_memory_mapping &= support_buffer_device_address;

        if (support_standard_layout) {
            auto features = gpu.getFeatures2KHR<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceUniformBufferStandardLayoutFeatures>(dispatch);
            support_standard_layout = static_cast<bool>(features.get<vk::PhysicalDeviceUniformBufferStandardLayoutFeatures>().uniformBufferStandardLayout);
        }
        support_memory_mapping &= support_standard_layout;

#ifdef __ANDROID__
        support_android_buffer_import &= SDL_GetAndroidSDKVersion() >= 26;
        support_unix_fd_import &= SDL_GetAndroidSDKVersion() >= 26;
#endif

        if (support_memory_mapping) {
            mask |= (1 << static_cast<int>(MappingMethod::DoubleBuffer));
#ifndef __SWITCH__
            // See VKState::create: no cached+coherent memory type exists on NVK/Tegra.
            mask |= (1 << static_cast<int>(MappingMethod::PageTable));
#endif

            if (support_external_memory) {
                auto props = gpu.getProperties2KHR<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>(dispatch);
                support_external_memory = (props.get<vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>().minImportedHostPointerAlignment <= 4096);
            }

            if (support_external_memory)
                mask |= (1 << static_cast<int>(MappingMethod::ExernalHost));

#ifdef __ANDROID__
            if (support_android_buffer_import || support_unix_fd_import)
                mask |= (1 << static_cast<int>(MappingMethod::NativeBuffer));
#endif
        }
    }
#endif // !__APPLE__

    return mask;
}

renderer::VulkanDeviceInfo renderer::enumerate_vulkan_devices(const std::string &custom_driver_name) {
    VulkanDeviceInfo info;
    info.gpu_names.emplace_back("Automatic");
    info.custom_driver_requested = !custom_driver_name.empty();

    try {
        vk::detail::DispatchLoaderDynamic dispatch;
#ifdef __ANDROID__
        PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr = android_driver::resolve_vk_get_instance_proc_addr(custom_driver_name);
        if (!vk_get_instance_proc_addr)
            return info;

        dispatch.init(vk_get_instance_proc_addr);
#elif defined(__SWITCH__)
        dispatch.init(&::vk_icdGetInstanceProcAddr);
#else
        dispatch.init();
#endif

        vk::ApplicationInfo app_info{
            .apiVersion = VK_API_VERSION_1_0
        };

        std::vector<const char *> instance_extensions;
        bool has_properties2 = false;
        for (const vk::ExtensionProperties &prop : vk::enumerateInstanceExtensionProperties(nullptr, dispatch)) {
            const std::string_view name(prop.extensionName.data());
            if (name == vk::KHRGetPhysicalDeviceProperties2ExtensionName) {
                instance_extensions.push_back(vk::KHRGetPhysicalDeviceProperties2ExtensionName);
                has_properties2 = true;
            }
#ifdef __APPLE__
            else if (name == vk::KHRPortabilityEnumerationExtensionName) {
                instance_extensions.push_back(vk::KHRPortabilityEnumerationExtensionName);
            }
#endif
        }

        vk::InstanceCreateInfo instance_info{
#ifdef __APPLE__
            .flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
#endif
            .pApplicationInfo = &app_info,
        };
        instance_info.setPEnabledExtensionNames(instance_extensions);

        vk::UniqueInstance instance = vk::createInstanceUnique(instance_info, nullptr, dispatch);
        dispatch.init(instance.get(), dispatch.vkGetInstanceProcAddr);
        std::vector<vk::PhysicalDevice> physical_devices = instance->enumeratePhysicalDevices(dispatch);

        for (const vk::PhysicalDevice &gpu : physical_devices) {
            const vk::PhysicalDeviceProperties properties = gpu.getProperties(dispatch);
            info.gpu_names.emplace_back(properties.deviceName.data());
            info.mapping_method_masks.push_back(get_supported_mapping_methods_mask(gpu, has_properties2, dispatch));
        }

#ifdef __ANDROID__
        if (info.custom_driver_requested && !physical_devices.empty())
            info.custom_driver_loaded = android_driver::is_custom_driver_loaded(
                custom_driver_name,
                physical_devices.front().getProperties(dispatch).vendorID,
                physical_devices.front().getProperties(dispatch).driverVersion,
                physical_devices.front().getProperties(dispatch).deviceName.data());
#endif
    } catch (const std::exception &e) {
        LOG_WARN("Vulkan device enumeration failed: {}", e.what());
    }

    return info;
}

namespace renderer::vulkan {

void VKState::precompile_shader(const ShadersHash &hash) {
    Sha256Hash empty_hash{};
    if (hash.vert != empty_hash) {
        pipeline_cache.precompile_shader(hash.vert);
    }
    if (hash.frag != empty_hash) {
        pipeline_cache.precompile_shader(hash.frag);
    }

    programs_count_pre_compiled++;
    LOG_INFO("Program Compiled {}/{}", programs_count_pre_compiled, shaders_cache_hashs.size());
}

void VKState::preclose_action() {
    // Stop the GPU request wait thread before destruction begins.
    // VKState (owns the queue) is destroyed before VKContext (owns the thread).
    request_queue.abort();

    // make sure we are in a game
    if (shaders_path.empty())
        return;

    pipeline_cache.save_pipeline_cache();
}

#ifdef __ANDROID__
bool VKState::support_custom_drivers() {
    // vendor ID 0x5143 is Qualcomm, being stock or turnip
    return physical_device_properties.vendorID == 0x5143;
}

void VKState::set_turbo_mode(bool set) {
#ifdef USE_ADRENO_TOOLS
    if (!support_custom_drivers())
        return;

    adrenotools_set_turbo(set);
#endif
}
#endif

BufferTrapping::BufferTrapping(VKState &state)
    : state(state) {}

TrappedBuffer *BufferTrapping::access_buffer(Address addr, uint32_t size, MemState &mem, bool always_trap, bool cover_everything) {
#ifdef __SWITCH__
    // Desktop Double Buffer marks cached GPU copies dirty with mprotect faults.
    // Horizon cannot resume that kind of guest write fault. Copying on every
    // draw is not equivalent: it overwrites data produced by Vita shader buffer
    // stores before a later draw can consume it. Compare small guest blocks with
    // the bytes last uploaded instead. Unchanged CPU blocks remain GPU-owned;
    // changed blocks are copied and explicitly flushed for NVK's cached mapping.
    // Base-address lookup on purpose: resolving buffers suballocated inside a
    // mapping walks tracking state this path was never built for.
    auto mem_it = state.mapped_memories.lower_bound(addr);
    if (mem_it == state.mapped_memories.end()
        || static_cast<uint64_t>(mem_it->first) + mem_it->second.size < static_cast<uint64_t>(addr) + size) {
        LOG_ERROR_ONCE("Buffer at address {} is not completely mapped", log_hex(addr));
        return &temp_buffer;
    }

    MappedMemory &mapping = mem_it->second;
    // Imported mappings alias guest memory: nothing to copy, only the CPU
    // cache to clean, or to flush where shader stores may write the range.
    if (std::holds_alternative<ExternalBuffer>(mapping.buffer_impl)) {
        uint8_t *const host = Ptr<uint8_t>(addr).get(mem);
        if (host) {
            if (always_trap)
                armDCacheFlush(host, size);
            else
                armDCacheClean(host, size);
        }
        temp_buffer.size = size;
        temp_buffer.mapped_location = host;
        // Force the index path to recompute its max element every draw.
        temp_buffer.extra = ~0U;
        return &temp_buffer;
    }
    vkutil::Buffer *const gpu_buffer_ptr = std::get_if<vkutil::Buffer>(&mapping.buffer_impl);
    if (!gpu_buffer_ptr) {
        LOG_ERROR_ONCE("Buffer at address {} is in a mapping with no host pointer", log_hex(addr));
        return &temp_buffer;
    }
    vkutil::Buffer &gpu_buffer = *gpu_buffer_ptr;
    if (!gpu_buffer.mapped_data) {
        LOG_ERROR_ONCE("Buffer at address {} is in a mapping with no host pointer", log_hex(addr));
        return &temp_buffer;
    }

    // Not every mapping path sizes the tracking arrays; size them on first use.
    const size_t hash_block_count = (mapping.size + SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE - 1)
        / SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE;
    if (mapping.cpu_block_scene.size() != hash_block_count) {
        mapping.cpu_block_hashes.assign(hash_block_count, 0);
        mapping.cpu_block_hash_valid.assign((hash_block_count + 63) / 64, 0);
        mapping.cpu_block_scene.assign(hash_block_count, 0);
    }

    const uint32_t request_offset = addr - mem_it->first;
    const uint32_t first_block = request_offset / SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE;
    const uint32_t end_offset = request_offset + size;
    const uint32_t end_block = size == 0
        ? first_block
        : (end_offset + SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE - 1) / SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE;

    bool any_cpu_block_changed = false;
    uint32_t dirty_run_begin = 0;
    bool dirty_run_active = false;

    const auto upload_dirty_run = [&](const uint32_t begin_block, const uint32_t end_block_exclusive) {
        const uint32_t begin = begin_block * SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE;
        const uint32_t end = std::min<uint32_t>(end_block_exclusive * SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE, mapping.size);
        const uint32_t bytes = end - begin;
        auto *const destination = static_cast<uint8_t *>(gpu_buffer.mapped_data) + mapping.buffer_offset + begin;
        const void *const source = Ptr<const void>(mem_it->first + begin).get(mem);
        if (!source)
            return;
        memcpy(destination, source, bytes);
        gpu_buffer.flush(mapping.buffer_offset + begin, bytes);
    };

    // Shader-store ranges must be re-checked every draw, and uniform ranges are
    // exempted conservatively (a guest uniform ring could legally reuse bytes
    // between draws of one scene). Vertex and index data is protected by GXM's
    // own contract: rewriting it while the scene is in flight would corrupt the
    // real console too.
    const bool memo_allowed = !always_trap && !cover_everything;
    const uint32_t scene_epoch = static_cast<uint32_t>(state.texture_cache.memo_scene);
    for (uint32_t block = first_block; block < end_block; block++) {
        const uint32_t block_offset = block * SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE;
        // Skip guest pages with no translation (never committed).
        const void *const guest_block = Ptr<const void>(mem_it->first + block_offset).get(mem);
        if (!guest_block) {
            if (dirty_run_active) {
                upload_dirty_run(dirty_run_begin, block);
                dirty_run_active = false;
            }
            continue;
        }
        const uint64_t valid_bit = uint64_t{ 1 } << (block & 63);
        const bool valid = mapping.cpu_block_hash_valid[block / 64] & valid_bit;
        bool changed = false;
        const bool verified_this_scene = memo_allowed && valid
            && mapping.cpu_block_scene[block] == scene_epoch;
        if (!verified_this_scene) {
            const uint32_t block_size = std::min<uint32_t>(SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE, mapping.size - block_offset);
            const uint64_t hash = XXH3_64bits(guest_block, block_size);
            changed = !valid || mapping.cpu_block_hashes[block] != hash;

            if (changed) {
                mapping.cpu_block_hashes[block] = hash;
                mapping.cpu_block_hash_valid[block / 64] |= valid_bit;
                any_cpu_block_changed = true;
                if (!dirty_run_active) {
                    dirty_run_begin = block;
                    dirty_run_active = true;
                }
            }
            mapping.cpu_block_scene[block] = scene_epoch;
        }

        // Keep each memcpy inside one guest page. Switch guest allocations are
        // contiguous, but this also remains safe for a mapping at an allocation
        // boundary or any future non-contiguous backing scheme.
        const bool page_ends = ((block + 1) * SWITCH_DOUBLE_BUFFER_HASH_BLOCK_SIZE) % KiB(4) == 0;
        if (dirty_run_active && (!changed || page_ends)) {
            const uint32_t dirty_run_end = changed ? block + 1 : block;
            upload_dirty_run(dirty_run_begin, dirty_run_end);
            dirty_run_active = false;
        }
    }
    if (dirty_run_active)
        upload_dirty_run(dirty_run_begin, end_block);

    auto [tracked_it, switch_is_new] = trapped_buffers.try_emplace(addr);
    TrappedBuffer &tracked = tracked_it->second;
    if (switch_is_new || tracked.size != size || any_cpu_block_changed)
        tracked.extra = ~0U;
    tracked.size = size;
    tracked.dirty = false;
    tracked.mapped_location = static_cast<uint8_t *>(gpu_buffer.mapped_data)
        + mapping.buffer_offset + request_offset;
    return &tracked;
#endif

    const bool is_buffer_small = (size < 3 * KiB(4));

    if (is_buffer_small && always_trap) {
        // overwise we may end up with trapping nothing
        cover_everything = true;
    } else if (is_buffer_small) {
        // not big enough to apply buffer trapping
        auto mem_it = state.mapped_memories.lower_bound(addr);
        if (mem_it == state.mapped_memories.end() || mem_it->first + mem_it->second.size < addr + size) {
            LOG_ERROR("Buffer at address {} is not completely mapped", log_hex(addr));
            return &temp_buffer;
        }

        temp_buffer.size = size;
        temp_buffer.mapped_location = reinterpret_cast<uint8_t *>(std::get<vkutil::Buffer>(mem_it->second.buffer_impl).mapped_data);
        temp_buffer.mapped_location += addr - mem_it->first;
        temp_buffer.extra = ~0;

        memcpy(temp_buffer.mapped_location, Ptr<void>(addr).get(mem), size);
        return &temp_buffer;
    }

    auto it = trapped_buffers.find(addr);
    bool is_new = false;
    if (it != trapped_buffers.end()) {
        // must check if everything match
        TrappedBuffer &buffer = it->second;
        if (!buffer.dirty && buffer.size >= size)
            // nothing to change
            return &it->second;
    } else {
        it = trapped_buffers.emplace(std::piecewise_construct, std::forward_as_tuple(addr), std::forward_as_tuple()).first;
        is_new = true;
    }

    {
        // remove the following overlapping dirty buffers
        auto next_it = it;
        next_it++;
        while (next_it != trapped_buffers.end() && next_it->first < addr + size) {
            if (next_it->second.dirty)
                next_it = trapped_buffers.erase(next_it);
            else
                next_it++;
        }
    }
    it->second.size = size;
    it->second.dirty = false;
    it->second.extra = ~0;

    if (is_new) {
        // we must find the matching mapped buffer
        auto mem_it = state.mapped_memories.lower_bound(addr);
        if (mem_it == state.mapped_memories.end() || mem_it->first + mem_it->second.size < addr + size) {
            LOG_ERROR("Buffer at address {} is not completely mapped", log_hex(addr));
            return &it->second;
        }

        it->second.mapped_location = reinterpret_cast<uint8_t *>(std::get<vkutil::Buffer>(mem_it->second.buffer_impl).mapped_data);
        it->second.mapped_location += addr - mem_it->first;
    }

    Address aligned_addr;
    uint32_t aligned_size;
    if (cover_everything) {
        aligned_addr = align_down(addr, KiB(4));
        aligned_size = align(addr + size, KiB(4)) - aligned_addr;
    } else {
        aligned_addr = align(addr, KiB(4));
        aligned_size = align_down(addr + size - aligned_addr, KiB(4));
    }
    add_protect(mem, aligned_addr, aligned_size, MemPerm::ReadOnly, [it](Address addr, bool write) {
        it->second.dirty = true;
        return true;
    });

    // copy back the data as it was non-existent or dirty
    memcpy(it->second.mapped_location, Ptr<void>(addr).get(mem), size);

    return &it->second;
}

void BufferTrapping::remove_range(Address start, Address end) {
    auto it = trapped_buffers.lower_bound(start);
    while (it != trapped_buffers.end() && it->first < end)
        it = trapped_buffers.erase(it);
}

} // namespace renderer::vulkan
