// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#include <renderer/texture/packed.h>

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace renderer::texture;

static void check(bool condition) {
    if (!condition) {
        std::cerr << "Packed format check failed\n";
        std::exit(1);
    }
}

static uint32_t reference_rgb9e5(double r, double g, double b) {
    const auto clamp = [](double v) { return v > 0 ? std::min(v, 65408.0) : 0.0; };
    r = clamp(r);
    g = clamp(g);
    b = clamp(b);
    const double maximum = std::max({ r, g, b });
    unsigned exponent = 0;
    while (exponent < 31 && std::floor(std::ldexp(maximum, 24 - int(exponent)) + 0.5) >= 512)
        ++exponent;
    return uint32_t(std::floor(std::ldexp(r, 24 - int(exponent)) + 0.5))
        | (uint32_t(std::floor(std::ldexp(g, 24 - int(exponent)) + 0.5)) << 9)
        | (uint32_t(std::floor(std::ldexp(b, 24 - int(exponent)) + 0.5)) << 18)
        | (exponent << 27);
}

int main() {
    for (uint32_t f10 = 0; f10 < 1024; ++f10)
        check(half_to_unsigned_f10(uint16_t(f10 << 5)) == f10);
    check(half_to_unsigned_f10(0x10) == 0);
    check(half_to_unsigned_f10(0x30) == 2);
    check(half_to_unsigned_f10(0x7c01) > 0x3e0);
    check(half_to_unsigned_f10(0xfc00) == 0);
    check(half_to_unsigned_f10(0x7bff) == 0x3e0);

    for (uint32_t bits = 0; bits < 65536; ++bits) {
        const unsigned exponent = (bits >> 10) & 31;
        const unsigned mantissa = bits & 1023;
        double value = exponent ? std::ldexp(double(1024 + mantissa), int(exponent) - 25) : std::ldexp(double(mantissa), -24);
        if (bits & 0x8000)
            value = -value;
        if (exponent == 31)
            value = mantissa ? std::numeric_limits<double>::quiet_NaN() : ((bits & 0x8000) ? -INFINITY : INFINITY);
        const uint32_t alpha = !(value > 0) ? 0 : value >= 1 ? 3 : uint32_t(std::floor(value * 3 + 0.5));
        check(half_to_unorm2(uint16_t(bits)) == alpha);
        const float decoded = half_to_float(uint16_t(bits));
        check(pack_rgb9e5(decoded, decoded * 0.5f, 1.0f) == reference_rgb9e5(decoded, decoded * 0.5f, 1.0f));
    }

    const std::array<uint16_t, 4> rgba{ 0x3c00, 0x4000, 0x3800, 0x3c00 };
    check(pack_u2f10f10f10(rgba, PackedColorOrder::ABGR) == (0x1e0U | (0x200U << 10) | (0x1c0U << 20) | (3U << 30)));
    check(pack_u2f10f10f10(rgba, PackedColorOrder::ARGB) == (0x1c0U | (0x200U << 10) | (0x1e0U << 20) | (3U << 30)));
    check(pack_u2f10f10f10(rgba, PackedColorOrder::RGBA) == (3U | (0x1c0U << 2) | (0x200U << 12) | (0x1e0U << 22)));
    check(pack_u2f10f10f10(rgba, PackedColorOrder::BGRA) == (3U | (0x1e0U << 2) | (0x200U << 12) | (0x1c0U << 22)));
    check(pack_se5m9m9m9(rgba, false) == pack_rgb9e5(1, 2, 0.5f));
    check(pack_se5m9m9m9(rgba, true) == pack_rgb9e5(0.5f, 2, 1));
    check(pack_rgb9e5(65408, 65408, 65408) == 0xffffffff);
    check(pack_rgb9e5(0, 0, 0) == 0);
    std::cout << "Packed format checks passed\n";
}
