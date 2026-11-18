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
#include <cpu/arm.h>
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/state.h>
#include <util/log.h>

#include <mem/functions.h>
#include <mem/ptr.h>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

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

// Dynarmic asserts on unhandled coprocessor operations, even in release builds.
static std::uint64_t coproc_noop(void *, std::uint32_t, std::uint32_t) {
    return 0;
}

static Dynarmic::A32::Coprocessor::Callback coproc_noop_callback() {
    return Dynarmic::A32::Coprocessor::Callback{ &coproc_noop, std::nullopt };
}

class ArmDynarmicNullCP : public Dynarmic::A32::Coprocessor {
public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    ~ArmDynarmicNullCP() override = default;

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return coproc_noop_callback();
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override {
        return coproc_noop_callback();
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return coproc_noop_callback();
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override {
        return coproc_noop_callback();
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        return coproc_noop_callback();
    }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return coproc_noop_callback();
    }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return coproc_noop_callback();
    }
};

class ArmDynarmicCP15 : public Dynarmic::A32::Coprocessor {
public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    uint32_t tpidruro = 0;
    uint32_t tpidrurw = 0;
    uint32_t sctlr = 0;
    uint32_t dacr = 0;

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd,
        CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return coproc_noop_callback();
    }

    // MCR p15, 0, Rt, CRn, c0, opc2 for TPIDRURO (c13, 3), TPIDRURW (c13, 2), SCTLR (c1, 0) and DACR (c3, 0)
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
        CoprocReg CRm, unsigned opc2) override {
        if (opc1 == 0 && CRm == CoprocReg::C0) {
            if (CRn == CoprocReg::C13 && opc2 == 3)
                return &tpidruro;
            if (!two && CRn == CoprocReg::C13 && opc2 == 2)
                return &tpidrurw;
            if (!two && CRn == CoprocReg::C1 && opc2 == 0)
                return &sctlr;
            if (!two && CRn == CoprocReg::C3 && opc2 == 0)
                return &dacr;
        }
        LOG_WARN("Unhandled CP15 MCR or MRC: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, static_cast<int>(CRn), static_cast<int>(CRm), opc2);
        return coproc_noop_callback();
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return coproc_noop_callback();
    }

    // MRC reads the same registers
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return CompileSendOneWord(two, opc1, CRn, CRm, opc2);
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return coproc_noop_callback();
    }

    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return coproc_noop_callback();
    }

    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return coproc_noop_callback();
    }
};

static constexpr bool LOG_INVALID_ACCESS = false;

// Why the JIT halts, as the mask Run() and Step() return
static constexpr Dynarmic::HaltReason HALT_STOP = Dynarmic::HaltReason::UserDefined1;
static constexpr Dynarmic::HaltReason HALT_RETURN = Dynarmic::HaltReason::UserDefined2;
static constexpr Dynarmic::HaltReason HALT_BREAKPOINT = Dynarmic::HaltReason::UserDefined3;
static constexpr Dynarmic::HaltReason HALT_FAULT = Dynarmic::HaltReason::UserDefined4;
static constexpr Dynarmic::HaltReason HALT_IMPORT = Dynarmic::HaltReason::UserDefined8;
#ifdef __SWITCH__
static constexpr Dynarmic::HaltReason HALT_PREEMPT = Dynarmic::HaltReason::UserDefined7;

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
static constexpr std::size_t MAIN_CODE_SLICE_B = 64u * 1024u * 1024u;
#endif

class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {
    DynarmicCPU &cpu;

public:
    explicit ArmDynarmicCallback(DynarmicCPU &cpu)
        : cpu(cpu) {}

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr addr) override {
        if (cpu.log_mem)
            LOG_TRACE("Instruction fetch at address 0x{:X}", addr);
        if (addr < KiB(4))
            return std::nullopt;
        return MemoryRead32(addr);
    }

    static void TraceInstruction(uint64_t self_, uint64_t address, uint64_t is_thumb) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);

        const bool valid = is_valid_addr(self.cpu.mem, static_cast<Address>(address));
        const std::string disassembly = valid ? self.cpu.disassemble(address) : "invalid address";
        LOG_TRACE("{} ({}): {} {}", log_hex(self_), self.cpu.thread_id, log_hex(address), disassembly);
    }

    inline static std::recursive_timed_mutex loader_lock_mutex;
    inline static std::atomic<int> loader_lock_timeouts{ 0 };
    int loader_lock_held = 0;

    static void MonoLoaderLockAcquire(uint64_t self_, uint64_t, uint64_t) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);
        if (self.loader_lock_held > 0) {
            ++self.loader_lock_held;
            return;
        }
        // A halted guest can strand the lock; do not block other threads indefinitely.
        if (!loader_lock_mutex.try_lock_for(std::chrono::seconds(5))) {
            if (loader_lock_timeouts.fetch_add(1) < 20)
                LOG_WARN("Timed out waiting for the emulated mono loader lock on thread {}, proceeding unserialised", self.cpu.thread_id);
            return;
        }
        self.loader_lock_held = 1;
    }

    static void MonoLoaderLockRelease(uint64_t self_, uint64_t, uint64_t) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);
        if (self.loader_lock_held == 0)
            return;
        if (--self.loader_lock_held == 0)
            loader_lock_mutex.unlock();
    }

    enum class MonoLoaderOp {
        None,
        Lock,
        Unlock,
    };

    static constexpr bool is_movw(uint32_t insn, uint32_t rd) {
        return (insn & 0xFFF0F000) == (0xE3000000 | (rd << 12));
    }

    static constexpr bool is_movt(uint32_t insn, uint32_t rd) {
        return (insn & 0xFFF0F000) == (0xE3400000 | (rd << 12));
    }

    // Matches unexported Mono loader lock routines by shape; their mutex is compiled out.
    MonoLoaderOp classify_mono_loader_lock(Dynarmic::A32::VAddr pc) {
        static constexpr uint32_t PUSH_R4_LR = 0xE92D4010;
        static constexpr uint32_t LDR_R0_R0 = 0xE5900000;
        static constexpr uint32_t CMP_R0_0 = 0xE3500000;
        static constexpr uint32_t BEQ_EPILOGUE = 0x0A00000B; // beq +11 words -> the pop below
        static constexpr uint32_t LDR_R4_R0 = 0xE5904000;
        static constexpr uint32_t MOV_R0_R4 = 0xE1A00004;
        static constexpr uint32_t BLX_IP = 0xE12FFF3C;
        static constexpr uint32_t ADD_R1_R0_1 = 0xE2801001; // lock:   nest + 1
        static constexpr uint32_t SUB_R1_R0_1 = 0xE2401001; // unlock: nest - 1
        static constexpr uint32_t POP_R4_PC = 0xE8BD8010;
        static constexpr size_t BODY_WORDS = 19;

        // Address validation accepts the allocated but inaccessible null page.
        if (pc < cpu.mem.host_page_size || !is_valid_addr_range_size(cpu.mem, pc, sizeof(uint32_t)))
            return MonoLoaderOp::None;

        const uint32_t *w = Ptr<uint32_t>(static_cast<Address>(pc)).get(cpu.mem);
        if (w[0] != PUSH_R4_LR)
            return MonoLoaderOp::None;
        if (!is_valid_addr_range_size(cpu.mem, pc, sizeof(uint32_t) * BODY_WORDS))
            return MonoLoaderOp::None;

        const bool shape = is_movw(w[1], 0) && is_movt(w[2], 0) && w[3] == LDR_R0_R0
            && w[4] == CMP_R0_0 && w[5] == BEQ_EPILOGUE
            && is_movw(w[6], 0) && is_movt(w[7], 0) && w[8] == LDR_R4_R0
            && is_movw(w[9], 12) && is_movt(w[10], 12) && w[11] == MOV_R0_R4 && w[12] == BLX_IP
            && is_movw(w[13], 12) && is_movt(w[15], 12) && w[16] == MOV_R0_R4 && w[17] == BLX_IP
            && w[18] == POP_R4_PC;
        if (!shape)
            return MonoLoaderOp::None;

        if (w[14] == ADD_R1_R0_1)
            return MonoLoaderOp::Lock;
        if (w[14] == SUB_R1_R0_1)
            return MonoLoaderOp::Unlock;
        return MonoLoaderOp::None;
    }

    void PreCodeTranslationHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        // Matched routines start unconditionally, so the host call needs no predicate.
        if (!is_thumb) {
            const MonoLoaderOp op = classify_mono_loader_lock(pc);
            if (op != MonoLoaderOp::None) {
                LOG_INFO_ONCE("Serialising mono's loader lock: it takes no mutex in this build");
                ir.CallHostFunction(op == MonoLoaderOp::Lock ? &MonoLoaderLockAcquire : &MonoLoaderLockRelease,
                    ir.Imm64((uint64_t)this), ir.Imm64(0), ir.Imm64(0));
            }
        }
        if (cpu.log_code) {
            ir.CallHostFunction(&TraceInstruction, ir.Imm64(reinterpret_cast<uint64_t>(this)), ir.Imm64(pc), ir.Imm64(is_thumb));
        }
    }

    // Logs and returns true if guest code may not access the size bytes at addr
    bool invalid_access(const char *access, Dynarmic::A32::VAddr addr, uint32_t size) {
        MemState &mem = cpu.mem;
        // A page without host backing has a null page-table entry
        if (addr >= mem.host_page_size && is_valid_addr_range_size(mem, addr, size) && Ptr<uint8_t>(addr).get(mem))
            return false;
        const uint32_t pc = cpu.get_pc();
        if (LOG_INVALID_ACCESS) {
            LOG_ERROR("Invalid {} at 0x{:x}\n{}", access, addr, cpu.get_context().description());
            if (pc < mem.host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", cpu.disassemble(pc, nullptr));
        } else if (!mem.invalid_access_logged.test_and_set()) {
            LOG_ERROR("Invalid {} at 0x{:x} by thread {} (pc 0x{:x}). Set LOG_INVALID_ACCESS for full reports", access, addr, cpu.thread_id, pc);
        } else {
            LOG_ERROR("Invalid {} at 0x{:x} by thread {} (pc 0x{:x})", access, addr, cpu.thread_id, pc);
        }
        return true;
    }

    template <typename T>
    T MemoryRead(Dynarmic::A32::VAddr addr) {
        if (invalid_access("read", addr, sizeof(T)))
            return 0;
        const T ret = *Ptr<T>(addr).get(cpu.mem);
        if (cpu.log_mem) {
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
        if (invalid_access("write", addr, sizeof(T)))
            return;
        *Ptr<T>(addr).get(cpu.mem) = value;
        if (cpu.log_mem) {
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
        if (invalid_access("exclusive write", addr, sizeof(T)))
            return false;
        const bool result = Ptr<T>(addr).atomic_compare_and_swap(cpu.mem, value, expected);
        if (cpu.log_mem) {
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
        return MemoryWriteExclusive(addr, value, expected);
    }

    void InterpreterFallback(Dynarmic::A32::VAddr addr, size_t num_insts) override {
        LOG_ERROR("Unimplemented instruction at address {}:\n{}", log_hex(addr), cpu.get_context().description());
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::Breakpoint:
            cpu.jit->HaltExecution(HALT_BREAKPOINT);
            cpu.set_pc(cpu.is_thumb_mode() ? pc | arm::THUMB_BIT : pc);
            break;
        case Dynarmic::A32::Exception::NoExecuteFault:
            LOG_CRITICAL("PC is 0x{:x} in thread {}, ending the app\n{}", pc, cpu.thread_id, cpu.get_context().description());
            cpu.jit->HaltExecution(HALT_FAULT);
            break;
        case Dynarmic::A32::Exception::WaitForInterrupt:
            cpu.jit->HaltExecution(HALT_RETURN);
            break;
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
            cpu.wfe_local_event = true;
#endif
            break;
        case Dynarmic::A32::Exception::WaitForEvent:
#ifdef __SWITCH__
            // Vita titles poll with WFE. Honour the hint rather than letting
            // the linked JIT run monopolize an application core.
            if (cpu.wait_for_event())
                cpu.jit->HaltExecution(HALT_STOP);
#endif
            break;
        case Dynarmic::A32::Exception::Yield:
#ifdef __SWITCH__
            if (cpu.yield_hint())
                cpu.jit->HaltExecution(HALT_STOP);
#endif
            break;
        case Dynarmic::A32::Exception::UndefinedInstruction:
            LOG_WARN("Undefined instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), cpu.disassemble(pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::UnpredictableInstruction:
            LOG_WARN("Unpredictable instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), cpu.disassemble(pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::DecodeError:
            LOG_WARN("Decode error at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), cpu.disassemble(pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        default:
            LOG_WARN("Unknown exception {} Raised at pc = 0x{:x}", static_cast<size_t>(exception), pc);
            LOG_TRACE("at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), cpu.disassemble(pc, nullptr));
        }
    }

    void CallSVC(uint32_t) override {
#ifdef __SWITCH__
        // Reaching the kernel means the thread got past whatever it polled on.
        cpu.wait_hints = 0;
#endif
        cpu.jit->HaltExecution(HALT_IMPORT);
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
    if (now - last_wait_hint_tick > WAIT_HINT_RESET_TICKS)
        wait_hints = 0;
    last_wait_hint_tick = now;

    const uint32_t hints = ++wait_hints;
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
    if (wfe_local_event) {
        wfe_local_event = false;
        wait_hints = 0;
        return false;
    }
    uint32_t seen = g_wfe_seq.load(std::memory_order_seq_cst);
    if (seen != wfe_last_seen) {
        wfe_last_seen = seen;
        wait_hints = 0;
        return false;
    }

    const uint64_t now = armGetSystemTick();
    if (now - last_wait_hint_tick > WAIT_HINT_RESET_TICKS)
        wait_hints = 0;
    last_wait_hint_tick = now;

    const uint32_t hints = ++wait_hints;
    if (hints <= WAIT_HINT_SPIN_LIMIT)
        return false;

    // Park on the arbiter: SEV and monitor-clear end the wait at once, and the
    // escalating timeout covers flags written without either.
    const uint32_t shift = std::min(hints - WAIT_HINT_SPIN_LIMIT, WAIT_HINT_SLEEP_MAX_SHIFT);
    const s64 timeout = static_cast<s64>(std::min(WAIT_HINT_SLEEP_NS << shift, WAIT_HINT_SLEEP_MAX_NS));

    g_wfe_waiters.fetch_add(1, std::memory_order_seq_cst);
    seen = g_wfe_seq.load(std::memory_order_seq_cst);
    Result rc = 0;
    if (seen == wfe_last_seen)
        rc = svcWaitForAddress(wfe_seq_word(), ArbitrationType_WaitIfEqual,
            static_cast<s64>(static_cast<s32>(seen)), timeout);
    g_wfe_waiters.fetch_sub(1, std::memory_order_seq_cst);
    wfe_last_seen = g_wfe_seq.load(std::memory_order_seq_cst);

    if (rc == KERNELRESULT(TimedOut)) {
    } else if (R_SUCCEEDED(rc) || rc == KERNELRESULT(InvalidState)) {
        // Signalled, or the sequence moved before the wait began.
        wait_hints = 0;
    } else {
        // The arbiter refused the wait; sleep so this can never degenerate
        // back into a hot spin.
        svcSleepThread(timeout);
    }
    return true;
}

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
    auto context = get_context();
    code_slice_B = GROWN_CODE_SLICE_B;
    retired_jits.push_back(std::exchange(jit, make_jit()));
    set_context(context);
    LOG_INFO("[switch] guest thread {} JIT grown to {} MiB after {} capacity recycles",
        thread_id, GROWN_CODE_SLICE_B >> 20, recycles);
}
#endif

static Dynarmic::ExclusiveMonitor shared_monitor(MAX_CORE_COUNT);

std::unique_ptr<Dynarmic::A32::Jit> DynarmicCPU::make_jit() {
    Dynarmic::A32::UserConfig config{};
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.callbacks = cb.get();
    if (mem.use_page_table) {
        config.page_table = (log_mem || !cpu_opt) ? nullptr : reinterpret_cast<decltype(config.page_table)>(mem.page_table.get());
        config.absolute_offset_page_table = true;
    } else if (!log_mem && cpu_opt) {
        config.fastmem_pointer = std::bit_cast<uintptr_t>(mem.memory.get());
    }
    config.hook_hint_instructions = true;
    config.global_monitor = &shared_monitor;
    config.coprocessors[arm::CP15] = cp15;
    static const std::shared_ptr<ArmDynarmicNullCP> null_cp = std::make_shared<ArmDynarmicNullCP>();
    for (auto &coproc : config.coprocessors) {
        if (!coproc)
            coproc = null_cp;
    }
    config.processor_id = processor_id;
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
        code_slice_B = !hot_slice_taken.exchange(true) ? MAIN_CODE_SLICE_B : lean_slice;
    }
    config.code_cache_size = code_slice_B;
#endif

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

DynarmicCPU::DynarmicCPU(MemState &mem, SceUID thread_id, std::size_t processor_id, bool cpu_opt)
    : CPUState(mem, thread_id, processor_id)
    , cb(std::make_unique<ArmDynarmicCallback>(*this))
    , cp15(std::make_shared<ArmDynarmicCP15>())
    , cpu_opt(cpu_opt) {
#ifdef __SWITCH__
    static const bool logged_code_cache_size = [] {
        LOG_INFO("[switch] Dynarmic code cache: 64 MiB main-thread slice, {} MiB per worker JIT, shared Horizon pool",
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
    jit->HaltExecution(HALT_PREEMPT);
}
#endif

GuestExit DynarmicCPU::run(RunMode mode) {
    const bool step = mode == RunMode::single_step;
    Dynarmic::HaltReason reason;
    if (step) {
        reason = jit->Step();
    } else {
        // Run until the JIT stops for something other than a step or a cache invalidation
        while (true) {
            reason = jit->Run();
#ifdef __SWITCH__
            maybe_grow_code_cache();
            // A preemption request carries no other meaning, so it is consumed here
            // and never reaches the thread loop. Anything else raised alongside it -
            // an import, a breakpoint - still has to be handled, so only yield when
            // preemption is all there was.
            if (reason == HALT_PREEMPT) {
                // A yield only offers the core to equal or higher priorities. When
                // the thread this one is spinning on sits below it - a worker pool
                // waiting on its own scheduler thread - nothing but giving the core
                // up outright will ever let that thread run.
                svcSleepThread(preempt_release_core.load(std::memory_order_relaxed)
                        ? PREEMPT_RELEASE_NS
                        : 0);
                continue;
            }
            reason = reason & ~HALT_PREEMPT;
#endif
            if (reason != Dynarmic::HaltReason::Step && reason != Dynarmic::HaltReason::CacheInvalidation)
                break;
        }
    }
    // The NID is the word after the SVC. A stepped SVC reports the debugger stop with it.
    if (Dynarmic::Has(reason, HALT_IMPORT))
        return { GuestExit::Kind::import, *Ptr<uint32_t>(get_pc() + arm::ARM_INSTRUCTION_SIZE).get(mem), step };
    if (Dynarmic::Has(reason, HALT_FAULT))
        return { GuestExit::Kind::fault };
    // A run that halted stopped at the return address, and a step never returns
    if (!step && Dynarmic::Has(reason, HALT_RETURN))
        return { GuestExit::Kind::returned, get_reg(0) };
    // A breakpoint, or a step that ran its instruction. A step that a pending stop request ended
    // before its instruction has neither.
    return { GuestExit::Kind::stopped, 0, Dynarmic::Has(reason, Dynarmic::HaltReason::Step | HALT_BREAKPOINT) };
}

void DynarmicCPU::clear_exclusive() {
    shared_monitor.ClearProcessor(processor_id);
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

bool DynarmicCPU::get_log_mem() {
    return log_mem;
}

void DynarmicCPU::request_stop() {
    jit->HaltExecution(HALT_STOP);
}

uint32_t DynarmicCPU::get_reg(uint8_t idx) {
    return jit->Regs()[idx];
}

void DynarmicCPU::set_reg(uint8_t idx, uint32_t val) {
    jit->Regs()[idx] = val;
}

void DynarmicCPU::set_cpsr(uint32_t val) {
    jit->SetCpsr(val);
}

uint32_t DynarmicCPU::get_tpidruro() {
    return cp15->tpidruro;
}

void DynarmicCPU::set_tpidruro(uint32_t val) {
    cp15->tpidruro = val;
}

uint32_t DynarmicCPU::get_tpidrurw() {
    return cp15->tpidrurw;
}

void DynarmicCPU::set_tpidrurw(uint32_t val) {
    cp15->tpidrurw = val;
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

CPUContext DynarmicCPU::get_context() {
    CPUContext ctx;
    ctx.cpu_registers = jit->Regs();
    ctx.fpu_registers = std::bit_cast<std::array<float, 64>>(jit->ExtRegs());
    ctx.fpscr = jit->Fpscr();
    ctx.cpsr = jit->Cpsr();
    ctx.tpidrurw = cp15->tpidrurw;
    return ctx;
}

void DynarmicCPU::set_context(const CPUContext &ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->ExtRegs() = std::bit_cast<std::array<uint32_t, 64>>(ctx.fpu_registers);
    jit->SetCpsr(ctx.cpsr);
    jit->SetFpscr(ctx.fpscr);
    cp15->tpidrurw = ctx.tpidrurw;
}

float DynarmicCPU::get_float_reg(uint8_t idx) {
    return std::bit_cast<float>(jit->ExtRegs()[idx]);
}

void DynarmicCPU::set_float_reg(uint8_t idx, float val) {
    jit->ExtRegs()[idx] = std::bit_cast<uint32_t>(val);
}

void DynarmicCPU::invalidate_jit_cache(Address start, size_t length) {
    jit->InvalidateCacheRange(start, length);
}
