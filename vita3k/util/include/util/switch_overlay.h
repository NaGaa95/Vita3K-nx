#pragma once

#include <cstdint>

// SaltyNX's FPS overlay (NX-FPS). The Switch 2 has no SaltyNX: its build
// keeps none of it, and never looks up SaltyNX's ports.
namespace switch_overlay {
#if defined(__SWITCH__) && !defined(__SWITCH2__)
void start();
void stop();
void frame(uint32_t width, uint32_t height, uint8_t api);
#else
inline void start() {}
inline void stop() {}
inline void frame(uint32_t, uint32_t, uint8_t) {}
#endif
}
