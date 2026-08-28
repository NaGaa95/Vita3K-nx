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

#include <cpu/common.h>
#include <cpu/disasm/state.h>
#include <mem/state.h>
#include <util/types.h>

#include <atomic>

struct CPUState {
    CPUState() = default;

    SceUID thread_id = 0;
    MemState *mem = nullptr;
    DisasmState disasm;

    CPUInterfacePtr cpu;
    bool svc_called;
    uint32_t svc;

    // Exception handler support (kubridge abort handlers)
    // These are set by the page fault callback (signal-safe atomics)
    // and consumed by run_loop after HaltExecution returns.
    std::atomic<bool> abort_pending{ false };
    std::atomic<uint32_t> abort_fault_addr{ 0 };
    std::atomic<bool> abort_is_write{ false };

#ifdef __SWITCH__
    // Guest wait-hint (WFE/YIELD) spin tracking. Only the owning thread writes
    // the first two; the totals are read by the hang reporter.
    uint32_t wait_hints = 0;
    uint64_t last_wait_hint_tick = 0;

    // WFE event register: SEV advances the global sequence, SEVL sets only
    // the local flag, and WFE consumes whichever is pending before waiting.
    bool wfe_local_event = false;
    uint32_t wfe_last_seen = 0;

    // Incremented on every import; the preemption watchdog treats a thread whose
    // count is frozen across a quantum as spinning in translated code.
    std::atomic<uint64_t> import_serial{ 0 };
#endif
};
