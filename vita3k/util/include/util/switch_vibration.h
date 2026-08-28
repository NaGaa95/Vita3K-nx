// Vita3K emulator project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <cstdint>

// Native Horizon vibration bridge for the controller exposed as Vita port 1.
// The SDL Switch backend currently has no gamepad device, so SceCtrl cannot use
// SDL_RumbleGamepad there.
bool switch_set_vibration(uint8_t small_motor, uint8_t large_motor);
void switch_stop_vibration();
