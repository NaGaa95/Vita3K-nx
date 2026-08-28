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

#ifdef __SWITCH__
// For the shared Horizon code-memory region backing every Jit's code cache.
#include <oaknut/code_block.hpp>
#endif

#include "cpu/common.h"
#include <cpu/functions.h>
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/state.h>
#include <util/log.h>

#include <mem/functions.h>
#include <mem/ptr.h>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <bit>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifdef __SWITCH__
#include <switch.h>

#include <algorithm>
#endif

#ifdef __SWITCH__
SwitchJitPoolShutdownResult switch_shutdown_jit_code_pool() {
    const auto result = oaknut::switch_detail::code_pool_shutdown();
    return {
        .regions_closed = result.regions_closed,
        .live_slices = result.live_slices,
        .close_result = result.close_rc,
    };
}
#endif

class ArmDynarmicCP15 : public Dynarmic::A32::Coprocessor {
    uint32_t tpidruro;
    uint32_t sctlr;
    uint32_t dacr;

public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    explicit ArmDynarmicCP15()
        : tpidruro(0)
        , sctlr(0)
        , dacr(0) {
    }

    ~ArmDynarmicCP15() override = default;

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd,
        CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return std::nullopt;
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
        CoprocReg CRm, unsigned opc2) override {
        // MCR p15, 0, Rt, c13, c0, 3 — write TPIDRURO
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // MCR p15, 0, Rt, c1, c0, 0 — write SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MCR p15, 0, Rt, c3, c0, 0 — write DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MCR: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        // MRC p15, 0, Rt, c13, c0, 3 — read TPIDRURO (thread-local storage)
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // MRC p15, 0, Rt, c1, c0, 0 — read SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MRC p15, 0, Rt, c3, c0, 0 — read DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MRC: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    void set_tpidruro(uint32_t tpidruro) {
        this->tpidruro = tpidruro;
    }

    uint32_t get_tpidruro() const {
        return tpidruro;
    }
};

#ifdef __SWITCH__
// ARM event register: one global sequence plus each thread's last consumed
// value. SEV raises it for everyone; a thread with a pending event is one whose
// last_seen lags. Waits park on Horizon's address arbiter, whose in-kernel
// value check closes the lost-wakeup race.
static std::atomic<uint32_t> g_wfe_seq{ 0 };
static std::atomic<uint32_t> g_wfe_waiters{ 0 };
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));

static void *wfe_seq_word() {
    return const_cast<uint32_t *>(reinterpret_cast<const uint32_t *>(&g_wfe_seq));
}

static void wfe_signal_event() {
    g_wfe_seq.fetch_add(1, std::memory_order_seq_cst);
    if (g_wfe_waiters.load(std::memory_order_seq_cst) != 0)
        svcSignalToAddress(wfe_seq_word(), SignalType_Signal, 0, -1);
}

// A successful exclusive store cleared other cores' reservations. Gated on
// sleepers so a normal STREX pays one load; the racing window this leaves is
// covered by the wait timeout.
static void wfe_exclusive_store_event() {
    if (g_wfe_waiters.load(std::memory_order_seq_cst) != 0)
        wfe_signal_event();
}

static const bool g_wfe_hook_installed = [] {
    Dynarmic::g_exclusive_store_event = wfe_exclusive_store_event;
    return true;
}();

// Slice a JIT is grown to once capacity recycles prove its translation
// working set does not fit the lean size.
static constexpr std::size_t GROWN_CODE_SLICE_B = 32u * 1024u * 1024u;
#endif

class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {
    friend class DynarmicCPU;

    CPUState *parent;
    DynarmicCPU *cpu;

public:
    explicit ArmDynarmicCallback(CPUState &parent, DynarmicCPU &cpu)
        : parent(&parent)
        , cpu(&cpu) {}

    ~ArmDynarmicCallback() override = default;

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr addr) override {
        if (cpu->log_mem)
            LOG_TRACE("Instruction fetch at address 0x{:X}", addr);
        return MemoryRead32(addr);
    }

    static void TraceInstruction(uint64_t self_, uint64_t address, uint64_t is_thumb) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);

        std::string disassembly = [&]() -> std::string {
            if (!address || !Ptr<uint32_t>{ (uint32_t)address }.valid(*self.parent->mem)) {
                return "invalid address";
            }
            return disassemble(*self.parent, address);
        }();
        LOG_TRACE("{} ({}): {} {}", log_hex(self_), self.parent->thread_id, log_hex(address), disassembly);
    }

    void PreCodeTranslationHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (cpu->log_code) {
            ir.CallHostFunction(&TraceInstruction, ir.Imm64((uint64_t)this), ir.Imm64(pc), ir.Imm64(is_thumb));
        }
    }

    template <typename T>
    T MemoryRead(Dynarmic::A32::VAddr addr) {
        Ptr<T> ptr{ addr };
        if (!ptr || !is_valid_addr_range_size(*parent->mem, addr, sizeof(T))
            || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid read of uint{}_t at address: 0x{:x}\n{}", sizeof(T) * 8, addr, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return 0;
        }

        T ret = *ptr.get(*parent->mem);
        if (cpu->log_mem) {
            LOG_TRACE("Read uint{}_t at address: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, ret);
        }
        return ret;
    }

    uint8_t MemoryRead8(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint8_t>(addr);
    }

    uint16_t MemoryRead16(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint16_t>(addr);
    }

    uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint32_t>(addr);
    }

    uint64_t MemoryRead64(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint64_t>(addr);
    }

    template <typename T>
    void MemoryWrite(Dynarmic::A32::VAddr addr, T value) {
        Ptr<T> ptr{ addr };
        if (!ptr || !is_valid_addr_range_size(*parent->mem, addr, sizeof(T))
            || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid write of uint{}_t at addr: 0x{:x}, val = 0x{:x}\n{}", sizeof(T) * 8, addr, value, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return;
        }

        *ptr.get(*parent->mem) = value;
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, value);
        }
    }

    void MemoryWrite8(Dynarmic::A32::VAddr addr, uint8_t value) override {
        MemoryWrite<uint8_t>(addr, value);
    }

    void MemoryWrite16(Dynarmic::A32::VAddr addr, uint16_t value) override {
        MemoryWrite<uint16_t>(addr, value);
    }

    void MemoryWrite32(Dynarmic::A32::VAddr addr, uint32_t value) override {
        MemoryWrite<uint32_t>(addr, value);
    }

    void MemoryWrite64(Dynarmic::A32::VAddr addr, uint64_t value) override {
        MemoryWrite<uint64_t>(addr, value);
    }

    template <typename T>
    bool MemoryWriteExclusive(Dynarmic::A32::VAddr addr, T value, T expected) {
        Ptr<T> ptr{ addr };
        if (!ptr || !is_valid_addr_range_size(*parent->mem, addr, sizeof(T))
            || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid exclusive write of uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}\n{}", sizeof(T) * 8, addr, value, expected, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return false;
        }

        auto result = Ptr<T>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}", sizeof(T) * 8, addr, value, expected);
        }
        return result;
    }

    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr addr, uint8_t value, uint8_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr addr, uint16_t value, uint16_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr addr, uint32_t value, uint32_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr addr, uint64_t value, uint64_t expected) override {
        return MemoryWriteExclusive(addr, value, expected); // Ptr<uint64_t>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
    }

    void InterpreterFallback(Dynarmic::A32::VAddr addr, size_t num_insts) override {
        LOG_ERROR("Unimplemented instruction at address {}:\n{}", log_hex(addr), save_context(*parent).description());
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::Breakpoint: {
            cpu->break_ = true;
            cpu->jit->HaltExecution();
            if (cpu->is_thumb_mode())
                cpu->set_pc(pc | 1);
            else
                cpu->set_pc(pc);
            break;
        }
        case Dynarmic::A32::Exception::WaitForInterrupt: {
            cpu->halted = true;
            cpu->jit->HaltExecution();
            break;
        }
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadInstruction:
            // Prefetch hints do not suspend the guest and are common in hot
            // copy loops; they must not force a JIT exit on Switch.
            break;
        case Dynarmic::A32::Exception::SendEvent:
#ifdef __SWITCH__
            wfe_signal_event();
#endif
            break;
        case Dynarmic::A32::Exception::SendEventLocal:
#ifdef __SWITCH__
            parent->wfe_local_event = true;
#endif
            break;
        case Dynarmic::A32::Exception::WaitForEvent:
#ifdef __SWITCH__
            // Vita titles poll with WFE. Honour the hint rather than letting
            // the linked JIT run monopolize an application core.
            if (cpu->wait_for_event())
                cpu->jit->HaltExecution();
#endif
            break;
        case Dynarmic::A32::Exception::Yield:
#ifdef __SWITCH__
            if (cpu->yield_hint())
                cpu->jit->HaltExecution();
#endif
            break;
        case Dynarmic::A32::Exception::UndefinedInstruction:
            LOG_WARN("Undefined instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::UnpredictableInstruction:
            LOG_WARN("Unpredictable instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::DecodeError: {
            LOG_WARN("Decode error at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        }
        default:
            LOG_WARN("Unknown exception {} Raised at pc = 0x{:x}", static_cast<size_t>(exception), pc);
            LOG_TRACE("at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
        }
    }

    void CallSVC(uint32_t svc) override {
#ifdef __SWITCH__
        // Reaching the kernel means the thread got past whatever it polled on.
        parent->wait_hints = 0;
#endif
        parent->svc_called = true;
        parent->svc = svc;
        cpu->jit->HaltExecution(Dynarmic::HaltReason::UserDefined8);
    }

    void AddTicks(uint64_t ticks) override {}

    uint64_t GetTicksRemaining() override {
        return 1ull << 60;
    }
};

#ifdef __SWITCH__
// A yield only reaches threads of equal or higher priority, so a guest polling
// on a lower-priority producer spins against it forever. Spin while the wait
// still looks short, then yield, then sleep - only a sleep frees the core.
static constexpr uint32_t WAIT_HINT_SPIN_LIMIT = 16;
static constexpr uint32_t WAIT_HINT_YIELD_LIMIT = 64;
static constexpr uint64_t WAIT_HINT_SLEEP_NS = 50'000;
static constexpr uint64_t WAIT_HINT_SLEEP_MAX_NS = 1'000'000;
static constexpr uint32_t WAIT_HINT_SLEEP_MAX_SHIFT = 5;
// Horizon's system counter runs at 19.2 MHz; 2 ms in ticks.
static constexpr uint64_t WAIT_HINT_RESET_TICKS = 38'400;

bool DynarmicCPU::yield_hint() {
    const uint64_t now = armGetSystemTick();
    // A hint long after the last one belongs to a fresh wait, not this spin.
    if (now - parent->last_wait_hint_tick > WAIT_HINT_RESET_TICKS)
        parent->wait_hints = 0;
    parent->last_wait_hint_tick = now;

    const uint32_t hints = ++parent->wait_hints;
    if (hints <= WAIT_HINT_SPIN_LIMIT)
        return false;
    if (hints <= WAIT_HINT_YIELD_LIMIT) {
        svcSleepThread(0);
        return true;
    }

    // The wait is not short: only a sleep frees the core for a lower-priority
    // producer, and backing off bounds what a long wait costs in wakeups.
    const uint32_t shift = std::min(hints - WAIT_HINT_YIELD_LIMIT, WAIT_HINT_SLEEP_MAX_SHIFT);
    svcSleepThread(std::min(WAIT_HINT_SLEEP_NS << shift, WAIT_HINT_SLEEP_MAX_NS));
    return true;
}

bool DynarmicCPU::wait_for_event() {

    // A pending event makes WFE fall straight through: SEVL first, then the
    // broadcast sequence, consumed by catching last_seen up to it.
    if (parent->wfe_local_event) {
        parent->wfe_local_event = false;
        parent->wait_hints = 0;
        return false;
    }
    uint32_t seen = g_wfe_seq.load(std::memory_order_seq_cst);
    if (seen != parent->wfe_last_seen) {
        parent->wfe_last_seen = seen;
        parent->wait_hints = 0;
        return false;
    }

    const uint64_t now = armGetSystemTick();
    if (now - parent->last_wait_hint_tick > WAIT_HINT_RESET_TICKS)
        parent->wait_hints = 0;
    parent->last_wait_hint_tick = now;

    const uint32_t hints = ++parent->wait_hints;
    if (hints <= WAIT_HINT_SPIN_LIMIT)
        return false;

    // Park on the arbiter: SEV and monitor-clear end the wait at once, and the
    // escalating timeout covers flags written without either.
    const uint32_t shift = std::min(hints - WAIT_HINT_SPIN_LIMIT, WAIT_HINT_SLEEP_MAX_SHIFT);
    const s64 timeout = static_cast<s64>(std::min(WAIT_HINT_SLEEP_NS << shift, WAIT_HINT_SLEEP_MAX_NS));

    g_wfe_waiters.fetch_add(1, std::memory_order_seq_cst);
    seen = g_wfe_seq.load(std::memory_order_seq_cst);
    Result rc = 0;
    if (seen == parent->wfe_last_seen)
        rc = svcWaitForAddress(wfe_seq_word(), ArbitrationType_WaitIfEqual,
            static_cast<s64>(static_cast<s32>(seen)), timeout);
    g_wfe_waiters.fetch_sub(1, std::memory_order_seq_cst);
    parent->wfe_last_seen = g_wfe_seq.load(std::memory_order_seq_cst);

    if (rc == KERNELRESULT(TimedOut)) {
    } else if (R_SUCCEEDED(rc) || rc == KERNELRESULT(InvalidState)) {
        // Signalled, or the sequence moved before the wait began.
        parent->wait_hints = 0;
    } else {
        // The arbiter refused the wait; sleep so this can never degenerate
        // back into a hot spin.
        svcSleepThread(timeout);
    }
    return true;
}
#endif

#ifdef __SWITCH__
void DynarmicCPU::maybe_grow_code_cache() {
    if (code_slice_B >= GROWN_CODE_SLICE_B)
        return;
    const std::uint64_t recycles = jit->CapacityRecycleCount();
    if (recycles == handled_capacity_recycles)
        return;
    handled_capacity_recycles = recycles;
    // One recycle can be one-off churn; a second means the working set does not
    // fit and the thread will thrash.
    if (recycles < 2)
        return;

    // Between Run() calls no generated code is live and the recycle already
    // discarded every block, so only the Jit object is rebuilt. The old one is
    // parked rather than freed (see retired_jits).
    auto context = save_context();
    code_slice_B = GROWN_CODE_SLICE_B;
    retired_jits.push_back(std::exchange(jit, make_jit()));
    load_context(context);
    LOG_INFO("[switch] guest thread {} JIT grown to {} MiB after {} capacity recycles",
        parent->thread_id, GROWN_CODE_SLICE_B >> 20, recycles);
}
#endif

Dynarmic::ExclusiveMonitor DynarmicCPU::shared_monitor(MAX_CORE_COUNT);

std::unique_ptr<Dynarmic::A32::Jit> DynarmicCPU::make_jit() {
    Dynarmic::A32::UserConfig config{};
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.callbacks = cb.get();
    if (parent->mem->use_page_table) {
        config.page_table = (log_mem || !cpu_opt) ? nullptr : reinterpret_cast<decltype(config.page_table)>(parent->mem->page_table.get());
        config.absolute_offset_page_table = true;
    } else if (!log_mem && cpu_opt) {
        config.fastmem_pointer = std::bit_cast<uintptr_t>(parent->mem->memory.get());
    }
    config.hook_hint_instructions = true;
    config.global_monitor = &shared_monitor;
    config.coprocessors[15] = cp15;
    config.processor_id = core_id;
    config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
    config.enable_cycle_counting = false;

#ifdef __SWITCH__
    // Each JIT takes a slice of the shared Horizon code-memory pool, which
    // works around Horizon's low limit on concurrent code-memory objects. The
    // first guest thread is the game's main thread and carries the hot
    // translation working set, so it gets a large slice; the rest stay lean and
    // are grown on demand by maybe_grow_code_cache().
    if (code_slice_B == 0) {
        static std::atomic<bool> hot_slice_taken{ false };
        const std::size_t code_slice = oaknut::switch_detail::code_pool_slice_size();
        const std::size_t lean_slice = code_slice ? code_slice : (8 * 1024 * 1024);
        code_slice_B = !hot_slice_taken.exchange(true) ? GROWN_CODE_SLICE_B : lean_slice;
    }
    config.code_cache_size = code_slice_B;
#endif

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

DynarmicCPU::DynarmicCPU(CPUState *state, std::size_t processor_id, bool cpu_opt)
    : parent(state)
    , cb(std::make_unique<ArmDynarmicCallback>(*state, *this))
    , cp15(std::make_shared<ArmDynarmicCP15>())
    , core_id(processor_id)
    , cpu_opt(cpu_opt) {
#ifdef __SWITCH__
    static const bool logged_code_cache_size = [] {
        LOG_INFO("[switch] Dynarmic code cache: 32 MiB main-thread slice, {} MiB per worker JIT, shared Horizon pool",
            oaknut::switch_detail::code_pool_slice_size() / (1024u * 1024u));
        return true;
    }();
    (void)logged_code_cache_size;

    try {
        jit = make_jit();
    } catch (const std::exception &e) {
        extern volatile unsigned int g_switch_last_jit_rc;
        const unsigned int rc = g_switch_last_jit_rc;
        u64 mem_avail = 0, mem_used = 0;
        svcGetInfo(&mem_avail, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        LOG_CRITICAL("[switch] Dynarmic JIT creation failed ({}). jitCreate rc=0x{:08X} (module={}, desc={}). mem total={} MiB used={} MiB free={} MiB",
            e.what(), rc, rc & 0x1FF, (rc >> 9) & 0x1FFF,
            mem_avail >> 20, mem_used >> 20, (mem_avail - mem_used) >> 20);
        throw;
    }
#else
    jit = make_jit();
#endif
}

DynarmicCPU::~DynarmicCPU() = default;

#ifdef __SWITCH__
// Horizon schedules strictly by priority and the JIT has no tick budget, so a
// guest spin-waiting on a plain load holds its core indefinitely and a sibling
// of equal priority never runs. The watchdog halts such a thread here; the
// return to the host yields, or sleeps, and hands the core over.
constexpr int64_t PREEMPT_RELEASE_NS = 100'000;
void DynarmicCPU::preempt(const bool release_core) {
    preempt_release_core.store(release_core, std::memory_order_relaxed);
    jit->HaltExecution(Dynarmic::HaltReason::UserDefined7);
}
#endif

int DynarmicCPU::run() {
    halted = false;
    break_ = false;
    parent->svc_called = false;
    Dynarmic::HaltReason halt_reason;
    while (true) {
        halt_reason = jit->Run();
#ifdef __SWITCH__
        maybe_grow_code_cache();
        // A preemption request carries no other meaning, so it is consumed here
        // and never reaches the thread loop. Anything else raised alongside it -
        // a syscall, a breakpoint - still has to be handled, so only yield when
        // preemption is all there was.
        if (halt_reason == Dynarmic::HaltReason::UserDefined7) {
            // A yield only offers the core to equal or higher priorities. When
            // the thread this one is spinning on sits below it - a worker pool
            // waiting on its own scheduler thread - nothing but giving the core
            // up outright will ever let that thread run.
            svcSleepThread(preempt_release_core.load(std::memory_order_relaxed)
                    ? PREEMPT_RELEASE_NS
                    : 0);
            continue;
        }
        halt_reason = halt_reason & ~Dynarmic::HaltReason::UserDefined7;
#endif
        if ((halt_reason != Dynarmic::HaltReason::Step) && (halt_reason != Dynarmic::HaltReason::CacheInvalidation))
            break;
    }

    return halted;
}

int DynarmicCPU::step() {
    parent->svc_called = false;
    jit->Step();
    return 0;
}

bool DynarmicCPU::hit_breakpoint() {
    return break_;
}

void DynarmicCPU::trigger_breakpoint() {
    break_ = true;
    stop();
}

void DynarmicCPU::set_log_code(bool log) {
    if (log_code == log)
        return;

    log_code = log;
    jit = make_jit();
}

void DynarmicCPU::set_log_mem(bool log) {
    if (log_mem == log)
        return;

    log_mem = log;
    jit = make_jit();
}

bool DynarmicCPU::get_log_code() {
    return log_code;
}

bool DynarmicCPU::get_log_mem() {
    return log_mem;
}

void DynarmicCPU::stop() {
    jit->HaltExecution();
}

uint32_t DynarmicCPU::get_reg(uint8_t idx) {
    return jit->Regs()[idx];
}

uint32_t DynarmicCPU::get_sp() {
    return jit->Regs()[13];
}

uint32_t DynarmicCPU::get_pc() {
    return jit->Regs()[15];
}

void DynarmicCPU::set_reg(uint8_t idx, uint32_t val) {
    jit->Regs()[idx] = val;
}

void DynarmicCPU::set_cpsr(uint32_t val) {
    jit->SetCpsr(val);
}

uint32_t DynarmicCPU::get_tpidruro() {
    return cp15->get_tpidruro();
}

void DynarmicCPU::set_tpidruro(uint32_t val) {
    cp15->set_tpidruro(val);
}

void DynarmicCPU::set_pc(uint32_t val) {
    if (val & 1) {
        set_cpsr(get_cpsr() | 0x20);
        val = val & 0xFFFFFFFE;
    } else {
        set_cpsr(get_cpsr() & 0xFFFFFFDF);
        val = val & 0xFFFFFFFC;
    }
    jit->Regs()[15] = val;
}

void DynarmicCPU::set_lr(uint32_t val) {
    jit->Regs()[14] = val;
}

void DynarmicCPU::set_sp(uint32_t val) {
    jit->Regs()[13] = val;
}

uint32_t DynarmicCPU::get_cpsr() {
    return jit->Cpsr();
}

uint32_t DynarmicCPU::get_fpscr() {
    return jit->Fpscr();
}

void DynarmicCPU::set_fpscr(uint32_t val) {
    jit->SetFpscr(val);
}

CPUContext DynarmicCPU::save_context() {
    CPUContext ctx;
    ctx.cpu_registers = jit->Regs();
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(ctx.fpu_registers.data(), jit->ExtRegs().data(), sizeof(ctx.fpu_registers));
    ctx.fpscr = jit->Fpscr();
    ctx.cpsr = jit->Cpsr();

    return ctx;
}

void DynarmicCPU::load_context(const CPUContext &ctx) {
    jit->Regs() = ctx.cpu_registers;
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(jit->ExtRegs().data(), ctx.fpu_registers.data(), sizeof(ctx.fpu_registers));
    jit->SetCpsr(ctx.cpsr);
    jit->SetFpscr(ctx.fpscr);
}

uint32_t DynarmicCPU::get_lr() {
    return jit->Regs()[14];
}

float DynarmicCPU::get_float_reg(uint8_t idx) {
    return std::bit_cast<float>(jit->ExtRegs()[idx]);
}

void DynarmicCPU::set_float_reg(uint8_t idx, float val) {
    jit->ExtRegs()[idx] = std::bit_cast<uint32_t>(val);
}

bool DynarmicCPU::is_thumb_mode() {
    return jit->Cpsr() & 0x20;
}

std::size_t DynarmicCPU::processor_id() const {
    return core_id;
}

void DynarmicCPU::invalidate_jit_cache(Address start, size_t length) {
    jit->InvalidateCacheRange(start, length);
}

void DynarmicCPU::clear_exclusive() {
    shared_monitor.ClearProcessor(core_id);
}
