// Vita3K emulator project
// Copyright (C) 2025 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <util/switch_thread.h>

#ifdef __SWITCH__

#include <switch.h>

#include <util/log.h>


namespace {

constexpr int32_t IDEAL_CORE_DONT_CARE = -1;
constexpr uint32_t APP_CORE_MASK = 0b0111u;
constexpr uint32_t ALL_CORE_MASK = 0b1111u;
constexpr uint32_t HELPER_CORE_MASK = 0b1000u;

// Vita user priorities run 64 (highest) to 191 (lowest); Horizon runs 0 to 63 the
// same way round. Guest threads map into a band that preserves the game's own
// ordering while staying below the emulator's latency-critical helpers.
constexpr int32_t GUEST_PRIORITY_HIGHEST = 45;
constexpr int32_t GUEST_PRIORITY_LOWEST = 56;
constexpr int32_t VITA_PRIORITY_HIGHEST = 64;
constexpr int32_t VITA_PRIORITY_LOWEST = 191;

// The app cores' preemption priority, the one level Horizon round-robins. Host
// workers with no deadline sit here: below every guest thread, sharing fairly.
constexpr int32_t BACKGROUND_PRIORITY = 59;

} // namespace

void switch_pin_to_app_cores(const char *what) {
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, IDEAL_CORE_DONT_CARE, APP_CORE_MASK);
    if (R_SUCCEEDED(rc))
        return;

    rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, APP_CORE_MASK);
    if (R_FAILED(rc))
        LOG_WARN("Could not restrict {} to cores 0/1/2 (rc=0x{:X}); it may run on the OS-reserved core 3", what, rc);
}


bool switch_pin_to_helper_core(const char *what, const int32_t helper_priority) {
    // Only a build whose NPDM asks for the fourth core may run there; the call
    // simply fails otherwise. Horizon never preempts between equal priorities,
    // so every core-3 tenant carries a distinct one, ordered by how long it runs
    // per wakeup: audio 43, vblank 44, Vulkan wait 45, GXM display 46, render
    // 47. The priority is applied only once the pin succeeded - on the app cores
    // these would outrank the guest itself.
    if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, HELPER_CORE_MASK))) {
        const int32_t priority = helper_priority;
        if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, priority)))
            LOG_INFO("[switch] {} runs on the helper core at priority {}", what, priority);
        else
            LOG_INFO("[switch] {} runs on the helper core", what);
        return true;
    }

    // Core 3 unavailable (applet launch): the tick still has to outrank the
    // guest band, or a busy game starves audio and vblank outright under
    // strict priority on the app cores.
    switch_pin_to_app_cores(what);
    if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, GUEST_PRIORITY_HIGHEST - 1)))
        LOG_WARN("Could not raise {} above the guest band", what);
    return false;
}

void switch_apply_guest_thread_affinity(const int32_t affinity_mask) {
    // SCE_KERNEL_CPU_MASK_USER_ALL, kept local so this file needs no guest headers.
    constexpr int32_t VITA_USER_CORE_ALL = 0x70000;
    constexpr int32_t VITA_USER_CORE_SHIFT = 16;

    const int32_t requested = affinity_mask & VITA_USER_CORE_ALL;
    if (requested == 0 || requested == VITA_USER_CORE_ALL)
        return;

    const uint32_t host_mask = static_cast<uint32_t>(requested) >> VITA_USER_CORE_SHIFT;
    const int32_t ideal_core = __builtin_ctz(host_mask);
    if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal_core, host_mask)))
        LOG_INFO("[switch] guest thread restricted to core mask 0b{:03b} at the game's request", host_mask);
    else
        LOG_WARN("Could not restrict a guest thread to core mask 0b{:03b}", host_mask);
}

int switch_mapped_guest_priority(int vita_priority) {
    if (vita_priority < VITA_PRIORITY_HIGHEST || vita_priority > VITA_PRIORITY_LOWEST)
        return -1;

    constexpr int32_t vita_span = VITA_PRIORITY_LOWEST - VITA_PRIORITY_HIGHEST;
    constexpr int32_t host_span = GUEST_PRIORITY_LOWEST - GUEST_PRIORITY_HIGHEST;
    return GUEST_PRIORITY_HIGHEST
        + ((vita_priority - VITA_PRIORITY_HIGHEST) * host_span + vita_span / 2) / vita_span;
}

void switch_pin_to_hardware_priority(const char *what, const int32_t helper_priority) {
    // These threads stand in for hardware the guest waits on, which never
    // competed for a core on the Vita. Prefer the helper core so their cycles
    // stop costing the guest one of its three.
    if (switch_pin_to_helper_core(what, helper_priority))
        return;

    // Core 3 unavailable (applet launch): fall back to the app cores, raised
    // above the guest band so busy guest threads cannot starve them.
    const int32_t priority = GUEST_PRIORITY_HIGHEST - 1;
    if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, priority)))
        LOG_INFO("[switch] {} raised to priority {}", what, priority);
    else
        LOG_WARN("Could not raise {} above the guest band", what);
}

void switch_allow_helper_core(const char *what) {
    // Permits the helper core without moving the thread there: the scheduler
    // uses core 3's idle time, but the tenants above always preempt it.
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, IDEAL_CORE_DONT_CARE, ALL_CORE_MASK)))
        switch_pin_to_app_cores(what);

    // libnx gives a new thread the priority of its creator, so without this a
    // burst of workers inherits whatever the main thread happens to sit at.
    s32 inherited = 0;
    svcGetThreadPriority(&inherited, CUR_THREAD_HANDLE);
    if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, BACKGROUND_PRIORITY)))
        LOG_INFO("[switch] {} may use any core, at background priority {} (inherited {})", what,
            BACKGROUND_PRIORITY, inherited);
    else
        LOG_WARN("Could not lower {} to background priority", what);
}



void switch_apply_guest_thread_priority(int vita_priority) {
    const int32_t priority = switch_mapped_guest_priority(vita_priority);
    if (priority < 0)
        return; // not a user priority; leave the thread where hbloader put it

    const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority));
    if (R_FAILED(rc))
        LOG_WARN("Could not set guest thread priority {} (rc=0x{:X})", priority, rc);
}

SwitchCpuBoost::SwitchCpuBoost(const char *what) {
    const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    m_held = R_SUCCEEDED(rc);
    if (m_held)
        LOG_INFO("[switch] CPU boost held for {}", what);
    else
        LOG_WARN("Could not boost the CPU for {} (rc=0x{:X})", what, rc);
}

SwitchCpuBoost::~SwitchCpuBoost() {
    if (m_held)
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}

#endif
