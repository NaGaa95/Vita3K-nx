// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

// libnx currently has twelve process-wide TLS slots behind pthread_key_create.
// That is too small for Vita3K plus SDL, OpenSSL and Mesa's Rust runtime. More
// importantly, libstdc++'s __cxa_thread_atexit implementation ignores a failed
// pthread_key_create and then uses its zero-initialized key, corrupting libgcc's
// key 0. The corruption is only observed later, when an otherwise healthy host
// thread exits and libgcc interprets unrelated data as an exception object.
//
// The linker wraps the four pthread key functions with this implementation. All
// POSIX keys are multiplexed through one real libnx TLS slot, whose destructor
// dispatches the virtual-key destructors. This also preserves destructor retry
// semantics and makes key deletion/reuse safe through per-key generations.

#include <switch.h>

#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr std::size_t MAX_VIRTUAL_KEYS = 128;
constexpr int DESTRUCTOR_ITERATIONS = 4;
constexpr int NATIVE_SLOT_UNINITIALIZED = -2;
constexpr int NATIVE_SLOT_INITIALIZING = -3;
constexpr int NATIVE_SLOT_FAILED = -4;

using KeyDestructor = void (*)(void *);

struct VirtualKey {
    KeyDestructor destructor = nullptr;
    std::uint32_t generation = 0;
    bool active = false;
};

struct ThreadValues {
    void *values[MAX_VIRTUAL_KEYS]{};
    std::uint32_t generations[MAX_VIRTUAL_KEYS]{};
};

std::atomic<int> native_slot{ NATIVE_SLOT_UNINITIALIZED };
Mutex key_mutex{};
VirtualKey virtual_keys[MAX_VIRTUAL_KEYS]{};

void destroy_thread_values(void *opaque);

int get_native_slot() {
    int slot = native_slot.load(std::memory_order_acquire);
    if (slot >= 0 || slot == NATIVE_SLOT_FAILED)
        return slot;

    int expected = NATIVE_SLOT_UNINITIALIZED;
    if (native_slot.compare_exchange_strong(expected, NATIVE_SLOT_INITIALIZING,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        const int allocated = threadTlsAlloc(destroy_thread_values);
        slot = allocated >= 0 ? allocated : NATIVE_SLOT_FAILED;
        native_slot.store(slot, std::memory_order_release);
        return slot;
    }

    do {
        svcSleepThread(0);
        slot = native_slot.load(std::memory_order_acquire);
    } while (slot == NATIVE_SLOT_INITIALIZING);
    return slot;
}

bool snapshot_key(const pthread_key_t key, std::uint32_t &generation, KeyDestructor &destructor) {
    if (key >= MAX_VIRTUAL_KEYS)
        return false;

    mutexLock(&key_mutex);
    const VirtualKey &record = virtual_keys[key];
    const bool active = record.active;
    generation = record.generation;
    destructor = record.destructor;
    mutexUnlock(&key_mutex);
    return active;
}

void destroy_thread_values(void *opaque) {
    auto *storage = static_cast<ThreadValues *>(opaque);
    if (!storage)
        return;

    const int slot = native_slot.load(std::memory_order_acquire);
    if (slot >= 0)
        threadTlsSet(slot, storage);

    // POSIX permits a destructor to install another value. Repeat the pass so
    // that such values are cleaned up too, bounded by the standard iteration
    // count used by newlib/libstdc++.
    for (int pass = 0; pass < DESTRUCTOR_ITERATIONS; ++pass) {
        bool called_destructor = false;
        for (std::size_t i = 0; i < MAX_VIRTUAL_KEYS; ++i) {
            std::uint32_t generation = 0;
            KeyDestructor destructor = nullptr;
            if (!snapshot_key(static_cast<pthread_key_t>(i), generation, destructor)
                || !destructor || storage->generations[i] != generation || !storage->values[i]) {
                continue;
            }

            void *value = storage->values[i];
            storage->values[i] = nullptr;
            destructor(value);
            called_destructor = true;
        }
        if (!called_destructor)
            break;
    }

    if (slot >= 0)
        threadTlsSet(slot, nullptr);
    std::free(storage);
}

} // namespace

extern "C" int __wrap_pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key)
        return EINVAL;
    if (get_native_slot() < 0)
        return EAGAIN;

    mutexLock(&key_mutex);
    for (std::size_t i = 0; i < MAX_VIRTUAL_KEYS; ++i) {
        VirtualKey &record = virtual_keys[i];
        if (record.active)
            continue;

        ++record.generation;
        if (record.generation == 0)
            ++record.generation;
        record.destructor = destructor;
        record.active = true;
        *key = static_cast<pthread_key_t>(i);
        mutexUnlock(&key_mutex);
        return 0;
    }
    mutexUnlock(&key_mutex);
    return EAGAIN;
}

extern "C" int __wrap_pthread_key_delete(const pthread_key_t key) {
    if (key >= MAX_VIRTUAL_KEYS)
        return EINVAL;

    mutexLock(&key_mutex);
    VirtualKey &record = virtual_keys[key];
    if (!record.active) {
        mutexUnlock(&key_mutex);
        return EINVAL;
    }

    record.active = false;
    record.destructor = nullptr;
    ++record.generation;
    if (record.generation == 0)
        ++record.generation;
    mutexUnlock(&key_mutex);
    return 0;
}

extern "C" void *__wrap_pthread_getspecific(const pthread_key_t key) {
    const int slot = get_native_slot();
    if (slot < 0)
        return nullptr;

    std::uint32_t generation = 0;
    KeyDestructor destructor = nullptr;
    if (!snapshot_key(key, generation, destructor))
        return nullptr;

    auto *storage = static_cast<ThreadValues *>(threadTlsGet(slot));
    if (!storage || storage->generations[key] != generation)
        return nullptr;
    return storage->values[key];
}

extern "C" int __wrap_pthread_setspecific(const pthread_key_t key, const void *value) {
    const int slot = get_native_slot();
    if (slot < 0)
        return EAGAIN;

    std::uint32_t generation = 0;
    KeyDestructor destructor = nullptr;
    if (!snapshot_key(key, generation, destructor))
        return EINVAL;

    auto *storage = static_cast<ThreadValues *>(threadTlsGet(slot));
    if (!storage) {
        if (!value)
            return 0;
        storage = static_cast<ThreadValues *>(std::calloc(1, sizeof(ThreadValues)));
        if (!storage)
            return ENOMEM;
        threadTlsSet(slot, storage);
    }

    storage->generations[key] = generation;
    storage->values[key] = const_cast<void *>(value);
    return 0;
}
