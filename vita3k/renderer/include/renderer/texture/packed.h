// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace renderer::texture {

inline float half_to_float(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000) << 16;
    const uint32_t exponent = (value >> 10) & 31;
    const uint32_t mantissa = value & 1023;
    if (exponent == 0) {
        const float magnitude = std::ldexp(float(mantissa), -24);
        return sign ? -magnitude : magnitude;
    }
    const uint32_t bits = sign | ((exponent == 31 ? 255 : exponent + 112) << 23) | (mantissa << 13);
    return std::bit_cast<float>(bits);
}

inline uint32_t half_to_unsigned_f10(uint16_t value) {
    if ((value & 0x7c00) == 0x7c00 && (value & 0x3ff))
        return 0x3e0 | std::max(1U, uint32_t(value & 0x3ff) >> 5);
    if (value & 0x8000)
        return 0;
    // The exponent bias is unchanged; round the five discarded mantissa bits to even.
    return (uint32_t(value) + 15 + ((value >> 5) & 1)) >> 5;
}

inline uint32_t half_to_unorm2(uint16_t value) {
    const float decoded = half_to_float(value);
    if (!(decoded > 0.0f))
        return 0;
    if (decoded >= 1.0f)
        return 3;
    return uint32_t(decoded * 3.0f + 0.5f);
}

enum class PackedColorOrder {
    ABGR,
    ARGB,
    RGBA,
    BGRA
};

inline uint32_t pack_u2f10f10f10(const std::array<uint16_t, 4> &rgba, PackedColorOrder order) {
    const uint32_t r = half_to_unsigned_f10(rgba[0]);
    const uint32_t g = half_to_unsigned_f10(rgba[1]);
    const uint32_t b = half_to_unsigned_f10(rgba[2]);
    const uint32_t a = half_to_unorm2(rgba[3]);
    switch (order) {
    case PackedColorOrder::ABGR:
        return r | (g << 10) | (b << 20) | (a << 30);
    case PackedColorOrder::ARGB:
        return b | (g << 10) | (r << 20) | (a << 30);
    case PackedColorOrder::RGBA:
        return a | (b << 2) | (g << 12) | (r << 22);
    case PackedColorOrder::BGRA:
        return a | (r << 2) | (g << 12) | (b << 22);
    }
    return 0;
}

inline uint32_t pack_rgb9e5(float r, float g, float b) {
    const auto clamp = [](float value) {
        return value > 0.0f ? std::min(value, 65408.0f) : 0.0f;
    };
    r = clamp(r);
    g = clamp(g);
    b = clamp(b);
    const float maximum = std::max({ r, g, b });
    if (maximum == 0.0f)
        return 0;

    const int exponent = int(std::bit_cast<uint32_t>(maximum) >> 23) - 127;
    int shared_exponent = std::max(-16, exponent) + 16;
    float scale = std::ldexp(1.0f, shared_exponent - 24);
    if (uint32_t(maximum / scale + 0.5f) == 512) {
        ++shared_exponent;
        scale *= 2.0f;
    }
    return uint32_t(r / scale + 0.5f)
        | (uint32_t(g / scale + 0.5f) << 9)
        | (uint32_t(b / scale + 0.5f) << 18)
        | (uint32_t(shared_exponent) << 27);
}

inline uint32_t pack_se5m9m9m9(const std::array<uint16_t, 4> &rgba, bool swap_rb) {
    return pack_rgb9e5(half_to_float(rgba[swap_rb ? 2 : 0]), half_to_float(rgba[1]), half_to_float(rgba[swap_rb ? 0 : 2]));
}

} // namespace renderer::texture
