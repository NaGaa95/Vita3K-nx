#include <ngs/modules/filter.h>
#include <ngs/modules/equalizer.h>
#include <ngs/modules/distortion.h>
#include <ngs/modules/envelope.h>
#include <renderer/vertex_stream.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

struct KernelState {};

namespace ngs {
System::System(Ptr<void> storage, uint32_t size) : MempoolObject(storage, size) {}
Rack::Rack(System *owner, Ptr<void> storage, uint32_t size) : MempoolObject(storage, size), system(owner) {}
ModuleData::ModuleData() : parent(nullptr), index(0), is_bypassed(false), flags(0) {}
}

#include "ngs_lifecycle.inc"

template<class Params>
static void parameters(ngs::ModuleData &data, const Params &params) {
    data.flags = ngs::ModuleData::PARAMS_LOCK;
    data.last_info.resize(sizeof(params));
    std::memcpy(data.last_info.data(), &params, sizeof(params));
}

int main() {
    using namespace ngs;
    MemState memory{};
    KernelState kernel;
    System system({}, 0);
    system.granularity = 256;
    system.sample_rate = 48000;
    Rack rack(&system, {}, 0);
    VoiceDefinition definition{BussType::BUSS_MASTER, 1};
    rack.vdef = &definition;
    Voice voice{};
    voice.rack = &rack;
    voice.state = VOICE_STATE_ACTIVE;
    ModuleData data;
    data.parent = &voice;
    std::recursive_mutex scheduler_mutex;
    std::mutex voice_mutex;
    std::unique_lock scheduler_lock(scheduler_mutex);
    std::unique_lock voice_lock(voice_mutex);
    std::array<float, 512> samples{};
    voice.products[0].data = reinterpret_cast<uint8_t *>(samples.data());
    auto process = [&](Module &module) { return module.process(kernel, memory, 0, data, scheduler_lock, voice_lock); };

    FilterModule filter;
    SceNgsFilterParams fp{};
    fp.desc.id = SCE_NGS_FILTER_PARAMS_STRUCT_ID;
    fp.params = {SCE_NGS_FILTER_LOWPASS_RESONANT, 2000.0f, 0.7071f, 0.0f};
    parameters(data, fp);
    samples[0] = samples[1] = 1.0f;
    assert(!process(filter));
    auto *filter_state = data.get_logical_state<FilterLogicalState>();
    assert(filter_state->active && filter_state->processed_last_tick);
    const auto c = filter_state->coefficients;
    assert(std::fabs(samples[0] - c.b0) < 1e-6f);
    assert(std::fabs(samples[2] - (c.b1 - c.a1 * c.b0)) < 1e-6f);
    assert(samples[0] == samples[1]);
    samples.fill(0.25f);
    data.is_bypassed = true;
    assert(!process(filter) && samples[0] == 0.25f);
    data.is_bypassed = false;
    SceNgsParamCoEff invalid{1, 0, 0, 0, std::numeric_limits<float>::infinity()};
    BiquadCoefficients coefficients;
    assert(classify_biquad(invalid, coefficients) == BiquadUse::Invalid);
    for (uint32_t mode = SCE_NGS_FILTER_LOWPASS_RESONANT; mode <= SCE_NGS_FILTER_LOWPASS_RESONANT_NORMALIZED; ++mode) {
        fp.params.eFilterMode = SceNgsParamFilterMode(mode);
        assert(design_biquad(fp.params, 48000, coefficients));
    }

    data.logical_state.reset();
    EqualizerModule eq;
    SceNgsParamEqParams ep{};
    ep.desc.id = SCE_NGS_PARAM_EQ_STRUCT_ID;
    ep.filter[0] = fp.params;
    parameters(data, ep);
    samples.fill(0);
    samples[0] = samples[1] = 1;
    assert(!process(eq));
    assert(data.get_logical_state<EqualizerLogicalState>()->active_bands == 1);

    data.logical_state.reset();
    DistortionModule distortion;
    SceNgsDistortionParams dp{};
    dp.desc.id = SCE_NGS_DISTORTION_PARAMS_STRUCT_ID;
    dp.fA = 2; dp.fClip = 0.5f; dp.fWetGain = 1;
    parameters(data, dp);
    samples.fill(0.4f);
    assert(!process(distortion));
    assert(samples[0] == 0.5f && samples[1] == 0.5f);
    const auto capacity = data.scratch_data.capacity();
    assert(!process(distortion) && data.scratch_data.capacity() == capacity);

    data.logical_state.reset();
    EnvelopeModule envelope;
    SceNgsEnvelopeParams env{};
    env.desc.id = SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID;
    env.uNumPoints = 2;
    env.envelopePoints[0] = {10, 1, SCE_NGS_ENVELOPE_LINEAR};
    env.envelopePoints[1] = {0, 0, SCE_NGS_ENVELOPE_LINEAR};
    env.nLoopEnd = SCE_NGS_ENVELOPE_NO_LOOP;
    env.uReleaseMsecs = 10;
    parameters(data, env);
    samples.fill(1);
    assert(!process(envelope));
    assert(std::fabs(data.get_state<SceNgsEnvelopeStates>()->fCurrentHeight - (1 - 256.0f / 480.0f)) < 1e-5f);
    envelope.on_state_change(memory, data, VOICE_STATE_AVAILABLE);
    assert(data.get_logical_state<EnvelopeLogicalState>()->current_point == 0);
    voice.state = VOICE_STATE_FINALIZING;
    samples.fill(1);
    assert(!process(envelope));
    assert(process(envelope));
    voice.state = VOICE_STATE_ACTIVE;
    env.envelopePoints[0].uMsecsToNextPoint = 0;
    env.uLoopStart = 0;
    env.nLoopEnd = 0;
    parameters(data, env);
    envelope.on_param_change(memory, data);
    for (int i = 0; i < 1000; ++i)
        assert(!process(envelope));

    using namespace renderer;
    VertexStreamLayout layout;
    layout.stride = 8; layout.record_size = 8; layout.repack = true;
    layout.copies = {{1, 0, 4}, {5, 4, 2}};
    std::array<uint8_t, 12> source{};
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i + 1);
    std::array<uint8_t, 24> packed;
    packed.fill(0xcc);
    repack_vertex_stream(layout, 6, source.data(), source.size(), packed.data() + 4, 2);
    assert(packed[4] == 2 && packed[8] == 6 && packed[10] == 0 && packed[12] == 8);
    assert(packed[16] == 12 && packed[17] == 0);
    assert(packed[3] == 0xcc && packed[20] == 0xcc);
    layout.stride = 0;
    repack_vertex_stream(layout, 0, source.data(), 3, packed.data(), 1);
    assert(packed[0] == 2 && packed[1] == 3 && packed[2] == 0 && packed[4] == 0);
    std::puts("NGS DSP: bypass, coefficients, impulse response, EQ, distortion, release and zero-duration loops passed");
    std::puts("Vertex packing: aligned fields, truncated tails, zero stride and guard bytes passed");
}
