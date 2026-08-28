// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

// Do not include <switch.h> here: its BIT macros collide with Vita3K headers.
// The implementation owns the libnx dependency.
#ifdef __SWITCH__
// Flushes the SD card's write cache. Horizon holds fsdev writes in memory until
// the device is committed, so anything written since the last commit is lost if
// the process dies rather than exiting cleanly - which is precisely when a cache
// written to survive the next run is worth having.
void switch_commit_storage();
#else
inline void switch_commit_storage() {}
#endif
