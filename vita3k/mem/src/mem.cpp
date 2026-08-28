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

#include <mem/functions.h>
#include <mem/state.h>

#include <util/align.h>
#include <util/log.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif defined(__SWITCH__)
// Horizon has no mmap/mprotect/POSIX signals. The guest's flat 4 GiB address
// space is represented by Vita3K's page table, while a low host-address pool is
// reserved with libnx virtmem. Neither svcMapPhysicalMemory (needs
// SystemResourceSize, which is 0 for hbloader apps) nor svcMapPhysicalMemoryUnsafe
// (the unsafe pool is unavailable in this memory config) can back it. Instead we
// commit pages by aliasing them, via svcMapMemory, out of a physically-backed
// pool carved from the newlib heap — the same primitive libnx's JIT uses.
// Access-violation-driven features (fault-based lazy commit, GPU write-protection
// sync) are unavailable — memory is committed explicitly at allocation time.
#include <switch.h>
#ifdef BIT
#undef BIT
#endif
#ifdef BITL
#undef BITL
#endif
#include <malloc.h>
#include <unistd.h>
#include <vector>
extern "C" {
extern char *fake_heap_end;
}
// The reservation backing state.memory (needed to release the address space).
static VirtmemReservation *g_switch_mem_reservation = nullptr;
// Guest reservation base in the Stack region (reserved early, before threads and
// the renderer fragment that region).
static void *g_switch_base = nullptr;
// Size of the physically-backed host region reserved in the Stack region (the
// largest contiguous slice). The guest address space is the full TOTAL_MEM_SIZE;
// guest pages are mapped onto this smaller host region via the page table.
static size_t g_switch_host_size = 0;
// Set when a guest page cannot be committed, so init() aborts cleanly instead of
// faulting on the following memset.
static bool g_switch_commit_failed = false;

// Physically-backed pool (heap memory) whose pages are aliased into the guest
// reservation with svcMapMemory. Tracks which pool page backs each guest page so
// frees can unmap and recycle it.
static uint8_t *g_switch_pool = nullptr;
// Free host-page runs as {start_slot -> page_count}, kept coalesced. Each guest
// allocation is backed by a CONTIGUOUS run of host pages: guest code and the
// module loader do bulk memcpy/memset across a whole guest allocation, which only
// lands correctly when the underlying host pages are contiguous. A LIFO free-list
// fragments after frees (e.g. resolved var-import stubs) and silently corrupts
// those bulk operations, so a run allocator is mandatory, not an optimisation.
static std::map<uint32_t, uint32_t> g_switch_pool_runs;
static size_t g_switch_pool_free_pages = 0;
static std::vector<int32_t> g_switch_guest_pool; // guest page -> pool page (-1 = unmapped)
static std::vector<int32_t> g_switch_pool_guest; // pool page -> guest page (-1 = free); reverse map for host->guest (Ptr(host, mem))
static std::mutex g_switch_pool_mutex;
static bool g_switch_pool_ready = false;
// If a per-session alias cannot be removed, a new title must not reuse the
// surviving mapping with a fresh allocator/page table.
static bool g_switch_pool_cleanup_failed = false;

static bool switch_pool_init(size_t pool_bytes, uint32_t total_pages) {
    if (g_switch_pool_ready)
        return true;
    g_switch_pool = static_cast<uint8_t *>(memalign(0x1000, pool_bytes));
    if (!g_switch_pool) {
        LOG_CRITICAL("Failed to allocate {} byte guest memory pool", pool_bytes);
        return false;
    }
    const uint32_t pool_pages = static_cast<uint32_t>(pool_bytes / 0x1000);
    try {
        g_switch_pool_runs.clear();
        g_switch_pool_runs[0] = pool_pages; // one big free run
        g_switch_pool_free_pages = pool_pages;
        g_switch_guest_pool.assign(total_pages, -1);
        g_switch_pool_guest.assign(pool_pages, -1);
    } catch (const std::bad_alloc &) {
        LOG_CRITICAL("Failed to allocate guest memory pool metadata ({} guest pages, {} pool pages)",
            total_pages, pool_pages);
        g_switch_pool_runs.clear();
        g_switch_pool_free_pages = 0;
        g_switch_guest_pool.clear();
        g_switch_pool_guest.clear();
        free(g_switch_pool);
        g_switch_pool = nullptr;
        return false;
    }
    g_switch_pool_ready = true;
    g_switch_pool_cleanup_failed = false;
    LOG_INFO("Guest memory pool: {} bytes ({} pages) at {}", pool_bytes, pool_pages, fmt::ptr(g_switch_pool));
    return true;
}

// Allocate a contiguous run of n free pool pages (first-fit). Returns the start
// slot, or UINT32_MAX if no single run is large enough. Caller holds the pool mutex.
static uint32_t switch_pool_alloc_run(uint32_t n) {
    for (auto it = g_switch_pool_runs.begin(); it != g_switch_pool_runs.end(); ++it) {
        if (it->second >= n) {
            const uint32_t start = it->first;
            const uint32_t remain = it->second - n;
            g_switch_pool_runs.erase(it);
            if (remain > 0)
                g_switch_pool_runs[start + n] = remain;
            g_switch_pool_free_pages -= n;
            return start;
        }
    }
    return UINT32_MAX;
}

// Return run [start, start+n) to the free set, coalescing with adjacent runs.
static void switch_pool_free_run(uint32_t start, uint32_t n) {
    auto it = g_switch_pool_runs.lower_bound(start);
    if (it != g_switch_pool_runs.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == start) { // predecessor ends at start
            start = prev->first;
            n += prev->second;
            g_switch_pool_runs.erase(prev);
        }
    }
    auto succ = g_switch_pool_runs.find(start + n); // successor begins at end
    if (succ != g_switch_pool_runs.end()) {
        n += succ->second;
        g_switch_pool_runs.erase(succ);
    }
    g_switch_pool_runs[start] = n;
    g_switch_pool_free_pages += n;
}

// Permanently remove a single pool slot from the free set (splitting its run). Used
// when a stray kernel mapping (a thread's TLS/stack) has taken that slot's
// destination inside our reservation — the Switch kernel does not honour
// virtmemAddReservation — so it can never be committed and the allocator must route
// around it. Caller holds the pool mutex.
static void switch_pool_block_slot(uint32_t pp) {
    auto it = g_switch_pool_runs.upper_bound(pp); // first run starting after pp
    if (it == g_switch_pool_runs.begin())
        return; // nothing starts at/before pp -> not currently free
    --it;
    const uint32_t start = it->first;
    const uint32_t count = it->second;
    if (pp < start || pp >= start + count)
        return; // pp is not in a free run (already allocated or blocked)
    g_switch_pool_runs.erase(it);
    if (pp > start)
        g_switch_pool_runs[start] = pp - start; // left remainder
    if (pp + 1 < start + count)
        g_switch_pool_runs[pp + 1] = (start + count) - (pp + 1); // right remainder
    g_switch_pool_free_pages -= 1;
}

// Unmap each pool-backed guest page in [start, start+size) and recycle its pool
// page. Mirrors the mprotect(PROT_NONE)/svcUnmapPhysicalMemory decommit path.
static bool switch_decommit_range(MemState &state, uint64_t start, uint64_t size) {
    std::lock_guard<std::mutex> lock(g_switch_pool_mutex);
    const uint32_t gp0 = static_cast<uint32_t>(start / 0x1000);
    const uint32_t npages = static_cast<uint32_t>(size / 0x1000);
    bool all_unmapped = true;
    uint32_t i = 0;
    while (i < npages) {
        const int32_t pp = g_switch_guest_pool[gp0 + i];
        if (pp < 0) {
            i++;
            continue;
        }
        // Extend a maximal run of consecutive guest pages backed by consecutive pool
        // slots, so it unmaps in one svcUnmapMemory (mirrors the batched map above and
        // keeps the kernel memory-block count low).
        uint32_t runlen = 1;
        while (i + runlen < npages && g_switch_guest_pool[gp0 + i + runlen] == pp + static_cast<int32_t>(runlen))
            runlen++;
        uint8_t *const host = state.memory.get() + static_cast<size_t>(pp) * 0x1000;
        void *const src = g_switch_pool + static_cast<size_t>(pp) * 0x1000;
        const Result rc = svcUnmapMemory(host, src, static_cast<size_t>(runlen) * 0x1000);
        if (R_FAILED(rc)) {
            // The kernel alias is still live. Keep both maps and keep the pool
            // run allocated; recycling it would give two guest ranges the same
            // physical backing and corrupt both.
            LOG_CRITICAL("svcUnmapMemory failed: 0x{:X}; retaining {} pool page(s)", rc, runlen);
            all_unmapped = false;
            i += runlen;
            continue;
        }
        for (uint32_t k = 0; k < runlen; k++) {
            if (state.use_page_table)
                state.page_table[gp0 + i + k] = state.memory.get();
            g_switch_guest_pool[gp0 + i + k] = -1;
            g_switch_pool_guest[pp + k] = -1;
        }
        switch_pool_free_run(static_cast<uint32_t>(pp), runlen); // coalesces into runs
        i += runlen;
    }
    return all_unmapped;
}

// Reverse of the page table: convert a HOST pointer (into state.memory, which is
// laid out by POOL SLOT) back to its guest address. The naive `host - memory`
// used by Ptr(T*, mem) is correct only for the linear desktop mapping; on Switch
// the guest->host mapping is non-linear (guest page -> arbitrary pool slot), so it
// returned the pool-slot offset instead of the guest address (e.g. guest
// 0x88126130 whose page mapped to pool slot 0x08126 came back as 0x08126130 --
// looking like a "bit-31 clear"). Any HLE code that builds a Ptr from a host
// pointer (sceClibMspaceCreate, ...) needs this. Returns 0 if the host pointer is
// not inside a mapped pool slot.
Address switch_host_to_guest(const MemState &state, const void *host) {
    const uint8_t *const base = state.memory.get();
    const uint8_t *const h = static_cast<const uint8_t *>(host);
    if (!h || h < base)
        return 0;
    const size_t byte_off = static_cast<size_t>(h - base);
    const uint32_t pp = static_cast<uint32_t>(byte_off / 0x1000);
    const uint32_t in_page = static_cast<uint32_t>(byte_off % 0x1000);
    if (pp >= g_switch_pool_guest.size() || g_switch_pool_guest[pp] < 0)
        return 0;
    return static_cast<uint32_t>(g_switch_pool_guest[pp]) * 0x1000u + in_page;
}
#else
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#endif

constexpr uint32_t STANDARD_PAGE_SIZE = KiB(4);
// Full 32-bit Vita guest address space. On Switch this is the address range the
// allocator and page table cover; the physical/host backing is smaller (only the
// contiguous slice we can reserve in the Stack region) and mapped in via the page
// table, which lets high guest pages (e.g. the eboot at 0x81000000) live on low
// host pages.
constexpr size_t TOTAL_MEM_SIZE = GiB(4);
constexpr bool LOG_PROTECT = false;
#ifdef NDEBUG
constexpr bool PAGE_NAME_TRACKING = false;
#else
constexpr bool PAGE_NAME_TRACKING = true;
#endif

// TODO: support multiple handlers
static AccessViolationHandler access_violation_handler;
static void register_access_violation_handler(const AccessViolationHandler &handler);

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force);
static void delete_memory(uint8_t *memory);

#ifdef _WIN32
static std::string get_error_msg() {
    return std::system_category().message(GetLastError());
}
#else
static std::string get_error_msg() {
    return strerror(errno);
}
#endif

#ifdef __SWITCH__
bool switch_reserve_guest_region() {
    if (g_switch_base) {
        if (g_switch_pool_cleanup_failed) {
            LOG_CRITICAL("Guest page-table pool still has aliases from the previous session");
            return false;
        }
        return true;
    }
    u64 stack_a = 0, stack_s = 0;
    svcGetInfo(&stack_a, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&stack_s, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);

    // The Stack region (the only valid svcMapMemory destination) is split by the
    // main thread stack. Scan it for the largest free contiguous chunk (a random
    // virtmemFindStack under-reports it), and reserve that.
    const u64 region_end = stack_a + stack_s;
    u64 best_addr = 0, best_size = 0, addr = stack_a;
    while (addr < region_end) {
        MemoryInfo mi = {};
        u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)))
            break;
        u64 blk_end = mi.addr + mi.size;
        if (blk_end > region_end)
            blk_end = region_end;
        if ((mi.type & 0xFF) == MemType_Unmapped) {
            const u64 fs = mi.addr < stack_a ? stack_a : mi.addr;
            if (blk_end > fs && blk_end - fs > best_size) {
                best_size = blk_end - fs;
                best_addr = fs;
            }
        }
        if (blk_end <= addr)
            break;
        addr = blk_end;
    }

    constexpr u64 ALIGN = 0x200000; // 2 MiB
    const u64 base = (best_addr + ALIGN - 1) & ~(ALIGN - 1);
    u64 avail = (base < best_addr + best_size) ? (best_addr + best_size - base) : 0;
    avail &= ~(ALIGN - 1);
    u64 want = avail < TOTAL_MEM_SIZE ? avail : TOTAL_MEM_SIZE;

    // The full Vita address space remains reserved virtually, but eagerly backing
    // every byte would waste unified RAM and starve NVK. Scale physical backing
    // from Horizon's actual memory class, while preserving a stock-console-safe
    // amount of ordinary newlib heap after the pool allocation.
    u64 total_memory = 0, heap_base = 0;
    svcGetInfo(&total_memory, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&heap_base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    const u64 heap_end = reinterpret_cast<u64>(fake_heap_end);
    const u64 committed_heap = heap_end > heap_base ? heap_end - heap_base : 0;
    constexpr u64 MIN_POOL = 0x20000000; // 512 MiB hard minimum
    constexpr u64 PREFERRED_POOL_MIN = 0x30000000; // 768 MiB on stock memory
    constexpr u64 PREFERRED_POOL_MAX = 0x50000000; // 1.25 GiB on RAM mods
    constexpr u64 HOST_HEAP_RESERVE = 0x40000000; // 1 GiB after guest backing
    const u64 scaled_pool = total_memory / 6;
    const u64 preferred_pool = std::clamp(scaled_pool, PREFERRED_POOL_MIN, PREFERRED_POOL_MAX) & ~(ALIGN - 1);
    want = std::min(want, preferred_pool);
    if (committed_heap > HOST_HEAP_RESERVE + MIN_POOL)
        want = std::min(want, (committed_heap - HOST_HEAP_RESERVE) & ~(ALIGN - 1));

    LOG_INFO("Guest backing plan: total={} MiB, committed host heap={} MiB, preferred pool={} MiB, selected={} MiB, host reserve={} MiB",
        total_memory >> 20, committed_heap >> 20, preferred_pool >> 20, want >> 20, HOST_HEAP_RESERVE >> 20);

    if (want < MIN_POOL) {
        LOG_CRITICAL("Stack region [0x{:X}+0x{:X}] largest free chunk 0x{:X} is too small", stack_a, stack_s, best_size);
        return false;
    }

    virtmemLock();
    g_switch_mem_reservation = virtmemAddReservation(reinterpret_cast<void *>(base), want);
    virtmemUnlock();
    if (!g_switch_mem_reservation) {
        LOG_CRITICAL("virtmemAddReservation(0x{:X}, 0x{:X}) failed", base, want);
        return false;
    }
    g_switch_base = reinterpret_cast<void *>(base);
    g_switch_host_size = want;
    LOG_INFO("Host backing reserved at 0x{:X} (0x{:X}); Stack region [0x{:X}+0x{:X}], largest free 0x{:X}; guest space 0x{:X}",
        base, want, stack_a, stack_s, best_size, static_cast<u64>(TOTAL_MEM_SIZE));
    // Pool sized to the host backing; the guest->slot map spans the full guest space.
    if (switch_pool_init(want, static_cast<uint32_t>(TOTAL_MEM_SIZE / STANDARD_PAGE_SIZE)))
        return true;

    // Keep reservation and physical backing transactional: a later retry must
    // not mistake a bare address-space reservation for a usable guest pool.
    virtmemLock();
    virtmemRemoveReservation(g_switch_mem_reservation);
    virtmemUnlock();
    g_switch_mem_reservation = nullptr;
    g_switch_base = nullptr;
    g_switch_host_size = 0;
    return false;
}

bool switch_release_page_table_region() {
    std::lock_guard<std::mutex> lock(g_switch_pool_mutex);
    bool aliases_released = true;

    // First unmap any pool pages still aliased into the reservation (svcMapMemory),
    // so no kernel mappings linger in the Stack region: hbloader lays the next
    // NRO's stacks there and faults (0xD401) on a leftover alias. host and src are
    // both indexed by pool slot, so a contiguous run of committed slots unmaps in a
    // single call. Install runs no guest code, so in practice this loop is empty.
    if (g_switch_base && g_switch_pool) {
        const size_t pool_pages = g_switch_pool_guest.size();
        for (size_t pp = 0; pp < pool_pages;) {
            if (g_switch_pool_guest[pp] < 0) {
                pp++;
                continue;
            }
            size_t runlen = 1;
            while (pp + runlen < pool_pages && g_switch_pool_guest[pp + runlen] >= 0)
                runlen++;
            uint8_t *const host = static_cast<uint8_t *>(g_switch_base) + pp * 0x1000;
            void *const src = g_switch_pool + pp * 0x1000;
            const Result rc = svcUnmapMemory(host, src, runlen * 0x1000);
            if (R_FAILED(rc)) {
                LOG_ERROR("release: svcUnmapMemory failed for pool slots {}-{}: 0x{:X}",
                    pp, pp + runlen, rc);
                aliases_released = false;
            } else {
                for (size_t k = 0; k < runlen; ++k) {
                    const int32_t gp = g_switch_pool_guest[pp + k];
                    if (gp >= 0 && static_cast<size_t>(gp) < g_switch_guest_pool.size())
                        g_switch_guest_pool[gp] = -1;
                    g_switch_pool_guest[pp + k] = -1;
                }
            }
            pp += runlen;
        }
    }

    if (!aliases_released) {
        LOG_ERROR("Guest page-table pool retained because one or more aliases are still live");
        return false;
    }

    if (g_switch_mem_reservation) {
        virtmemLock();
        virtmemRemoveReservation(g_switch_mem_reservation);
        virtmemUnlock();
        g_switch_mem_reservation = nullptr;
    }
    if (g_switch_pool) {
        free(g_switch_pool);
        g_switch_pool = nullptr;
    }
    g_switch_pool_runs.clear();
    g_switch_pool_free_pages = 0;
    g_switch_guest_pool.clear();
    g_switch_pool_guest.clear();
    g_switch_pool_ready = false;
    g_switch_pool_cleanup_failed = false;
    g_switch_base = nullptr;
    g_switch_host_size = 0;
    LOG_INFO("Guest page-table reservation + pool released.");
    return true;
}

// Release guest memory before chainloading the launcher.
void switch_release_guest_region() {
    if (!switch_release_page_table_region())
        LOG_ERROR("Switch page-table pool final cleanup was incomplete; backing retained for retry");
}
#endif

bool init(MemState &state, const bool use_page_table) {
#ifdef _WIN32
    SYSTEM_INFO system_info = {};
    GetSystemInfo(&system_info);
    state.host_page_size = system_info.dwPageSize;
#elif defined(__SWITCH__)
    state.host_page_size = 0x1000; // Horizon page size is 4 KiB.
#else
    state.host_page_size = static_cast<int>(sysconf(_SC_PAGESIZE));
#endif

    assert(state.host_page_size >= 4096); // Limit imposed by Unicorn.

    void *preferred_address = reinterpret_cast<void *>(1ULL << 34);

#ifdef _WIN32
    state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(preferred_address, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);
    if (!state.memory) {
        // fallback
        state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(nullptr, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);

        if (!state.memory) {
            LOG_CRITICAL("VirtualAlloc failed: {}", get_error_msg());
            return false;
        }
    }
#elif defined(__SWITCH__)
    (void)preferred_address;
    // The pool is normally reserved during bootstrap. Retry here so a transient
    // early reservation failure never prevents a game from launching.
    if (!switch_reserve_guest_region())
        return false;
    state.memory = Memory(static_cast<uint8_t *>(g_switch_base), delete_memory);
    g_switch_commit_failed = false;
#else
    // http://man7.org/linux/man-pages/man2/mmap.2.html
    const int prot = PROT_NONE;
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    const int fd = 0;
    const off_t offset = 0;
    // preferred_address is only a hint for mmap, if it can't use it, the kernel will choose itself the address
    state.memory = Memory(static_cast<uint8_t *>(mmap(preferred_address, TOTAL_MEM_SIZE, prot, flags, fd, offset)), delete_memory);
    if (state.memory.get() == MAP_FAILED) {
        LOG_CRITICAL("mmap failed {}", get_error_msg());
        return false;
    }
#endif

    const size_t table_length = TOTAL_MEM_SIZE / STANDARD_PAGE_SIZE;
    state.alloc_table = AllocPageTable(new AllocMemPage[table_length]);
    memset(state.alloc_table.get(), 0, sizeof(AllocMemPage) * table_length);

    state.allocator.set_maximum(table_length);

#ifdef __SWITCH__
    // Switch guest memory is backed by a non-linear low-memory pool, so the CPU
    // always uses Dynarmic's established page-table path.
    state.use_page_table = true;
#else
    state.use_page_table = use_page_table;
#endif
    if (state.use_page_table) {
        state.page_table = PageTable(new PagePtr[TOTAL_MEM_SIZE / KiB(4)]);
        // Default: identity (host == state.memory + addr) for as-yet-uncommitted pages.
        std::fill_n(state.page_table.get(), TOTAL_MEM_SIZE / KiB(4), state.memory.get());
    }

    const auto handler = [&state](uint8_t *addr, bool write) noexcept {
        return handle_access_violation(state, addr, write);
    };
    register_access_violation_handler(handler);

    const Address null_address = alloc_inner(state, 0, state.host_page_size / STANDARD_PAGE_SIZE, "null", true);
    assert(null_address == 0);
#ifdef __SWITCH__
    const uint32_t null_page_count = state.host_page_size / STANDARD_PAGE_SIZE;
    const bool null_allocation_succeeded = state.allocator.free_slot_count(0, null_page_count) == 0;
    if (!null_allocation_succeeded || g_switch_commit_failed) {
        LOG_CRITICAL("Guest memory backing is unavailable — cannot launch (see svcMapPhysicalMemory diagnostic above).");
        return false;
    }
#endif
#ifdef _WIN32
    DWORD old_protect = 0;
    const BOOL ret = VirtualProtect(state.memory.get(), state.host_page_size, PAGE_NOACCESS, &old_protect);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#elif defined(__SWITCH__)
    svcSetMemoryPermission(state.memory.get(), state.host_page_size, Perm_None);
#else
    const int ret = mprotect(state.memory.get(), state.host_page_size, PROT_NONE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif

    return true;
}

static void delete_memory(uint8_t *memory) {
    if (memory != nullptr) {
#ifdef _WIN32
        const BOOL ret = VirtualFree(memory, 0, MEM_RELEASE);
        assert(ret);
#elif defined(__SWITCH__)
        // The reservation and its aliased backing pool span the whole process,
        // not one game session. switch_release_guest_region() must retain the
        // base so it can svcUnmapMemory every alias before free(g_switch_pool).
        // Clearing it here made settings/return/exit skip the unmap pass and
        // free an inaccessible Alias source, causing a Data Abort at pool base.
        (void)memory;
#else
        munmap(memory, TOTAL_MEM_SIZE);
#endif
    }
}

bool is_valid_addr(const MemState &state, Address addr) {
    const uint32_t page_num = addr / STANDARD_PAGE_SIZE;
    return addr && state.allocator.free_slot_count(page_num, page_num + 1) == 0;
}

bool is_valid_addr_range(const MemState &state, Address start, Address end) {
    if (end < start)
        return false;
    return is_valid_addr_range_size(state, start, static_cast<uint64_t>(end) - start);
}

bool is_valid_addr_range_size(const MemState &state, const Address start, const uint64_t size) {
    if (size == 0)
        return true;

    if (static_cast<uint64_t>(start) >= TOTAL_MEM_SIZE
        || size > TOTAL_MEM_SIZE - static_cast<uint64_t>(start))
        return false;
    const uint64_t end = static_cast<uint64_t>(start) + size;

    const uint32_t start_page = start / STANDARD_PAGE_SIZE;
    const uint32_t end_page = static_cast<uint32_t>((end + STANDARD_PAGE_SIZE - 1) / STANDARD_PAGE_SIZE);
    return state.allocator.free_slot_count(start_page, end_page) == 0;
}

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force) {
    int page_num;
    if (force) {
        if (state.allocator.allocate_at(start_page, page_count) < 0) {
            return 0;
        }
        page_num = start_page;
    } else {
        page_num = state.allocator.allocate_from(start_page, page_count, false);
        if (page_num < 0)
            return 0;
    }

    const uint32_t size = page_count * STANDARD_PAGE_SIZE;
    const Address addr = page_num * STANDARD_PAGE_SIZE;

    const Address commit_start = align_down(addr, state.host_page_size);
    const Address commit_end = align(addr + size, state.host_page_size);
    const uint32_t commit_size = commit_end - commit_start;
    uint8_t *const commit_ptr = &state.memory[commit_start];

    // Make memory chunk available to access
#ifdef _WIN32
    const void *const ret = VirtualAlloc(commit_ptr, commit_size, MEM_COMMIT, PAGE_READWRITE);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#elif defined(__SWITCH__)
    // Back each guest page with a low host page from the pool, and point the
    // guest page at it via the page table. The guest address (which may be far
    // above the mappable Stack region, e.g. 0x81000000) is thereby decoupled
    // from the low host address svcMapMemory requires.
    (void)commit_ptr; // host address comes from the pool, not state.memory + addr
    std::lock_guard<std::mutex> lock(g_switch_pool_mutex);
    const uint32_t gp0 = commit_start / STANDARD_PAGE_SIZE;
    const uint32_t npages = commit_size / STANDARD_PAGE_SIZE;
    // Count pages still needing backing (normally all of them for a fresh alloc).
    uint32_t need = 0;
    for (uint32_t i = 0; i < npages; i++)
        if (g_switch_guest_pool[gp0 + i] < 0)
            need++;
    if (need != npages) {
        // A free allocator range must never retain page-table backing. Mixing
        // old slots with a new contiguous pool run breaks the host-contiguity
        // guarantee used by bulk memcpy/memset and can expose stale title RAM.
        LOG_CRITICAL("Guest allocation '{}' at 0x{:X} overlaps {} stale mapped page(s)",
            name, addr, npages - need);
        state.allocator.free(page_num, page_count);
        g_switch_commit_failed = true;
        return 0;
    }
    if (need > 0) {
        // One contiguous run backs the whole allocation, so bulk memcpy/memset
        // across its guest pages map to contiguous host pages.
        // The Switch kernel places thread TLS/stacks inside our reservation (it
        // ignores virtmemAddReservation), so a run's destination can collide with a
        // stray mapping and svcMapMemory fails (0xD401). On failure, free the run,
        // permanently BLOCK the exact slots the stray mapping occupies, and retry
        // with a different run — the pool has plenty free, it is just fragmented by
        // a few stray pages. Bounded so a genuinely-full pool still fails cleanly.
        // (Each svcMapMemory is one kernel block; hbloader apps have
        // SystemResourceSize=0, so map the whole run in a single call.)
        uint32_t run = UINT32_MAX;
        bool mapped = false;
        int total_blocked = 0;
        for (int attempt = 0; attempt < 128 && !mapped; attempt++) {
            run = switch_pool_alloc_run(need);
            if (run == UINT32_MAX)
                break; // no contiguous run of `need` pages left
            uint8_t *const try_host = state.memory.get() + static_cast<size_t>(run) * STANDARD_PAGE_SIZE;
            void *const try_src = g_switch_pool + static_cast<size_t>(run) * STANDARD_PAGE_SIZE;
            const Result ret = svcMapMemory(try_host, try_src, static_cast<size_t>(need) * STANDARD_PAGE_SIZE);
            if (R_SUCCEEDED(ret)) {
                mapped = true;
                break;
            }
            switch_pool_free_run(run, need);
            // Walk the destination; block every slot that is not Unmapped (type 0)
            // so the next attempt routes around the stray kernel mapping.
            const u64 dst_begin = reinterpret_cast<u64>(try_host);
            const u64 dst_end = dst_begin + static_cast<u64>(need) * STANDARD_PAGE_SIZE;
            const u64 res_base = reinterpret_cast<u64>(state.memory.get());
            int blocked_here = 0;
            for (u64 q = dst_begin; q < dst_end;) {
                MemoryInfo mi = {};
                u32 pi = 0;
                if (R_FAILED(svcQueryMemory(&mi, &pi, q)))
                    break;
                const u64 seg_end = mi.addr + mi.size;
                if ((mi.type & 0xFF) != 0) {
                    u64 b = mi.addr > dst_begin ? mi.addr : dst_begin;
                    const u64 be = seg_end < dst_end ? seg_end : dst_end;
                    for (; b < be; b += STANDARD_PAGE_SIZE) {
                        switch_pool_block_slot(static_cast<uint32_t>((b - res_base) / STANDARD_PAGE_SIZE));
                        blocked_here++;
                    }
                }
                if (seg_end <= q)
                    break;
                q = seg_end;
            }
            total_blocked += blocked_here;
            if (blocked_here == 0) {
                // Failed with nothing stray in the dst — not a collision we can route
                // around; stop instead of spinning.
                LOG_CRITICAL("svcMapMemory({}, {} pages) failed rc=0x{:X}, no stray dst mapping to block",
                    static_cast<void *>(try_host), need, ret);
                break;
            }
        }
        if (!mapped) {
            LOG_CRITICAL("Guest pool: could not map {} pages for guest 0x{:X} (free {} pages) after blocking {} stray reservation slot(s)",
                need, gp0 * STANDARD_PAGE_SIZE, g_switch_pool_free_pages, total_blocked);
            spdlog::default_logger()->flush();
            g_switch_commit_failed = true;
            state.allocator.free(page_num, page_count);
            return 0;
        }
        if (total_blocked > 0)
            LOG_INFO("Guest pool: routed a {}-page alloc around {} stray reservation slot(s)", need, total_blocked);
        uint8_t *const run_host = state.memory.get() + static_cast<size_t>(run) * STANDARD_PAGE_SIZE;
        std::memset(run_host, 0, static_cast<size_t>(need) * STANDARD_PAGE_SIZE);
        // Point each still-unbacked guest page at the next slot of the run.
        uint32_t j = 0;
        for (uint32_t i = 0; i < npages; i++) {
            const uint32_t gp = gp0 + i;
            if (g_switch_guest_pool[gp] >= 0)
                continue; // already backed (keeps its existing slot)
            const uint32_t pp = run + j;
            j++;
            g_switch_guest_pool[gp] = static_cast<int32_t>(pp);
            g_switch_pool_guest[pp] = static_cast<int32_t>(gp); // reverse: host->guest
            // page_table[gp] + guest_addr == host page (see Ptr::get).
            state.page_table[gp] = (state.memory.get() + static_cast<size_t>(pp) * STANDARD_PAGE_SIZE) - static_cast<size_t>(gp) * STANDARD_PAGE_SIZE;
        }
    }
#else
    const int ret = mprotect(commit_ptr, commit_size, PROT_READ | PROT_WRITE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
#ifndef __SWITCH__
    std::memset(&state.memory[addr], 0, size);
#endif

    AllocMemPage &page = state.alloc_table[page_num];
    assert(!page.allocated);
    page.allocated = 1;
    page.size = page_count;

    if (PAGE_NAME_TRACKING) {
        state.page_name_map.emplace(page_num, name);
    }

    return addr;
}

Address alloc_aligned(MemState &state, uint32_t size, const char *name, unsigned int alignment, Address start_addr) {
    if (alignment == 0)
        return alloc(state, size, name, start_addr);
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    size += alignment;
    const uint32_t page_count = align(size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, start_addr / STANDARD_PAGE_SIZE, page_count, name, false);
    const Address align_addr = align(addr, alignment);
    const uint32_t page_num = addr / STANDARD_PAGE_SIZE;
    const uint32_t align_page_num = align_addr / STANDARD_PAGE_SIZE;

    if (page_num != align_page_num) {
        AllocMemPage &page = state.alloc_table[page_num];
        AllocMemPage &align_page = state.alloc_table[align_page_num];
        const uint32_t remnant_front = align_page_num - page_num;
#ifdef __SWITCH__
        if (!switch_decommit_range(state, static_cast<uint64_t>(page_num) * STANDARD_PAGE_SIZE,
                static_cast<uint64_t>(remnant_front) * STANDARD_PAGE_SIZE)) {
            // Keep the original oversized allocation tracked rather than make
            // a still-mapped prefix available to an unrelated allocation.
            LOG_ERROR("Could not release page-table prefix for aligned allocation '{}'", name);
            return 0;
        }
#endif
        state.allocator.free(page_num, remnant_front);
        page.allocated = 0;
        align_page.allocated = 1;
        align_page.size = page.size - remnant_front;
    }

    return align_addr;
}

static void align_to_page(MemState &state, Address &addr, Address &size) {
    const Address end = align(addr + size, STANDARD_PAGE_SIZE);
    addr = align_down(addr, STANDARD_PAGE_SIZE);
    size = end - addr;
}

void unprotect_inner(MemState &state, Address addr, uint32_t size) {
    if (LOG_PROTECT) {
        fmt::print("Unprotect: {} {}\n", log_hex(addr), size);
    }
    uint8_t *addr_ptr = state.use_page_table ? state.page_table[addr / KiB(4)] : state.memory.get();

    uint8_t *target = &addr_ptr[addr];
    uint8_t *aligned_start = reinterpret_cast<uint8_t *>(
        align_down(reinterpret_cast<uintptr_t>(target), state.host_page_size));
    uint8_t *aligned_end = reinterpret_cast<uint8_t *>(align(reinterpret_cast<uintptr_t>(target + size), state.host_page_size));
    size_t aligned_size = aligned_end - aligned_start;

#ifdef _WIN32
    DWORD old_protect = 0;
    const BOOL ret = VirtualProtect(aligned_start, aligned_size, PAGE_READWRITE, &old_protect);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#elif defined(__SWITCH__)
    // Horizon has no resumable access-violation handler. Guest backing remains
    // writable and protection is advisory on this platform.
    (void)aligned_start;
    (void)aligned_size;
#else
    const int ret = mprotect(aligned_start, aligned_size, PROT_READ | PROT_WRITE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
}

void protect_inner(MemState &state, Address addr, uint32_t size, const MemPerm perm) {
    uint8_t *addr_ptr = state.use_page_table ? state.page_table[addr / KiB(4)] : state.memory.get();

    uint8_t *target = &addr_ptr[addr];
    uint8_t *aligned_start = reinterpret_cast<uint8_t *>(
        align_down(reinterpret_cast<uintptr_t>(target), state.host_page_size));
    uint8_t *aligned_end = reinterpret_cast<uint8_t *>(align(reinterpret_cast<uintptr_t>(target + size), state.host_page_size));
    size_t aligned_size = aligned_end - aligned_start;

#ifdef _WIN32
    DWORD old_protect = 0;
    const BOOL ret = VirtualProtect(aligned_start, aligned_size, (perm == MemPerm::None) ? PAGE_NOACCESS : ((perm == MemPerm::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE), &old_protect);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#elif defined(__SWITCH__)
    // Horizon has no resumable page-fault path. Generic Vita/kernel protections
    // remain advisory here.
    (void)perm;
    (void)aligned_start;
    (void)aligned_size;
#else
    const int ret = mprotect(aligned_start, aligned_size, (perm == MemPerm::None) ? PROT_NONE : ((perm == MemPerm::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE)));
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
}

bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept {
    const uintptr_t memory_addr = reinterpret_cast<uintptr_t>(state.memory.get());
    const uintptr_t fault_addr = reinterpret_cast<uintptr_t>(addr);

    Address vaddr = 0;
    const std::unique_lock<std::mutex> lock(state.protect_mutex);
    if (fault_addr < memory_addr || fault_addr >= memory_addr + TOTAL_MEM_SIZE) {
        if (state.use_page_table) {
            // this may come from an external mapping
            uint64_t addr_val = std::bit_cast<uint64_t>(addr);
            auto it = state.external_mapping.lower_bound(addr_val);
            if (it != state.external_mapping.end() && addr_val < it->first + it->second.size) {
                vaddr = static_cast<Address>(addr_val - it->first + it->second.address);
            } else {
                return false;
            }
        } else {
            return false;
        }
    } else {
        vaddr = static_cast<Address>(fault_addr - memory_addr);
    }

    if (!is_valid_addr(state, vaddr)) {
        return false;
    }
    if (LOG_PROTECT) {
        fmt::print("Access: {}\n", log_hex(vaddr));
    }

    auto it = state.protect_tree.lower_bound(vaddr);
    if (it == state.protect_tree.end()) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.host_page_size), state.host_page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    ProtectSegmentInfo &info = it->second;
    if (vaddr < it->first || vaddr >= it->first + info.size) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.host_page_size), state.host_page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    Address previous_beg = it->first;
    for (auto &[block_addr, block] : info.blocks) {
        block.callback(vaddr, write);
    }

    unprotect_inner(state, it->first, info.size);
    state.protect_tree.erase(it);

    return true;
}

bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm,
    const ProtectCallback &callback) {
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    ProtectSegmentInfo protect(size, perm);
    align_to_page(state, addr, protect.size);

    ProtectBlockInfo block;
    block.size = size;
    block.callback = callback;

    protect.blocks.emplace(addr, std::move(block));

    auto it = state.protect_tree.lower_bound(addr);
    if (it == state.protect_tree.end() || it->first + it->second.size <= addr) {
        if (it == state.protect_tree.begin())
            it = state.protect_tree.end();
        else
            --it;
    }

    while (it != state.protect_tree.end() && it->first < addr + size) {
        const Address start = std::min(it->first, addr);
        protect.size = std::max(it->first + it->second.size, addr + protect.size) - start;
        addr = start;
        protect.blocks.merge(it->second.blocks); // transfer blocks to the new protect
        protect.perm = most_restrictive_perm(protect.perm, it->second.perm);

        if (it == state.protect_tree.begin()) {
            state.protect_tree.erase(it);
            break;
        }

        // protect tree is in reverse order, so decrease it
        state.protect_tree.erase(it--);
    }

    protect_inner(state, addr, protect.size, protect.perm);

    state.protect_tree.emplace(addr, std::move(protect));
    return true;
}

bool is_protecting(MemState &state, Address addr, MemPerm *perm) {
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    auto ite = state.protect_tree.lower_bound(addr);

    if (ite != state.protect_tree.end() && addr < ite->first + ite->second.size) {
        if (perm)
            *perm = ite->second.perm;

        return true;
    }

    return false;
}

void add_external_mapping(MemState &mem, Address addr, uint32_t size, uint8_t *addr_ptr) {
    assert((size & 4095) == 0);
    if (!mem.use_page_table)
        return;

    uint64_t addr_value = std::bit_cast<uint64_t>(addr_ptr);
    uint8_t *page_table_entry = addr_ptr - addr;
    // Save the backing this range had before we redirect it (constant across a
    // contiguous allocation on both the flat desktop map and the Switch pool), so
    // remove_external_mapping can restore it without a linear-memory assumption.
    uint8_t *const original_entry = mem.page_table[addr / KiB(4)];
#ifdef __SWITCH__
    const uint32_t block_count = size / KiB(4);
    std::vector<uint8_t *> original_entries(block_count);
    bool original_backing_is_contiguous = true;
    for (uint32_t block = 0; block < block_count; block++) {
        original_entries[block] = mem.page_table[addr / KiB(4) + block];
        original_backing_is_contiguous &= original_entries[block] == original_entry;
    }

    if (original_backing_is_contiguous) {
        // Switch allocations normally come from one contiguous pool run. One
        // bulk copy is substantially cheaper than thousands of 4 KiB memcpys.
        memcpy(addr_ptr, original_entry + addr, size);
        std::fill_n(mem.page_table.get() + addr / KiB(4), block_count, page_table_entry);
    } else {
        for (uint32_t block = 0; block < block_count; block++) {
            const Address block_addr = addr + block * KiB(4);
            memcpy(addr_ptr + block * KiB(4), original_entries[block] + block_addr, KiB(4));
            mem.page_table[block_addr / KiB(4)] = page_table_entry;
        }
    }
#else
    for (int block = 0; block < size / KiB(4); block++) {
        // Read the CURRENT backing THROUGH the page table, not via &mem.memory[addr].
        // The latter is memory_base + addr, which is only the real data on the flat
        // desktop mapping; on the Switch's non-linear pool it lands outside the
        // (smaller) host backing for high guest addresses (e.g. CDRAM 0x60000000 ->
        // base + 0x60000000, well past the reservation) and data-aborts. Going through
        // the page table yields the same flat pointer on desktop and the real pool
        // slot on Switch. (Read the old entry before overwriting it below.)
        const Address block_addr = addr + block * KiB(4);
        const uint8_t *src = mem.page_table[block_addr / KiB(4)] + block_addr;
        // this is not thread write safe, but hopefully not other thread is busy copying while this happens
        memcpy(addr_ptr + block * KiB(4), src, KiB(4));
        mem.page_table[block_addr / KiB(4)] = page_table_entry;
    }
#endif

    // set the first page table entry to the original value to be able to call protect_inner
    mem.page_table[addr / KiB(4)] = mem.memory.get();
    protect_inner(mem, addr, size, MemPerm::None);
    mem.page_table[addr / KiB(4)] = page_table_entry;

    const std::unique_lock<std::mutex> lock(mem.protect_mutex);
    MemExternalMapping mapping{ addr, size, original_entry };
#ifdef __SWITCH__
    mapping.original_entries = std::move(original_entries);
#endif
    mem.external_mapping[addr_value] = std::move(mapping);
}

void remove_external_mapping(MemState &mem, uint8_t *addr_ptr, uint32_t size) {
    uint64_t addr_value = std::bit_cast<uint64_t>(addr_ptr);
    MemExternalMapping mapping;
    if (mem.use_page_table) {
        const std::unique_lock<std::mutex> lock(mem.protect_mutex);
        auto it = mem.external_mapping.find(addr_value);
        assert(it != mem.external_mapping.end());

        mapping = std::move(it->second);
        mem.external_mapping.erase(it);
    } else {
        mapping.address = static_cast<Address>(addr_ptr - mem.memory.get());
        mapping.size = size;
    }

    // remove all protections on this range
    unprotect_inner(mem, mapping.address, mapping.size);
    {
        const std::unique_lock<std::mutex> lock(mem.protect_mutex);
        auto prot_it = mem.protect_tree.lower_bound(mapping.address);
        if (prot_it != mem.protect_tree.end()
            && prot_it->first + prot_it->second.size <= mapping.address) {
            if (prot_it == mem.protect_tree.begin())
                prot_it = mem.protect_tree.end();
            else
                --prot_it;
        }

        while (prot_it != mem.protect_tree.end() && prot_it->first < mapping.address + mapping.size) {
            if (prot_it == mem.protect_tree.begin()) {
                mem.protect_tree.erase(prot_it);
                break;
            }

            mem.protect_tree.erase(prot_it--);
        }
    }

    if (mem.use_page_table) {
        // Restore the backing saved at add time. mem.memory.get() is only the real
        // data on the flat desktop map; on the Switch's non-linear pool it points
        // outside the host backing for high guest addresses and would fault/corrupt.
        // unprotect_inner reads the page table, so put the saved entry back first.
        mem.page_table[mapping.address / KiB(4)] = mapping.original_entry;
        unprotect_inner(mem, mapping.address, mapping.size);
        // copy the (possibly GPU-updated) data back to the real backing and repoint
        // the page table at it.
#ifdef __SWITCH__
        const uint32_t block_count = mapping.size / KiB(4);
        const bool original_backing_is_contiguous = mapping.original_entries.empty()
            || std::all_of(mapping.original_entries.begin(), mapping.original_entries.end(),
                [&](const uint8_t *entry) { return entry == mapping.original_entry; });
        if (original_backing_is_contiguous) {
            memcpy(mapping.original_entry + mapping.address, addr_ptr, mapping.size);
            std::fill_n(mem.page_table.get() + mapping.address / KiB(4), block_count, mapping.original_entry);
        } else {
            for (uint32_t block = 0; block < block_count; block++) {
                const Address block_addr = mapping.address + block * KiB(4);
                uint8_t *const original = mapping.original_entries[block];
                memcpy(original + block_addr, addr_ptr + block * KiB(4), KiB(4));
                mem.page_table[block_addr / KiB(4)] = original;
            }
        }
#else
        for (int block = 0; block < mapping.size / KiB(4); block++) {
            const Address block_addr = mapping.address + block * KiB(4);
            // this is not thread write safe, but hopefully not other thread is busy copying while this happens
            memcpy(mapping.original_entry + block_addr, addr_ptr + block * KiB(4), KiB(4));
            mem.page_table[block_addr / KiB(4)] = mapping.original_entry;
        }
#endif
    }
}

Address alloc(MemState &state, uint32_t size, const char *name, Address start_addr) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_count = align(size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, start_addr / STANDARD_PAGE_SIZE, page_count, name, false);
    return addr;
}

Address alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    auto addr = try_alloc_at(state, address, size, name);
    LOG_CRITICAL_IF(addr == 0, "Failed to allocate at specific page. Memory address:{}, size:{}, name:{}", log_hex(address), log_hex(size), name);
    return addr;
}

Address try_alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t wanted_page = address / STANDARD_PAGE_SIZE;
    size += address % STANDARD_PAGE_SIZE;
    const uint32_t page_count = align(size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, wanted_page, page_count, name, true);
    return addr ? address : 0;
}

Block alloc_block(MemState &mem, uint32_t size, const char *name, Address start_addr) {
    const Address address = alloc(mem, size, name, start_addr);
    return Block(address, [&mem](Address stack) {
        free(mem, stack);
    });
}

void free(MemState &state, Address address) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_num = address / STANDARD_PAGE_SIZE;
    assert(page_num >= 0);

    AllocMemPage &page = state.alloc_table[page_num];
    if (!page.allocated) {
        // The stale size may span pages now owned by other allocations.
        LOG_CRITICAL("Freeing unallocated page at 0x{:X}", address);
        return;
    }
    const uint32_t allocation_page_count = page.size;
    const Address region_start = page_num * STANDARD_PAGE_SIZE;
    const Address region_end = region_start + allocation_page_count * STANDARD_PAGE_SIZE;

#ifdef __SWITCH__
    if (!switch_decommit_range(state, region_start,
            static_cast<uint64_t>(allocation_page_count) * STANDARD_PAGE_SIZE)) {
        // Leave allocator and allocation metadata intact. The live kernel alias
        // cannot safely be handed to another guest allocation.
        LOG_ERROR("Guest allocation at 0x{:X} retained after page-table unmap failure", region_start);
        return;
    }
#endif

    page.allocated = 0;

    state.allocator.free(page_num, allocation_page_count);
    if (PAGE_NAME_TRACKING) {
        state.page_name_map.erase(page_num);
    }

#ifndef __SWITCH__
    // On Switch every committed page legitimately remaps its page-table entry to a
    // low host page, so this invariant does not hold.
    assert(!state.use_page_table || state.page_table[address / KiB(4)] == state.memory.get());
#endif
#ifdef __SWITCH__
    // The page-table backend was synchronously decommitted before publishing
    // the range as free above. Its Horizon host page is exactly 4 KiB, so no
    // shared-host-page scan is needed.
    return;
#endif

    Address host_page = align_down(region_start, state.host_page_size);
    Address batch_start = 0;
    uint32_t batch_size = 0;

    while (host_page < region_end) {
        Address host_page_end = host_page + state.host_page_size;
        uint32_t first_guest = host_page / STANDARD_PAGE_SIZE;
        uint32_t last_guest = host_page_end / STANDARD_PAGE_SIZE;

        if (state.allocator.free_slot_count(first_guest, last_guest) == (last_guest - first_guest)) {
            if (batch_size == 0)
                batch_start = host_page;
            batch_size += state.host_page_size;
        } else if (batch_size > 0) {
            uint8_t *memory = &state.memory[batch_start];
#ifdef _WIN32
            const BOOL ret = VirtualFree(memory, batch_size, MEM_DECOMMIT);
            LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#elif defined(__SWITCH__)
            (void)memory;
            switch_decommit_range(state, batch_start, batch_size);
#else
            int ret = mprotect(memory, batch_size, PROT_NONE);
            LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
            ret = madvise(memory, batch_size, MADV_DONTNEED);
            LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
            batch_size = 0;
        }
        host_page = host_page_end;
    }

    if (batch_size > 0) {
        uint8_t *memory = &state.memory[batch_start];
#ifdef _WIN32
        const BOOL ret = VirtualFree(memory, batch_size, MEM_DECOMMIT);
        LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#elif defined(__SWITCH__)
        (void)memory;
        switch_decommit_range(state, batch_start, batch_size);
#else
        int ret = mprotect(memory, batch_size, PROT_NONE);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
        ret = madvise(memory, batch_size, MADV_DONTNEED);
        LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
    }
}

uint32_t mem_available(MemState &state) {
    return state.allocator.free_slot_count(0, state.allocator.max_offset) * STANDARD_PAGE_SIZE;
}

const char *mem_name(Address address, MemState &state) {
    if (PAGE_NAME_TRACKING) {
        return state.page_name_map.find(address / STANDARD_PAGE_SIZE)->second.c_str();
    }
    return "";
}

void deinit_mem(MemState &state) {
    const std::lock_guard<std::mutex> gen_lock(state.generation_mutex);

    {
        const std::lock_guard<std::mutex> prot_lock(state.protect_mutex);
        state.protect_tree.clear();
    }

#ifdef __SWITCH__
    if (g_switch_pool_ready) {
        g_switch_pool_cleanup_failed = !switch_decommit_range(state, 0, TOTAL_MEM_SIZE);
        if (g_switch_pool_cleanup_failed)
            LOG_ERROR("Switch page-table per-game cleanup was incomplete; the next in-process launch will be refused");
    }
#endif
    state.memory.reset();
    state.alloc_table.reset();
    state.allocator.reset();
    state.page_name_map.clear();
    state.page_table.reset();
    state.external_mapping.clear();
    state.use_page_table = false;
    state.host_page_size = 0;
}

#ifdef _WIN32

static LONG WINAPI exception_handler(PEXCEPTION_POINTERS pExp) noexcept {
    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto ptr = reinterpret_cast<uint8_t *>(pExp->ExceptionRecord->ExceptionInformation[1]);
    const bool is_writing = pExp->ExceptionRecord->ExceptionInformation[0] == 1;
    const bool is_executing = pExp->ExceptionRecord->ExceptionInformation[0] == 8;

    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !is_executing) {
        if (access_violation_handler(ptr, is_writing)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    if (!AddVectoredExceptionHandler(1, exception_handler)) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
}

#elif defined(__SWITCH__)

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    // Horizon has no POSIX-signal access-violation delivery. Keep the handler
    // (so it can be invoked explicitly) but install nothing — guest memory is
    // committed eagerly at allocation time rather than lazily on fault, and the
    // GPU write-protection sync that relies on faults is disabled.
    access_violation_handler = handler;
}

#else

static void signal_handler(int sig, siginfo_t *info, void *uct) noexcept {
    auto context = static_cast<ucontext_t *>(uct);

#ifdef __aarch64__
#ifdef __APPLE__
    const uint32_t esr = context->uc_mcontext->__es.__esr;
#else
    _aarch64_ctx *ctx = reinterpret_cast<_aarch64_ctx *>(context->uc_mcontext.__reserved);
    // get the ESR register
    while (ctx->magic != ESR_MAGIC) {
        if (ctx->magic == 0)
            [[unlikely]]
            raise(SIGTRAP);
        else
            [[likely]]
            ctx = reinterpret_cast<_aarch64_ctx *>(reinterpret_cast<uint8_t *>(ctx) + ctx->size);
    }

    const uint64_t esr = reinterpret_cast<esr_context *>(ctx)->esr;
#endif
    // https://developer.arm.com/documentation/ddi0595/2021-03/AArch64-Registers/ESR-EL1--Exception-Syndrome-Register--EL1-
    const uint32_t exception_class = static_cast<uint32_t>(esr) >> 26;
    const bool is_executing = (exception_class == 0b100000) || (exception_class == 0b100001);
    const bool is_data_abort = (exception_class == 0b100100) || (exception_class == 0b100101);
    const bool is_writing = is_data_abort && (esr & (1 << 6));
#else
#ifdef __APPLE__
    const uint64_t err = context->uc_mcontext->__es.__err;
#else
    const uint64_t err = context->uc_mcontext.gregs[REG_ERR];
#endif
    const bool is_executing = err & 0x10;
    const bool is_writing = err & 0x2;
#endif

    if (!is_executing) {
        if (access_violation_handler(reinterpret_cast<uint8_t *>(info->si_addr), is_writing)) {
            return;
        }
    }

    LOG_CRITICAL("Unhandled access to 0x{:X}", reinterpret_cast<uintptr_t>(info->si_addr));
    raise(SIGTRAP);
    return;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = signal_handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
#ifdef __APPLE__
    // When accessing memory region which is PROT_NONE on macOS, it is raising SIGBUS not SIGSEGV.
    // So apply same signal handler to SIGBUS
    if (sigaction(SIGBUS, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler to SIGBUS");
    }
#endif
}

#endif
