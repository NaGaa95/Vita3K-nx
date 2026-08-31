// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <kernel/pthread_compat.h>

#include <algorithm>
#include <array>
#include <optional>

namespace {
uint16_t read_halfword(std::span<const uint8_t> code, size_t offset) {
    return code[offset] | (code[offset + 1] << 8);
}

std::optional<uint32_t> call_target(std::span<const uint8_t> code, uint32_t base, size_t offset, bool exchange = false) {
    const uint16_t upper = read_halfword(code, offset);
    const uint16_t lower = read_halfword(code, offset + 2);
    if ((upper & 0xF800) != 0xF000
        || (exchange ? (lower & 0xD001) != 0xC000 : (lower & 0xD000) != 0xD000))
        return std::nullopt;

    const uint32_t sign = (upper >> 10) & 1;
    const uint32_t i1 = 1 ^ ((lower >> 13) & 1) ^ sign;
    const uint32_t i2 = 1 ^ ((lower >> 11) & 1) ^ sign;
    const uint32_t displacement = (sign ? 0xFF000000 : 0) | (i1 << 23) | (i2 << 22)
        | ((upper & 0x3FF) << 12) | ((lower & 0x7FF) << 1);
    uint32_t pc = base + static_cast<uint32_t>(offset) + 4;
    if (exchange)
        pc &= ~3u;
    return pc + displacement;
}

bool write_call(std::span<uint16_t> code, size_t offset, uint32_t address, uint32_t target) {
    const int64_t displacement = static_cast<int64_t>(target) - address - 4;
    if ((displacement & 1) || displacement < -0x1000000 || displacement >= 0x1000000)
        return false;
    const uint32_t immediate = static_cast<uint32_t>(displacement);
    const uint32_t sign = (immediate >> 24) & 1;
    const uint32_t j1 = 1 ^ ((immediate >> 23) & 1) ^ sign;
    const uint32_t j2 = 1 ^ ((immediate >> 22) & 1) ^ sign;
    code[offset / 2] = 0xF000 | (sign << 10) | ((immediate >> 12) & 0x3FF);
    code[offset / 2 + 1] = 0xD000 | (j1 << 13) | (j2 << 11) | ((immediate >> 1) & 0x7FF);
    return true;
}
} // namespace

size_t fix_pthread_semaphore_post(std::span<uint8_t> code, uint32_t base, std::span<const uint32_t> signal_imports) {
    // Match the complete legacy Thumb sem_post; only BL destinations vary.
    static constexpr std::array<uint16_t, 58> signature = {
        0xB5F8, 0x6806, 0xB376, 0x1D37, 0x4605, 0x4638, 0x0000, 0x0000,
        0x4604, 0xB950, 0x682B, 0xB33B, 0x6832, 0xF647, 0x73FE, 0x429A,
        0xDD0A, 0x4638, 0x2422, 0x0000, 0x0000, 0x0000, 0x0000, 0x6004,
        0xF04F, 0x34FF, 0x4620, 0xBDF8, 0x68B0, 0x2101, 0x0000, 0x0000,
        0x6832, 0x1C53, 0x2B00, 0x6033, 0xDD04, 0x4638, 0x0000, 0x0000,
        0x4620, 0xBDF8, 0x2800, 0xD0F8, 0x4638, 0x6032, 0x2416, 0x0000,
        0x0000, 0xE7E2, 0x2416, 0xE7E0, 0x4638, 0xF04F, 0x34FF, 0x0000,
        0x0000, 0xE7DF
    };
    static constexpr std::array<size_t, 7> calls = { 0x0C, 0x26, 0x2A, 0x3C, 0x4C, 0x5E, 0x6E };
    if (signal_imports.empty() || (base & 1) || code.size() < sizeof(signature)
        || code.size() > (uint64_t{ 1 } << 32) - base)
        return 0;

    size_t fixed = 0;
    for (size_t at = 0; at <= code.size() - sizeof(signature); at += 2) {
        size_t matched = 0;
        while (matched < signature.size()
            && (!signature[matched] || signature[matched] == read_halfword(code, at + matched * 2)))
            ++matched;
        if (matched != signature.size())
            continue;

        std::array<uint32_t, calls.size()> targets{};
        size_t valid_calls = 0;
        for (size_t i = 0; i < calls.size(); ++i) {
            const auto target = call_target(code, base, at + calls[i]);
            if (!target || *target < base || static_cast<uint64_t>(*target - base) + 2 > code.size())
                break;
            targets[i] = *target;
            ++valid_calls;
        }
        if (valid_calls != calls.size() || targets[1] != targets[4]
            || targets[1] != targets[5] || targets[1] != targets[6] || targets[0] == targets[1])
            continue;

        const size_t post = targets[3] - base;
        if (code.size() - post < 12 || read_halfword(code, post) != 0xB508
            || read_halfword(code, post + 6) != 0x2000 || read_halfword(code, post + 8) != 0xBD08
            || read_halfword(code, post + 10) != 0xBF00)
            continue;
        const auto signal = call_target(code, base, post + 2, true);
        if (!signal || *signal < base || static_cast<uint64_t>(*signal - base) + 12 > code.size()
            || std::find(signal_imports.begin(), signal_imports.end(), *signal) == signal_imports.end())
            continue;

        // Increment the pthread count first; signal the kernel only for a waiter.
        std::array<uint16_t, 22> replacement = {
            0x1C53, 0x6033, 0x2B00, 0xDC05, 0x68B0, 0x2101, 0, 0,
            0x2800, 0xD104, 0x4638, 0, 0, 0x4620, 0xBDF8, 0x6833,
            0x3B01, 0x6033, 0x4638, 0, 0, 0xE7FF
        };
        const uint32_t patch_address = base + static_cast<uint32_t>(at) + 0x38;
        if (!write_call(replacement, 0x0C, patch_address + 0x0C, targets[3])
            || !write_call(replacement, 0x16, patch_address + 0x16, targets[1])
            || !write_call(replacement, 0x26, patch_address + 0x26, targets[1]))
            continue;
        for (size_t i = 0; i < replacement.size(); ++i) {
            code[at + 0x38 + i * 2] = static_cast<uint8_t>(replacement[i]);
            code[at + 0x39 + i * 2] = static_cast<uint8_t>(replacement[i] >> 8);
        }
        ++fixed;
    }
    return fixed;
}
