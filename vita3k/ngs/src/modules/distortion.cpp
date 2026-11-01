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

#include <ngs/modules/distortion.h>
#include <ngs/modules/filter.h>
#include <util/log.h>

#include <cmath>
#include <cstring>

namespace ngs {

namespace {
float distort(const SceNgsDistortionParams &p, const float x) {
    const float shaped = p.fA * x - p.fB * x * std::fabs(x);
    const float gated = std::fabs(shaped) < p.fGate ? 0.0f : shaped;
    const float limited = std::fabs(gated) > p.fClip ? std::copysign(p.fClip, gated) : gated;
    return p.fWetGain * limited + p.fDryGain * x;
}

bool distortion_applies(const SceNgsDistortionParams &p) {
    const float values[] = { p.fA, p.fB, p.fClip, p.fGate, p.fWetGain, p.fDryGain };
    for (const float value : values) {
        if (!std::isfinite(value))
            return false;
    }
    if (p.fA < 0.0f || p.fA > 10.0f || p.fB < 0.0f || p.fB > 10.0f || p.fClip < 0.0f || p.fClip > 1.0f
        || p.fGate < 0.0f || p.fGate > 1.0f || p.fWetGain < 0.0f || p.fWetGain > 16.0f || p.fDryGain < 0.0f || p.fDryGain > 16.0f)
        return false;
    bool silent = true;
    bool identity = true;
    for (const float x : { -0.9f, -0.5f, -0.1f, 0.1f, 0.5f, 0.9f }) {
        const float y = distort(p, x);
        silent = silent && std::fabs(y) < 1.0e-6f;
        identity = identity && std::fabs(y - x) < 1.0e-6f;
    }
    return !silent && !identity;
}
} // namespace

bool DistortionModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    const SceNgsDistortionParams *params = data.get_parameters<SceNgsDistortionParams>(mem);
    if (data.is_bypassed || !params || params->desc.id != SCE_NGS_DISTORTION_PARAMS_STRUCT_ID)
        return false;

    DistortionLogicalState *logical = data.get_logical_state<DistortionLogicalState>();
    if (!logical->have_params || std::memcmp(&logical->last_params, params, sizeof(SceNgsDistortionParams)) != 0) {
        std::memcpy(&logical->last_params, params, sizeof(SceNgsDistortionParams));
        logical->have_params = true;
        logical->active = distortion_applies(*params);
    }
    if (!logical->active)
        return false;

    float *signal = main_signal_of(data);
    const int32_t granularity = data.parent->rack->system->granularity;
    if (!signal || granularity <= 0)
        return false;
    for (int32_t i = 0; i < granularity * 2; i++)
        signal[i] = distort(logical->last_params, signal[i]);
    return false;
}
} // namespace ngs
