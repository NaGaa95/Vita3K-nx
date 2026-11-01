#pragma once

#include <cstdint>

namespace switch_overlay {
#ifdef __SWITCH__
void start();
void stop();
void frame(uint32_t width, uint32_t height, uint8_t api);
#else
inline void start() {}
inline void stop() {}
inline void frame(uint32_t, uint32_t, uint8_t) {}
#endif
}
