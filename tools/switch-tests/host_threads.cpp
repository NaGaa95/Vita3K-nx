// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <kernel/thread/host_threads.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int SDLCALL finish(void *data) {
    if (data)
        static_cast<std::atomic<unsigned> *>(data)->fetch_add(1);
    return 0;
}

static int SDLCALL wait_for_release(void *data) {
    auto &released = *static_cast<std::atomic<bool> *>(data);
    while (!released.load())
        SDL_Delay(1);
    return 0;
}

int main() {
    assert(SDL_Init(0));
    HostThreads threads;
    std::atomic<bool> released{ false };
    auto *live = SDL_CreateThread(wait_for_release, "live", &released);
    assert(live);
    threads.add(live);
    assert(threads.reap() == 0);
    released.store(true);
    assert(threads.reap(true) == 1);
    assert(threads.reap(true) == 0);

    std::atomic<unsigned> completed{ 0 };
    size_t joined = 0;
    constexpr unsigned count = 2048;
    for (unsigned i = 0; i < count; ++i) {
        auto *thread = SDL_CreateThread(finish, "repeat", &completed);
        assert(thread);
        threads.add(thread);
        joined += threads.reap();
    }
    joined += threads.reap(true);
    assert(joined == count && completed.load() == count);

    std::atomic<size_t> concurrent_joins{ 0 };
    std::vector<std::thread> creators;
    for (unsigned c = 0; c < 4; ++c) {
        creators.emplace_back([&] {
            for (unsigned i = 0; i < 128; ++i) {
                auto *thread = SDL_CreateThread(finish, "parallel", &completed);
                assert(thread);
                threads.add(thread);
                concurrent_joins.fetch_add(threads.reap());
            }
        });
    }
    for (auto &creator : creators)
        creator.join();
    concurrent_joins.fetch_add(threads.reap(true));
    assert(concurrent_joins.load() == 512 && completed.load() == count + 512);

    struct Reentry {
        HostThreads *threads;
        std::atomic<bool> release{ false };
    } reentry{ &threads };
    auto *outer = SDL_CreateThread([](void *data) -> int {
        auto &context = *static_cast<Reentry *>(data);
        while (!context.release.load())
            SDL_Delay(1);
        auto *inner = SDL_CreateThread(finish, "inner", nullptr);
        assert(inner);
        context.threads->add(inner);
        return 0;
    }, "reentry", &reentry);
    assert(outer);
    threads.add(outer);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        reentry.release.store(true);
    });
    const auto first_join = threads.reap(true);
    release.join();
    assert(first_join + threads.reap(true) == 2);
    assert(threads.reap(true) == 0);
    SDL_Quit();
    std::puts("Host thread lifecycle: live threads, repeated creation, concurrent reaping and shutdown passed");
}
