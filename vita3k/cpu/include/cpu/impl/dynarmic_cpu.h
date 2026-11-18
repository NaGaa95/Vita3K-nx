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

#include <dynarmic/interface/A32/a32.h>

#include <cpu/state.h>

#include <memory>
#ifdef __SWITCH__
#include <atomic>
#include <vector>
#endif

class ArmDynarmicCallback;
class ArmDynarmicCP15;

class DynarmicCPU : public CPUState {
    friend class ArmDynarmicCallback;

    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::unique_ptr<ArmDynarmicCallback> cb;
    std::shared_ptr<ArmDynarmicCP15> cp15;

    bool log_mem = false;
    bool log_code = false;
    bool cpu_opt;

    std::unique_ptr<Dynarmic::A32::Jit> make_jit();

#ifdef __SWITCH__
    // Set by the watchdog before it halts this core; read once the halt lands.
    std::atomic<bool> preempt_release_core{ false };

    // Guest wait-hint (WFE/YIELD) spin tracking, only written by the executor.
    uint32_t wait_hints = 0;
    uint64_t last_wait_hint_tick = 0;

    // WFE event register: SEV advances the global sequence, SEVL sets only
    // the local flag, and WFE consumes whichever is pending before waiting.
    bool wfe_local_event = false;
    uint32_t wfe_last_seen = 0;

    // Both return whether the guest hint should also leave the JIT.
    bool wait_for_event();
    bool yield_hint();

    std::size_t code_slice_B = 0;
    std::uint64_t handled_capacity_recycles = 0;
    // Superseded lean JITs parked until destruction: a cross-thread caller
    // (invalidation, stop, a register read from the hang reporter) racing the
    // swap then touches stale state instead of freed memory.
    std::vector<std::unique_ptr<Dynarmic::A32::Jit>> retired_jits;
    void maybe_grow_code_cache();
#endif

public:
    DynarmicCPU(MemState &mem, SceUID thread_id, std::size_t processor_id, bool cpu_opt);
    ~DynarmicCPU() override;
    GuestExit run(RunMode mode) override;
    void request_stop() override;
#ifdef __SWITCH__
    void preempt(bool release_core) override;
#endif
    void clear_exclusive() override;

    uint32_t get_reg(uint8_t idx) override;
    void set_reg(uint8_t idx, uint32_t val) override;

    uint32_t get_cpsr() override;
    void set_cpsr(uint32_t val) override;

    uint32_t get_tpidruro() override;
    void set_tpidruro(uint32_t val) override;

    uint32_t get_tpidrurw() override;
    void set_tpidrurw(uint32_t val) override;

    float get_float_reg(uint8_t idx) override;
    void set_float_reg(uint8_t idx, float val) override;

    uint32_t get_fpscr() override;
    void set_fpscr(uint32_t val) override;

    CPUContext get_context() override;
    void set_context(const CPUContext &ctx) override;

    void set_log_code(bool log) override;
    void set_log_mem(bool log) override;
    bool get_log_mem() override;
    void invalidate_jit_cache(Address start, size_t length) override;
};
