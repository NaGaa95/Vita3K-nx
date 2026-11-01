// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

struct SDL_Thread;

class HostThreads {
public:
    void add(SDL_Thread *thread);
    size_t reap(bool wait_all = false);

private:
    std::mutex mutex;
    std::vector<SDL_Thread *> threads;
};
