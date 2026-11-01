// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <kernel/thread/host_threads.h>

#include <SDL3/SDL_thread.h>

void HostThreads::add(SDL_Thread *thread) {
    const std::lock_guard<std::mutex> lock(mutex);
    threads.push_back(thread);
}

size_t HostThreads::reap(bool wait_all) {
    std::vector<SDL_Thread *> finished;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        std::erase_if(threads, [&](SDL_Thread *thread) {
            if (!wait_all && SDL_GetThreadState(thread) != SDL_THREAD_COMPLETE)
                return false;
            finished.push_back(thread);
            return true;
        });
    }
    for (auto *thread : finished)
        SDL_WaitThread(thread, nullptr);
    return finished.size();
}
