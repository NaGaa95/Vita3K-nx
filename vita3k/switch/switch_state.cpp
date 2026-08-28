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

#include <cpu/functions.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <util/switch_thread.h>
#include <rtc/rtc.h>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

// Not <switch.h>: kernel/state.h declares guest Mutex/Semaphore types that
// collide with libnx's.
#include <switch/types.h>
#include <switch/arm/counter.h>
extern "C" {
#include <switch/kernel/svc.h>
}

uint64_t switch_guest_tick(EmuEnvState &emuenv) {
    return rtc_get_ticks(emuenv.kernel.base_tick.tick);
}

void switch_advance_guest_start_tick(EmuEnvState &emuenv, uint64_t delta) {
    emuenv.kernel.start_tick += delta;
}

SwitchSessionState &switch_session_state() {
    static SwitchSessionState state;
    return state;
}

EmuEnvState *get_emuenv() {
    return switch_session_state().emuenv.get();
}

app::AppSessionController *get_app_session_controller() {
    return switch_session_state().app_session_controller.get();
}

Root &get_root_paths() {
    return switch_session_state().root_paths;
}

namespace {
constexpr uint64_t PREEMPT_QUANTUM_NS = 2'000'000; // 2 ms, close to a Vita slice
// A yield only reaches threads of equal or higher priority; a spin that outlasts
// this many quanta gives its core up instead, which is the only way a lower
// priority thread it may be waiting on ever runs.
constexpr unsigned PREEMPT_RELEASE_AFTER = 24; // ~48 ms of unbroken translated code
std::thread preemption_thread;
std::atomic<bool> preemption_running{ false };

void preemption_watchdog(EmuEnvState &emuenv) {
    // Above every core-3 tenant: it must run precisely when the app cores are
    // saturated, which is the only time it does anything.
    switch_pin_to_helper_core("preemption watchdog", 42);

    struct Watched {
        uint64_t ticks = 0;
        unsigned quanta = 0;
    };
    std::unordered_map<SceUID, Watched> seen;
    std::vector<std::pair<SceUID, ThreadStatePtr>> threads;
    uint64_t next_refresh = 0;

    while (preemption_running.load(std::memory_order_relaxed)) {
        svcSleepThread(PREEMPT_QUANTUM_NS);
        if (emuenv.kernel.is_threads_paused())
            continue;

        // The thread list changes far more slowly than the quantum, and the kernel
        // lock guards every create, lookup and exit.
        const uint64_t now = armGetSystemTick();
        if (now >= next_refresh) {
            next_refresh = now + armGetSystemTickFreq() / 2;
            threads.clear();
            const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
            for (const auto &entry : emuenv.kernel.threads)
                threads.push_back(entry);
        }

        for (const auto &[id, thread] : threads) {
            if (!thread || !thread->cpu)
                continue;
            Watched &watched = seen[id];
            // A thread parked in host code inside an import looks identical to a
            // spinning one, so only translated code qualifies. A thread the
            // kernel has asked to suspend looks identical too - status run, no
            // imports - until it reaches the boundary where it parks.
            if (thread->status.load(std::memory_order_relaxed) != ThreadStatus::run
                || thread->current_import_nid.load(std::memory_order_relaxed) != 0
                || thread->is_suspend_requested()) {
                watched.quanta = 0;
                continue;
            }
            const uint64_t ticks = thread->cpu->import_serial.load(std::memory_order_relaxed);
            const bool stayed_in_jit = watched.ticks == ticks;
            watched.ticks = ticks;
            if (!stayed_in_jit) {
                watched.quanta = 0;
                continue;
            }
            watched.quanta++;
            preempt(*thread->cpu, watched.quanta >= PREEMPT_RELEASE_AFTER);
        }
    }
}
} // namespace

void switch_start_preemption_watchdog(EmuEnvState &emuenv) {
    if (preemption_running.exchange(true))
        return;
    preemption_thread = std::thread(preemption_watchdog, std::ref(emuenv));
}

void switch_stop_preemption_watchdog() {
    if (!preemption_running.exchange(false))
        return;
    if (preemption_thread.joinable())
        preemption_thread.join();
}
