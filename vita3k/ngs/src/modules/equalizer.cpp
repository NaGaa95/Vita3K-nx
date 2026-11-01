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

#include <ngs/modules/equalizer.h>
#include <util/log.h>

#include <cstring>

namespace ngs {

void EqualizerModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    if (data.parent->state == VOICE_STATE_ACTIVE && previous == VOICE_STATE_AVAILABLE) {
        EqualizerLogicalState *logical = data.get_logical_state<EqualizerLogicalState>();
        logical->history = {};
        logical->processed_last_tick = false;
    }
}

bool EqualizerModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    share_main_signal(data);

    EqualizerLogicalState *logical = data.get_logical_state<EqualizerLogicalState>();
    const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
    const bool coefficient_form = desc && desc->id == SCE_NGS_PARAM_EQ_COEFF_STRUCT_ID;
    if (data.is_bypassed || !desc || (!coefficient_form && desc->id != SCE_NGS_PARAM_EQ_STRUCT_ID)) {
        logical->processed_last_tick = false;
        return false;
    }

    const size_t size = coefficient_form ? sizeof(SceNgsParamEqParamsCoEff) : sizeof(SceNgsParamEqParams);
    if (!logical->have_params || std::memcmp(logical->last_params.data(), desc, size) != 0) {
        logical->last_params.fill(0);
        std::memcpy(logical->last_params.data(), desc, size);
        logical->have_params = true;
        logical->active_bands = 0;
        bool silences = false;
        for (uint32_t band = 0; band < SCE_NGS_MAX_EQ_FILTERS; band++) {
            if (coefficient_form) {
                const SceNgsParamCoEff &raw = reinterpret_cast<const SceNgsParamEqParamsCoEff *>(desc)->filterCoEff[band];
                const BiquadUse use = classify_biquad(raw, logical->coefficients[band]);
                if (use == BiquadUse::Apply)
                    logical->active_bands |= 1u << band;
                silences = silences || use == BiquadUse::Silence;
            } else {
                const SceNgsParamFilter &filter = reinterpret_cast<const SceNgsParamEqParams *>(desc)->filter[band];
                if (design_biquad(filter, data.parent->rack->system->sample_rate, logical->coefficients[band]))
                    logical->active_bands |= 1u << band;
            }
        }
        if (silences)
            logical->active_bands = 0;
    }
    if (!logical->active_bands) {
        logical->processed_last_tick = false;
        return false;
    }

    const int send = send_output_of(data);
    float *input = send >= 0 ? reinterpret_cast<float *>(data.parent->products[send].data) : main_signal_of(data);
    const int32_t granularity = data.parent->rack->system->granularity;
    if (!input || granularity <= 0)
        return false;

    const size_t bytes = static_cast<size_t>(granularity) * 2 * sizeof(float);
    data.ensure_scratch_size(bytes);
    float *output = reinterpret_cast<float *>(data.scratch_data.data());
    std::memcpy(output, input, bytes);
    if (!logical->processed_last_tick)
        logical->history = {};
    for (uint32_t band = 0; band < SCE_NGS_MAX_EQ_FILTERS; band++) {
        if (!(logical->active_bands & (1u << band)))
            continue;
        if (!run_biquad(logical->coefficients[band], logical->history[band], output, output, granularity)) {
            logical->history = {};
            logical->processed_last_tick = false;
            return false;
        }
    }
    logical->processed_last_tick = true;
    if (send >= 0)
        data.parent->products[send].data = reinterpret_cast<uint8_t *>(output);
    else
        std::memcpy(input, output, bytes);
    return false;
}
} // namespace ngs
