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

#include <shader/usse_translator.h>

#include <SPIRV/SpvBuilder.h>

#include <shader/usse_types.h>
#include <util/log.h>

#include <bitset>

using namespace shader;
using namespace usse;

bool USSETranslatorVisitor::is_declared_output_type(const DataType type) const {
    if (!m_program.is_fragment())
        return false;
    static constexpr DataType types[] = { DataType::F32, DataType::F16, DataType::C10,
        DataType::UINT32, DataType::INT32, DataType::UINT16, DataType::INT16, DataType::UINT8, DataType::INT8 };
    const auto declared = m_program.get_fragment_output_type();
    return declared < std::size(types) && types[declared] == type;
}

spv::Id USSETranslatorVisitor::load(Operand op, const Imm4 dest_mask, const int shift_offset) {
    if (!m_second_program && op.bank == RegisterBank::OUTPUT && !m_output_accessed) {
        const int type_size = get_data_type_size(op.type);
        for (int i = 0; i < 4; ++i) {
            const auto channel = static_cast<unsigned>(op.swizzle[i]);
            if ((dest_mask & (1 << i)) && channel < 4 && op.num + shift_offset + (channel * type_size) / 4 == 0) {
                m_output_read_declared = !m_raw_move && (op.type == DataType::F16 || op.type == DataType::F32 || is_declared_output_type(op.type));
                m_output_accessed = true;
                break;
            }
        }
    }
    return utils::load(m_b, m_spirv_params, m_util_funcs, m_features, op, dest_mask, shift_offset);
}

void USSETranslatorVisitor::store(Operand dest, spv::Id source, std::uint8_t dest_mask, int shift_offset) {
    if (dest.bank != RegisterBank::FPINTERNAL) {
        const int type_size = get_data_type_size(dest.type);
        for (int i = 0; i < 4; ++i) {
            if (!(dest_mask & (1 << i)))
                continue;
            const uint32_t word = (dest.num + shift_offset + (i * type_size) / 4) & 0xFFFFFF;
            const uint32_t key = (static_cast<uint32_t>(dest.bank) << 24) | word;
            if (!m_store_from_vpck) {
                m_packed_word_formats.erase(key);
                continue;
            }
            auto &format = m_packed_word_formats[key];
            if (format.type != dest.type)
                format = { dest.type, 0 };
            format.bytes |= static_cast<uint8_t>(((1u << type_size) - 1) << ((i * type_size) % 4));
        }
    }
    if (!m_store_from_vpck) {
        const int type_size = get_data_type_size(dest.type);
        for (int i = 0; i < 4; i++) {
            if (!(dest_mask & (1 << i)))
                continue;
            const uint32_t word = (dest.num + shift_offset + (i * type_size) / 4) & 0xFFFFFF;
            m_vpck_written_bytes.erase((static_cast<uint32_t>(dest.bank) << 24) | word);
        }
    }
    if (dest.bank == RegisterBank::OUTPUT && dest.num + shift_offset == 0 && !m_second_program) {
        const int type_size = get_data_type_size(dest.type);
        const std::uint8_t first_word_mask = (type_size >= 4) ? 0b0001 : ((type_size == 2) ? 0b0011 : 0b1111);
        if (dest_mask & first_word_mask) {
            m_output_accessed = true;
            m_output_written_declared = dest.type == DataType::F16 || dest.type == DataType::F32 || is_declared_output_type(dest.type);
        }
    }
    utils::store(m_b, m_spirv_params, m_util_funcs, m_features, dest, source, dest_mask, shift_offset, m_store_raw_f16_lanes);
}

spv::Id USSETranslatorVisitor::swizzle_to_spv_comp(spv::Id composite, spv::Id type, SwizzleChannel swizzle) {
    switch (swizzle) {
    case SwizzleChannel::C_X:
    case SwizzleChannel::C_Y:
    case SwizzleChannel::C_Z:
    case SwizzleChannel::C_W:
        return m_b.createCompositeExtract(composite, type, static_cast<Imm4>(swizzle));

    // TODO: Implement these with OpCompositeExtract
    case SwizzleChannel::C_0: break;
    case SwizzleChannel::C_1: break;
    case SwizzleChannel::C_2: break;

    case SwizzleChannel::C_H: break;
    default: break;
    }

    LOG_WARN("Swizzle channel {} unsupported", static_cast<Imm4>(swizzle));
    return spv::NoResult;
}

size_t USSETranslatorVisitor::dest_mask_to_comp_count(Imm4 dest_mask) {
    std::bitset<4> bs(dest_mask);
    const auto bit_count = bs.count();
    return bit_count;
}
