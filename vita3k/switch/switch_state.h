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

#pragma once

// Nintendo Switch (Horizon / libnx) session state. This mirrors the Android
// port's AndroidSessionState but without any JNI: the frontend is native C++.

#include <app/session_controller.h>
#include <emuenv/state.h>
#include <util/fs.h>

#include <memory>

struct SwitchSessionState {
    std::unique_ptr<EmuEnvState> emuenv;
    std::unique_ptr<app::AppSessionController> app_session_controller;
    Root root_paths;
};

SwitchSessionState &switch_session_state();
EmuEnvState *get_emuenv();
app::AppSessionController *get_app_session_controller();
Root &get_root_paths();

// Phase-A initialisation (paths, EmuEnvState, HLE library tables, apps/users).
// storage_path is the writable root on the SD card, e.g. "sdmc:/vita3k/".
// install_mode: lightweight setup for "--install" only - skips the guest memory
// reservation, Vulkan enumeration, HLE library tables and apps/users so nothing
// heavyweight survives the envSetNextLoad chainload back to the launcher.
bool init_switch_session(const fs::path &storage_path, Root &root_paths,
    std::unique_ptr<EmuEnvState> &emuenv, bool install_mode = false);

// Restore the hbloader-owned process heap after the runtime-only trim performed
// by init_switch_session(). The loader retains the original size and expects it
// again when it maps the next NRO into this process.
bool switch_restore_loader_heap();

// Current value of the clock behind sceKernelGetProcessTime, in Vita ticks
// (microseconds). Defined away from the frontend because kernel/state.h declares
// Mutex and Semaphore, which collide with libnx's.
uint64_t switch_guest_tick(EmuEnvState &emuenv);

// Pushes kernel.start_tick forward so `delta` ticks of wall time do not appear in
// sceKernelGetProcessTime. Horizon freezes the whole process on HOME or sleep,
// which would otherwise reach the guest as one enormous frame delta.
void switch_advance_guest_start_tick(EmuEnvState &emuenv, uint64_t delta);

// Routes stderr (Mesa and the NVK shader compiler) into the emulator log.
void switch_capture_stderr();

// Horizon does not time-slice threads of equal priority, and the JIT has no tick
// budget, so a guest spin-wait holds its core until it finishes. These give the
// guest the preemption every other host provides for free.
void switch_start_preemption_watchdog(EmuEnvState &emuenv);
void switch_stop_preemption_watchdog();

