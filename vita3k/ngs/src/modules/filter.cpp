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

#include <ngs/modules/filter.h>
#include <util/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ngs {

namespace {
constexpr float pi = 3.14159265358979323846f;

float sanitised(const float value, const float lo, const float hi, const float fallback) {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}
} // namespace

bool design_biquad(const SceNgsParamFilter &band, const int32_t sample_rate, BiquadCoefficients &coefficients) {
    if (sample_rate <= 0 || band.eFilterMode == SCE_NGS_FILTER_MODE_OFF)
        return false;

    const float fs = static_cast<float>(sample_rate);
    const float f0 = sanitised(band.fFrequency, 10.0f, 0.49f * fs, 1000.0f);
    const float q = sanitised(band.fResonance, 0.05f, 40.0f, 0.7071f);
    const float gain_db = sanitised(band.fGain, -48.0f, 48.0f, 0.0f);
    const float w0 = 2.0f * pi * f0 / fs;
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.0f * q);
    const float a = std::pow(10.0f, gain_db / 40.0f);
    const float shelf = 2.0f * std::sqrt(a) * (sw * 0.5f * std::sqrt(2.0f));

    float b0, b1, b2, a0, a1, a2;
    switch (band.eFilterMode) {
    case SCE_NGS_FILTER_LOWPASS_RESONANT:
    case SCE_NGS_FILTER_LOWPASS_RESONANT_NORMALIZED:
        b0 = (1.0f - cw) * 0.5f;
        b1 = 1.0f - cw;
        b2 = b0;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_HIGHPASS_RESONANT:
        b0 = (1.0f + cw) * 0.5f;
        b1 = -(1.0f + cw);
        b2 = b0;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_BANDPASS_PEAK: {
        const float peak = std::pow(10.0f, gain_db / 20.0f);
        b0 = peak * alpha;
        b1 = 0.0f;
        b2 = -peak * alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    }
    case SCE_NGS_FILTER_BANDPASS_ZERO:
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_NOTCH:
        b0 = 1.0f;
        b1 = -2.0f * cw;
        b2 = 1.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_PEAK:
        b0 = 1.0f + alpha * a;
        b1 = -2.0f * cw;
        b2 = 1.0f - alpha * a;
        a0 = 1.0f + alpha / a;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha / a;
        break;
    case SCE_NGS_FILTER_HIGHSHELF:
        b0 = a * ((a + 1.0f) + (a - 1.0f) * cw + shelf);
        b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw);
        b2 = a * ((a + 1.0f) + (a - 1.0f) * cw - shelf);
        a0 = (a + 1.0f) - (a - 1.0f) * cw + shelf;
        a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw);
        a2 = (a + 1.0f) - (a - 1.0f) * cw - shelf;
        break;
    case SCE_NGS_FILTER_LOWSHELF:
        b0 = a * ((a + 1.0f) - (a - 1.0f) * cw + shelf);
        b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw);
        b2 = a * ((a + 1.0f) - (a - 1.0f) * cw - shelf);
        a0 = (a + 1.0f) + (a - 1.0f) * cw + shelf;
        a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw);
        a2 = (a + 1.0f) + (a - 1.0f) * cw - shelf;
        break;
    case SCE_NGS_FILTER_LOWPASS_ONEPOLE: {
        const float pole = std::exp(-w0);
        b0 = 1.0f - pole;
        b1 = 0.0f;
        b2 = 0.0f;
        a0 = 1.0f;
        a1 = -pole;
        a2 = 0.0f;
        break;
    }
    case SCE_NGS_FILTER_HIGHPASS_ONEPOLE: {
        const float pole = std::exp(-w0);
        b0 = (1.0f + pole) * 0.5f;
        b1 = -(1.0f + pole) * 0.5f;
        b2 = 0.0f;
        a0 = 1.0f;
        a1 = -pole;
        a2 = 0.0f;
        break;
    }
    case SCE_NGS_FILTER_ALLPASS:
        b0 = 1.0f - alpha;
        b1 = -2.0f * cw;
        b2 = 1.0f + alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha;
        break;
    default:
        return false;
    }

    // the normalized resonant low pass never rises above 0 dB: scale the resonant peak back to unity
    float scale = 1.0f;
    if (band.eFilterMode == SCE_NGS_FILTER_LOWPASS_RESONANT_NORMALIZED && q > 0.7071f)
        scale = std::sqrt(1.0f - 1.0f / (4.0f * q * q)) / q;

    coefficients = { scale * b0 / a0, scale * b1 / a0, scale * b2 / a0, a1 / a0, a2 / a0 };
    return std::isfinite(coefficients.b0) && std::isfinite(coefficients.b1) && std::isfinite(coefficients.b2)
        && std::isfinite(coefficients.a1) && std::isfinite(coefficients.a2);
}

BiquadUse classify_biquad(const SceNgsParamCoEff &raw, BiquadCoefficients &coefficients) {
    const float values[] = { raw.fB0, raw.fB1, raw.fB2, raw.fA1, raw.fA2 };
    for (const float value : values) {
        if (!std::isfinite(value))
            return BiquadUse::Invalid;
    }
    // both poles inside the unit circle
    if (std::fabs(raw.fA2) >= 1.0f || std::fabs(raw.fA1) >= 1.0f + raw.fA2)
        return BiquadUse::Invalid;
    if (raw.fB0 == 0.0f && raw.fB1 == 0.0f && raw.fB2 == 0.0f)
        return BiquadUse::Silence;
    if (raw.fB0 == 1.0f && raw.fB1 == 0.0f && raw.fB2 == 0.0f && raw.fA1 == 0.0f && raw.fA2 == 0.0f)
        return BiquadUse::Identity;
    coefficients = { raw.fB0, raw.fB1, raw.fB2, raw.fA1, raw.fA2 };
    return BiquadUse::Apply;
}

bool run_biquad(const BiquadCoefficients &c, StereoBiquadHistory &history, const float *input, float *output, const int32_t frames) {
    for (size_t channel = 0; channel < history.size(); channel++) {
        BiquadHistory h = history[channel];
        for (int32_t frame = 0; frame < frames; frame++) {
            const float x = input[frame * 2 + channel];
            const float y = c.b0 * x + c.b1 * h.x1 + c.b2 * h.x2 - c.a1 * h.y1 - c.a2 * h.y2;
            h.x2 = h.x1;
            h.x1 = x;
            h.y2 = h.y1;
            h.y1 = y;
            output[frame * 2 + channel] = y;
        }
        if (!std::isfinite(h.y1) || !std::isfinite(h.y2))
            return false;
        if (std::fabs(h.y1) < 1.0e-20f && std::fabs(h.y2) < 1.0e-20f) {
            h.y1 = 0.0f;
            h.y2 = 0.0f;
        }
        history[channel] = h;
    }
    return true;
}

int send_output_of(const ModuleData &data) {
    const VoiceDefinition *definition = (data.parent && data.parent->rack) ? data.parent->rack->vdef : nullptr;
    if (!definition)
        return -1;
    const uint32_t index = data.index;
    switch (definition->type) {
    case BussType::BUSS_SIMPLE:
    case BussType::BUSS_SIMPLE_ATRAC9:
        return (index == 4 || index == 5) ? static_cast<int>(index - 4) : -1;
    case BussType::BUSS_SCREAM:
    case BussType::BUSS_SCREAM_ATRAC9:
        return (index == 4) ? 0 : -1;
    case BussType::BUSS_ATRAC9:
    case BussType::BUSS_NORMAL_PLAYER:
        return (index >= 6 && index <= 9) ? static_cast<int>(index - 6) : -1;
    default:
        return -1;
    }
}

float *main_signal_of(ModuleData &data) {
    Voice *voice = data.parent;
    if (voice->products[0].data)
        return reinterpret_cast<float *>(voice->products[0].data);
    if (!voice->inputs.inputs.empty())
        return reinterpret_cast<float *>(voice->inputs.inputs[0].data());
    return nullptr;
}

void share_main_signal(ModuleData &data) {
    Voice *voice = data.parent;
    const uint32_t outputs = std::min<uint32_t>(voice->rack->vdef ? voice->rack->vdef->output_count : 1, MAX_VOICE_OUTPUT);
    for (uint32_t output = 1; output < outputs; output++) {
        if (!voice->products[output].data)
            voice->products[output] = voice->products[0];
    }
}

void FilterModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    if (data.parent->state == VOICE_STATE_ACTIVE && previous == VOICE_STATE_AVAILABLE) {
        FilterLogicalState *logical = data.get_logical_state<FilterLogicalState>();
        logical->history = {};
        logical->processed_last_tick = false;
    }
}

bool FilterModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    share_main_signal(data);

    FilterLogicalState *logical = data.get_logical_state<FilterLogicalState>();
    const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
    const bool coefficient_form = desc && desc->id == SCE_NGS_FILTER_PARAMS_COEFF_STRUCT_ID;
    if (data.is_bypassed || !desc || (!coefficient_form && desc->id != SCE_NGS_FILTER_PARAMS_STRUCT_ID)) {
        logical->processed_last_tick = false;
        return false;
    }

    const size_t size = coefficient_form ? sizeof(SceNgsFilterParamsCoEff) : sizeof(SceNgsFilterParams);
    if (!logical->have_params || std::memcmp(logical->last_params.data(), desc, size) != 0) {
        logical->last_params.fill(0);
        std::memcpy(logical->last_params.data(), desc, size);
        logical->have_params = true;
        if (coefficient_form) {
            const SceNgsParamCoEff &raw = reinterpret_cast<const SceNgsFilterParamsCoEff *>(desc)->params;
            logical->active = classify_biquad(raw, logical->coefficients) == BiquadUse::Apply;
        } else {
            const SceNgsParamFilter &band = reinterpret_cast<const SceNgsFilterParams *>(desc)->params;
            logical->active = design_biquad(band, data.parent->rack->system->sample_rate, logical->coefficients);
        }
    }
    if (!logical->active) {
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
    if (!logical->processed_last_tick)
        logical->history = {};
    if (!run_biquad(logical->coefficients, logical->history, input, output, granularity)) {
        logical->history = {};
        logical->processed_last_tick = false;
        return false;
    }
    logical->processed_last_tick = true;
    if (send >= 0)
        data.parent->products[send].data = reinterpret_cast<uint8_t *>(output);
    else
        std::memcpy(input, output, bytes);
    return false;
}
} // namespace ngs
