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

#include <cpu/state.h>
#include <kernel/callback.h>
#include <kernel/types.h>
#include <mem/block.h>
#include <mem/ptr.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

struct CPUContext;

struct ThreadState;
struct ThreadParams;
struct KernelState;

typedef std::unique_ptr<CPUState, std::function<void(CPUState *)>> CPUStatePtr;
typedef std::function<void(CPUState &, uint32_t, SceUID)> CallImport;
typedef std::function<std::string(Address)> ResolveNIDName;

enum class ThreadStatus {
    run, // Running
    dormant, // Waiting for a job
    suspend, // Suspended by debugger
    wait, // Waiting to be awaken by sync object or operation
};

// Thread status waits are sometimes performed while holding the guest sync
// primitive's mutex and sometimes while holding ThreadState::mutex. A regular
// std::condition_variable cannot safely coordinate those different mutexes, and
// an atomic status alone still permits a notification to land between the
// predicate check and the wait. This wrapper uses one internal mutex for the
// actual condition variable and handshakes with the caller's lock before it is
// released, closing that lost-wakeup window without polling.
class ThreadStatusCondition {
public:
    template <typename Lock, typename Predicate>
    void wait(Lock &external_lock, Predicate predicate) {
        while (!predicate()) {
            std::unique_lock<std::mutex> signal_lock(signal_mutex);
            if (predicate())
                return;

            external_lock.unlock();
            signal_condition.wait(signal_lock);
            signal_lock.unlock();
            external_lock.lock();
        }
    }

    template <typename Lock, typename Rep, typename Period, typename Predicate>
    bool wait_for(Lock &external_lock, const std::chrono::duration<Rep, Period> &timeout, Predicate predicate) {
        if (predicate())
            return true;

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            std::unique_lock<std::mutex> signal_lock(signal_mutex);
            if (predicate())
                return true;

            external_lock.unlock();
            const auto result = signal_condition.wait_until(signal_lock, deadline);
            signal_lock.unlock();
            external_lock.lock();

            if (predicate())
                return true;
            if (result == std::cv_status::timeout)
                return false;
        }
        return true;
    }

    void notify_one() {
        const std::lock_guard<std::mutex> lock(signal_mutex);
        signal_condition.notify_one();
    }

    void notify_all() {
        const std::lock_guard<std::mutex> lock(signal_mutex);
        signal_condition.notify_all();
    }

private:
    std::mutex signal_mutex;
    std::condition_variable signal_condition;
};

struct ThreadSignal {
    ThreadSignal() = default;
    ~ThreadSignal() = default;

    void wait();
    bool send();

private:
    std::mutex mutex;
    std::condition_variable recv_cond;
    bool signaled = false;
};

struct ThreadState {
    std::mutex mutex;
    std::string name;
    SceUID id;
    Address entry_point;

    Block stack;
    int stack_size;
    Block tls;

    int priority;
    SceInt32 affinity_mask;
    uint64_t start_tick;
    uint64_t last_vblank_waited;
    // set to true if thread is processing kernel callbacks
    bool is_processing_callbacks = false;

    CPUStatePtr cpu;
    // Status is observed under several different guest-primitive mutexes. Keep
    // the value atomic; ThreadStatusCondition provides the matching wakeup
    // ordering for waits.
    std::atomic<ThreadStatus> status{ ThreadStatus::dormant };

    // The import this thread is currently executing, 0 when it is running guest
    // code. A thread blocked inside an HLE call keeps ThreadStatus::run and a
    // frozen guest pc, so without this a host-side stall is indistinguishable
    // from guest code that simply is not being scheduled.
    std::atomic<uint32_t> current_import_nid{ 0 };

    ThreadSignal signal;
    std::vector<CallbackPtr> callbacks;
    ThreadStatusCondition status_cond;
    std::vector<std::shared_ptr<ThreadState>> waiting_threads;
    uint32_t returned_value = 0;

    ThreadState() = delete;
    explicit ThreadState(SceUID id, KernelState &kernel, MemState &mem);

    int init(const char *name, Ptr<const void> entry_point, int init_priority, SceInt32 affinity_mask, int stack_size, const SceKernelThreadOptParam *option);
    int start(SceSize arglen, const Ptr<void> argp, bool run_entry_callback = false);
    void exit(SceInt32 status);
    void exit_delete(bool exit = true);

    void update_status(ThreadStatus status, std::optional<ThreadStatus> expected = std::nullopt);
    Address stack_top() const;

    void run_loop();
    void raise_waiting_threads();

    // this function must be called from the thread itself (inside a svc call)
    uint32_t run_callback(Address callback_address, const std::vector<uint32_t> &args);

    // this function is called from another thread when this one is dormant
    // it is only used for module loading and gxm display queue right now
    // args and argp are passed to thread->start as is
    uint32_t run_guest_function(Address callback_address, SceSize args = 0, const Ptr<void> argp = Ptr<void>{});

    void suspend();
    void suspend_and_wait();
    void resume(bool step = false);
    void resume_if_suspended();
    // For a thread that is not running when a pause begins. suspend() cannot be
    // used on one: it asserts the thread is running and stops its CPU. These only
    // raise and clear the request, so a thread that wakes mid-pause honours it at
    // its next JIT re-entry instead of running on through the pause.
    void request_suspend();
    void cancel_suspend();
    bool is_suspend_requested() const { return suspend_requested.load(std::memory_order_relaxed); }
    bool is_delete_requested() const { return delete_requested.load(std::memory_order_acquire); }
    std::string log_stack_traceback() const;

private:
    void push_arguments(const std::vector<uint32_t> &args);
    void dispatch_abort(CPUState &cpu);

    KernelState &kernel;

    CPUContext init_cpu_ctx;
    // sceKernelExitThread (or top-level guest function return): park at dormant, thread reusable via start() / run_guest_function().
    bool exit_requested = false;
    // sceKernelExitDeleteThread (or external kill): will return from top-level run_loop(), then host thread joins.
    // Read by status wait predicates that do not hold ThreadState::mutex.
    std::atomic<bool> delete_requested{ false };
    // Set by suspend(), consumed in run_loop() to transition to ThreadStatus::suspend.
    // Atomic so the preemption watchdog can tell a thread the kernel is stopping
    // from one spinning in translated code, without taking ThreadState::mutex.
    std::atomic<bool> suspend_requested{ false };
    bool vm_suspended = false;
    // Single stepping mode.
    bool single_stepping = false;

    // Number of active run_loop frames. The top-level host thread keeps one
    // frame alive (run_loop()) while parked dormant; callbacks add nested frames.
    int call_level = 0;

    // when calling sceKernelStartThread
    bool run_start_callback = false;
    // when calling sceKernelExitThread or sceKernelExitDeleteThread
    bool run_end_callback = false;

    MemState &mem;
};

typedef std::shared_ptr<ThreadState> ThreadStatePtr;
