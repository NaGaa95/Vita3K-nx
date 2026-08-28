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

#include "switch_state.h"

#include <app/functions.h>
#include <compat/functions.h>
#include <compat/state.h>
#include <config/functions.h>
#include <config/version.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <renderer/functions.h>
#include <util/log.h>

#include <openssl/provider.h>

#include <switch.h>

#ifdef BIT
#undef BIT
#endif
#ifdef BITL
#undef BITL
#endif

#include <sys/iosupport.h>

#include <filesystem>
#include <mutex>
#include <string>

extern "C" {
extern char *fake_heap_start;
extern char *fake_heap_end;
}

static u64 g_switch_loader_heap_base = 0;
static u64 g_switch_loader_heap_size = 0;
static u64 g_switch_trimmed_heap_size = 0;

constexpr u64 SWITCH_HEAP_ALIGNMENT = 0x200000; // svcSetHeapSize: 2 MiB granularity
// Every allocation this process makes comes out of newlib's heap: the guest
// memory pool, dynarmic's JIT code memory and NVK's buffer backing store all
// come from it, so memory withheld here is simply denied to the guest and the
// GPU. This reserve only has to cover the kernel objects libnx creates for us.
constexpr u64 SWITCH_EXTERNAL_RESERVE = 0x2000000; // 32 MiB
constexpr u64 SWITCH_EXPANDED_MEMORY_TOTAL = 0x140000000; // 5 GiB: an expanded-memory unit
constexpr u64 SWITCH_MIN_USABLE_HOST_HEAP = 0x50000000; // 1.25 GiB beyond libnx's prefix

static constexpr u64 align_heap_down(u64 size) {
    return size & ~(SWITCH_HEAP_ALIGNMENT - 1);
}

static constexpr u64 align_heap_up(u64 size) {
    return (size + SWITCH_HEAP_ALIGNMENT - 1) & ~(SWITCH_HEAP_ALIGNMENT - 1);
}

// Size newlib's heap from the memory the kernel says this process has rather
// than whatever hbloader committed: it can leave memory uncommitted on one unit
// and over-commit on another, so resizing in either direction matters.
static void switch_configure_heap() {
    u64 total = 0, heap_base = 0, heap_region_size = 0, used_before = 0;
    const Result total_rc = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    const Result heap_base_rc = svcGetInfo(&heap_base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    const Result heap_size_rc = svcGetInfo(&heap_region_size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used_before, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    if (R_FAILED(total_rc) || R_FAILED(heap_base_rc) || R_FAILED(heap_size_rc)) {
        LOG_WARN("Heap configuration skipped: svcGetInfo failed (total=0x{:X}, base=0x{:X}, region=0x{:X})",
            total_rc, heap_base_rc, heap_size_rc);
        return;
    }

    const u64 fhs = reinterpret_cast<u64>(fake_heap_start);
    const u64 fhe = reinterpret_cast<u64>(fake_heap_end);
    // svcSetHeapSize manages the whole heap region from its base. newlib's heap
    // typically starts a little above the region base (libnx internals sit below
    // it), so require fake_heap_start to be *within* the region, not equal to base.
    if (fhs < heap_base || fhs >= heap_base + heap_region_size || fhe <= fhs || fhe > heap_base + heap_region_size) {
        LOG_WARN("Heap configuration skipped: newlib heap [0x{:X},0x{:X}) outside heap region [0x{:X},+0x{:X})",
            fhs, fhe, heap_base, heap_region_size);
        return;
    }

    const u64 cur_region = fhe - heap_base; // current committed heap size, from base
    const u64 external_reserve = SWITCH_EXTERNAL_RESERVE;
    u64 new_region = total > external_reserve ? align_heap_down(total - external_reserve) : 0;

    // Preserve libnx's prefix and enough usable CPU heap for the guest pool, JIT
    // source pages, renderer structures, modules, and normal allocations.
    const u64 min_region = align_heap_up((fhs - heap_base) + SWITCH_MIN_USABLE_HOST_HEAP);
    if (new_region < min_region)
        new_region = min_region;

    LOG_INFO("Switch RAM plan: total={} MiB, loader heap={} MiB, host heap target={} MiB, NVK/system reserve={} MiB ({})",
        total >> 20, cur_region >> 20, new_region >> 20, external_reserve >> 20,
        total >= SWITCH_EXPANDED_MEMORY_TOTAL ? "expanded-memory" : "stock-memory");

    if (new_region == cur_region) {
        LOG_INFO("Heap already spans the target region (0x{:X} bytes); no repartition needed", cur_region);
        return;
    }

    const char *const direction = new_region > cur_region ? "grown" : "trimmed";
    void *addr = nullptr;
    const Result rc = svcSetHeapSize(&addr, new_region);
    if (R_FAILED(rc)) {
        LOG_WARN("Heap not {}: svcSetHeapSize(0x{:X}) failed rc=0x{:X}", direction, new_region, rc);
        return;
    }
    g_switch_loader_heap_base = heap_base;
    g_switch_loader_heap_size = cur_region;
    g_switch_trimmed_heap_size = new_region;
    // Region shrunk; let newlib grow up to the new top (its active window near
    // fake_heap_start stays mapped since new_region >> current usage).
    fake_heap_end = reinterpret_cast<char *>(heap_base + new_region);

    u64 total_after = 0, used_after = 0;
    svcGetInfo(&total_after, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used_after, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    LOG_INFO("Heap {} {} MiB->{} MiB; used {} MiB->{} MiB, {} MiB of the budget left uncommitted",
        direction, cur_region >> 20, new_region >> 20, used_before >> 20, used_after >> 20,
        total_after > used_after ? (total_after - used_after) >> 20 : 0);
}

// Mesa and NVK's shader compiler report failures on stderr, which Horizon
// points at a null device - a NAK internal compiler error would otherwise reach
// the log as a bare vk::Result. Route stderr into the emulator log.
namespace {

std::mutex g_stderr_mutex;
std::string g_stderr_line;

ssize_t stderr_to_log(struct _reent *, void *, const char *ptr, size_t len) {
    // A logger that ever wrote to stderr itself would re-enter this endlessly.
    static thread_local bool reentered = false;
    if (reentered)
        return static_cast<ssize_t>(len);
    reentered = true;

    {
        const std::lock_guard<std::mutex> lock(g_stderr_mutex);
        for (size_t i = 0; i < len; i++) {
            if (ptr[i] == '\n') {
                if (!g_stderr_line.empty())
                    LOG_ERROR("[stderr] {}", g_stderr_line);
                g_stderr_line.clear();
            } else if (ptr[i] != '\r') {
                g_stderr_line.push_back(ptr[i]);
            }
        }
    }

    reentered = false;
    return static_cast<ssize_t>(len);
}

// Writes arrive as raw bytes with no file handle, exactly as libnx's own
// console device expects, so no open/close hook is needed.
const devoptab_t g_stderr_devoptab = {
    .name = "stderr",
    .structSize = 0,
    .write_r = stderr_to_log,
};

} // namespace

void switch_capture_stderr() {
    devoptab_list[STD_ERR] = &g_stderr_devoptab;
}

bool switch_restore_loader_heap() {
    if (g_switch_loader_heap_size == 0 || g_switch_trimmed_heap_size == 0)
        return true;

    u64 total_before = 0, used_before = 0;
    svcGetInfo(&total_before, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used_before, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    void *addr = nullptr;
    const Result rc = svcSetHeapSize(&addr, g_switch_loader_heap_size);
    LOG_WARN("Heap resize returned rc=0x{:X}, base 0x{:X}", rc, reinterpret_cast<u64>(addr));
    if (R_FAILED(rc)) {
        LOG_ERROR("Loader heap restore 0x{:X}->0x{:X} failed rc=0x{:X} (used 0x{:X}/0x{:X})",
            g_switch_trimmed_heap_size, g_switch_loader_heap_size, rc, used_before, total_before);
        return false;
    }

    const u64 restored_base = reinterpret_cast<u64>(addr);
    if (restored_base != g_switch_loader_heap_base) {
        LOG_ERROR("Loader heap restore returned unexpected base 0x{:X} (expected 0x{:X})",
            restored_base, g_switch_loader_heap_base);
        return false;
    }

    // Keep newlib's view consistent until control returns to hbloader. hbloader
    // originally supplied this exact end address in EntryType_OverrideHeap.
    fake_heap_end = reinterpret_cast<char *>(g_switch_loader_heap_base + g_switch_loader_heap_size);

    u64 total_after = 0, used_after = 0;
    svcGetInfo(&total_after, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used_after, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    LOG_INFO("Restored hbloader heap 0x{:X}->0x{:X} at 0x{:X}; used 0x{:X}->0x{:X}, free 0x{:X}",
        g_switch_trimmed_heap_size, g_switch_loader_heap_size, g_switch_loader_heap_base,
        used_before, used_after, total_after > used_after ? total_after - used_after : 0);

    g_switch_trimmed_heap_size = 0;
    return true;
}


// Phase-A initialisation. Mirrors the Android port's initialize_session(), with
// a writable SD-card root and read-only static assets from the libnx romfs.
bool init_switch_session(const fs::path &storage_path, Root &root_paths,
    std::unique_ptr<EmuEnvState> &emuenv, bool install_mode) {
    try {
        const fs::path vita_path = storage_path / "vita" / "";

        // Static (read-only) assets live in the romfs bundled into the .nro.
        root_paths.set_static_assets_path(fs::path("romfs:/"));
        root_paths.set_vita_fs_path(vita_path);
        root_paths.set_log_path(storage_path);
        root_paths.set_config_path(storage_path);
        root_paths.set_shared_path(storage_path);
        root_paths.set_cache_path(storage_path / "cache" / "");
        root_paths.set_patch_path(storage_path / "patch" / "");

        if (!fs::exists(root_paths.get_vita_fs_path()))
            fs::create_directories(root_paths.get_vita_fs_path());

        fs::create_directories(root_paths.get_config_path());
        fs::create_directories(root_paths.get_cache_path());
        fs::create_directories(root_paths.get_log_path() / "shaderlog");
        fs::create_directories(root_paths.get_log_path() / "texturelog");
        fs::create_directories(root_paths.get_patch_path());
        fs::create_directories(root_paths.get_shared_path() / "textures");

        // Pre-create the ux0 tree so load_users / apps scanning never hits a
        // missing directory (io::init also creates these, but the sdmc: fsdev
        // has been observed to leave a just-created dir non-openable).
        const fs::path ux0 = vita_path / "ux0";
        fs::create_directories(ux0 / "user");
        fs::create_directories(ux0 / "app");
        fs::create_directories(ux0 / "appmeta");
        fs::create_directories(ux0 / "license");
        fs::create_directories(ux0 / "data");

        // use_stdout=false: spdlog's stdout sink writes through the libnx software
        // console, which crashes once consoleExit runs or the renderer owns the
        // window. main_switch.cpp later redirects the session to a per-driver log.
        if (logging::init(root_paths, false) != Success)
            return false;
        // Logging is synchronous on Switch (see logging.cpp) so a hard guest
        // crash cannot lose the last lines naming what was executing.

        // Bound the newlib heap so the process leaves headroom; the guest memory
        // pool is later carved from this heap. Install mode never runs guest code,
        // so it reserves nothing - leaving the address space clean for the chainload.
        if (!install_mode) {
            switch_configure_heap();
        }

        // OpenSSL 3.x on Horizon does not auto-activate its built-in providers,
        // so EVP_*_fetch("AES-...") returns null and all SCE/PKG decryption fails.
        // Load them explicitly and keep them for the process lifetime. (default =
        // AES/SHA/etc.; legacy = older ciphers some Vita content still uses.)
        static OSSL_PROVIDER *s_default_provider = OSSL_PROVIDER_load(nullptr, "default");
        static OSSL_PROVIDER *s_legacy_provider = OSSL_PROVIDER_load(nullptr, "legacy");
        if (!s_default_provider)
            LOG_ERROR("Failed to load OpenSSL default provider — decryption will not work");
        else
            LOG_INFO("OpenSSL providers loaded (default={}, legacy={})",
                s_default_provider != nullptr, s_legacy_provider != nullptr);

        LOG_INFO("{}", window_title);
        LOG_INFO("Vita3K Switch port starting up");
        LOG_INFO("Switch frontend restart revision 4 active");

        emuenv = std::make_unique<EmuEnvState>();

        Config cfg{};
        char arg0[] = "vita3k";
        char *argv[] = { arg0, nullptr };
        if (config::init_config(cfg, 1, argv, root_paths, false) != Success) {
            LOG_ERROR("Failed to initialise config.");
            emuenv.reset();
            return false;
        }

        if (!install_mode) {
            // Reserve the page-table backing before Vulkan and worker threads
            // fragment Horizon's address regions.
            if (!switch_reserve_guest_region())
                LOG_ERROR("Early guest page-table reservation failed; will retry at game launch");
        }

        fs::create_directories(cfg.get_vita_fs_path());

        if (!app::init(*emuenv, cfg, root_paths, install_mode)) {
            LOG_ERROR("Failed to initialise emulated environment.");
            emuenv.reset();
            return false;
        }

        // Install mode stops here: cfg + filesystem paths are set, which is all the
        // installer touches. Everything below stands up heavyweight runtime state
        // (Vulkan, HLE tables, apps/users) that must not exist during the chainload.
        if (install_mode)
            return true;

        // Avoid loading NVK at launcher startup when the selected global
        // renderer is OpenGL/Zink. A per-game Vulkan override probes lazily in
        // main_switch.cpp after its effective configuration has been loaded.
        if (emuenv->backend_renderer == renderer::Backend::Vulkan)
            emuenv->vulkan_device_info = std::make_unique<renderer::VulkanDeviceInfo>(renderer::enumerate_vulkan_devices());
        else
            emuenv->vulkan_device_info.reset();

        if (emuenv->cfg.controller_binds.empty() || emuenv->cfg.controller_binds.size() != 15
            || emuenv->cfg.controller_axis_binds.empty() || emuenv->cfg.controller_axis_binds.size() != 6) {
            app::reset_controller_binding(*emuenv);
        }

        init_libraries(*emuenv);

        if (!app::init_apps_list(*emuenv))
            LOG_ERROR("Failed to initialise apps list.");

        app::load_users(*emuenv);
        if (!app::ensure_current_user(*emuenv)) {
            LOG_ERROR("Failed to initialize active user.");
            return false;
        }
        compat::load_from_disk(emuenv->compat, std::filesystem::path(emuenv->cache_path.string()));
        return true;
    } catch (const std::exception &error) {
        LOG_ERROR("Failed to initialize Switch storage path '{}': {}", storage_path, error.what());
        emuenv.reset();
        return false;
    }
}
