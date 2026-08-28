// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <util/switch_storage.h>

#ifdef __SWITCH__
#include <switch.h>

void switch_commit_storage() {
    fsdevCommitDevice("sdmc");
}
#endif
