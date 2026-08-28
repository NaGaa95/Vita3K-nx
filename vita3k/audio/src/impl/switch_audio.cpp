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

#include "audio/impl/switch_audio.h"
#include "util/log.h"
#include "util/switch_thread.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <malloc.h>

namespace {
// Renderer voice budget. Vita titles rarely open more than a handful of output
// ports simultaneously; 24 is generous while keeping the renderer light.
constexpr int SWITCH_NUM_VOICES = 24;
// Wavebuf slots per port. ~8 buffers of the port granularity give enough queue
// depth to avoid underruns without adding much latency.
constexpr int SWITCH_NUM_SLOTS = 8;

constexpr AudioRendererConfig AR_CONFIG = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = SWITCH_NUM_VOICES,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}
} // namespace

SwitchAudioAdapter::SwitchAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

SwitchAudioAdapter::~SwitchAudioAdapter() {
    running = false;
    wake_all_ports();
    if (update_thread.joinable())
        update_thread.join();

    if (initialized) {
        // Give still-detaching pools a few renderer frames to complete.
        for (int attempt = 0; attempt < 8; attempt++) {
            {
                std::lock_guard<std::mutex> lock(driver_mutex);
                if (pending_pool_releases.empty())
                    break;
                audrvUpdate(&driver);
                reap_detached_pools();
            }
            audrenWaitFrame();
        }

        std::lock_guard<std::mutex> lock(driver_mutex);
        audrenStopAudioRenderer();
        audrvClose(&driver);
        audrenExit();
        // The service session is gone; whatever never detached can no longer be
        // read by anyone, so the buffers are safe to free now.
        for (const PendingPoolRelease &pending : pending_pool_releases)
            free(pending.pool);
        pending_pool_releases.clear();
        initialized = false;
    }
}

bool SwitchAudioAdapter::init() {
    Result rc = audrenInitialize(&AR_CONFIG);
    if (R_FAILED(rc)) {
        LOG_ERROR("audrenInitialize failed: 0x{:08x}", rc);
        return false;
    }

    rc = audrvCreate(&driver, &AR_CONFIG, 2);
    if (R_FAILED(rc)) {
        LOG_ERROR("audrvCreate failed: 0x{:08x}", rc);
        audrenExit();
        return false;
    }

    static const u8 sink_channels[] = { 0, 1 };
    const int sink = audrvDeviceSinkAdd(&driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels);
    if (sink < 0) {
        LOG_ERROR("audrvDeviceSinkAdd failed: {}", sink);
        audrvClose(&driver);
        audrenExit();
        return false;
    }

    audrvUpdate(&driver);

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        LOG_ERROR("audrenStartAudioRenderer failed: 0x{:08x}", rc);
        audrvClose(&driver);
        audrenExit();
        return false;
    }

    voice_used.assign(SWITCH_NUM_VOICES, false);
    initialized = true;
    running = true;
    update_thread = std::thread(&SwitchAudioAdapter::update_thread_func, this);
    LOG_INFO("Switch audren audio backend initialised ({} voices)", SWITCH_NUM_VOICES);
    return true;
}

void SwitchAudioAdapter::update_thread_func() {
    // Same reasoning as the vblank tick: this thread spends its life blocked in
    // audrenWaitFrame and only does a short update per audio frame, but late
    // updates are heard as dropouts. Keep it off the cores the emulator saturates.
    switch_pin_to_helper_core("audio thread", 43);

    while (running.load()) {
        audrenWaitFrame(); // blocks ~5 ms until the next renderer frame
        if (!running.load())
            break;
        {
            std::lock_guard<std::mutex> lock(driver_mutex);
            audrvUpdate(&driver);
            reap_detached_pools();
        }
        // A wavebuf may have completed; wake any port waiting for a free slot.
        wait_cond.notify_all();
    }
}

int SwitchAudioAdapter::alloc_voice() {
    std::lock_guard<std::mutex> lock(voice_mutex);
    for (int i = 0; i < static_cast<int>(voice_used.size()); i++) {
        if (!voice_used[i]) {
            voice_used[i] = true;
            return i;
        }
    }
    return -1;
}

void SwitchAudioAdapter::free_voice(int voice_id) {
    if (voice_id < 0)
        return;
    std::lock_guard<std::mutex> lock(voice_mutex);
    if (voice_id < static_cast<int>(voice_used.size()))
        voice_used[voice_id] = false;
}

AudioOutPortPtr SwitchAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    if (!initialized)
        return nullptr;

    const int voice = alloc_voice();
    if (voice < 0) {
        LOG_ERROR("No free audren voice available for a new audio port");
        return nullptr;
    }

    auto port = std::make_shared<SwitchAudioOutPort>(*this);
    port->voice_id = voice;
    port->channels = nb_channels;
    port->freq = freq;
    port->len_bytes = nb_sample * nb_channels * static_cast<int>(sizeof(int16_t));
    port->len_microseconds = static_cast<uint64_t>(nb_sample) * 1'000'000ULL / freq;

    port->num_slots = SWITCH_NUM_SLOTS;
    port->slot_bytes = align_up(port->len_bytes, AUDREN_BUFFER_ALIGNMENT);
    port->pool_size = align_up(port->slot_bytes * port->num_slots, AUDREN_MEMPOOL_ALIGNMENT);
    port->pool = memalign(AUDREN_MEMPOOL_ALIGNMENT, port->pool_size);
    if (!port->pool) {
        LOG_ERROR("Failed to allocate {} byte audren mempool for audio port", port->pool_size);
        free_voice(voice);
        return nullptr;
    }
    memset(port->pool, 0, port->pool_size);

    port->wavebufs.assign(port->num_slots, AudioDriverWaveBuf{});
    for (int i = 0; i < port->num_slots; i++) {
        port->wavebufs[i].data_pcm16 = reinterpret_cast<s16 *>(static_cast<u8 *>(port->pool) + i * port->slot_bytes);
        port->wavebufs[i].state = AudioDriverWaveBufState_Free;
    }

    {
        std::lock_guard<std::mutex> lock(driver_mutex);
        port->mempool_id = audrvMemPoolAdd(&driver, port->pool, port->pool_size);
        if (port->mempool_id < 0 || !audrvMemPoolAttach(&driver, port->mempool_id)) {
            LOG_ERROR("audrvMemPoolAdd/Attach failed for audio port");
            free(port->pool);
            port->pool = nullptr;
            free_voice(voice);
            return nullptr;
        }

        if (!audrvVoiceInit(&driver, voice, nb_channels, PcmFormat_Int16, freq)) {
            LOG_ERROR("audrvVoiceInit failed (ch={} freq={})", nb_channels, freq);
            audrvMemPoolDetach(&driver, port->mempool_id);
            audrvMemPoolRemove(&driver, port->mempool_id);
            audrvUpdate(&driver);
            free(port->pool);
            port->pool = nullptr;
            free_voice(voice);
            return nullptr;
        }

        audrvVoiceSetDestinationMix(&driver, voice, AUDREN_FINAL_MIX_ID);
        if (nb_channels == 1) {
            // Mono voice fanned out to both output channels.
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 0, 0);
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 0, 1);
        } else {
            // Stereo identity mapping.
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 0, 0);
            audrvVoiceSetMixFactor(&driver, voice, 0.0f, 0, 1);
            audrvVoiceSetMixFactor(&driver, voice, 0.0f, 1, 0);
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 1, 1);
        }
        audrvVoiceSetVolume(&driver, voice, port->volume);
        audrvVoiceStart(&driver, voice);
        audrvUpdate(&driver);
    }

    LOG_INFO("Audio port opened on voice {}: {} ch @ {} Hz, {} samples, volume {}",
        voice, nb_channels, freq, nb_sample, port->volume);

    return port;
}

void SwitchAudioAdapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    auto &port = static_cast<SwitchAudioOutPort &>(out_port);
    if (port.voice_id < 0 || !port.pool)
        return;

    // A Vita port is normally single-producer, but serialize it explicitly so
    // two guest threads can never claim the same reusable wavebuf slot.
    std::lock_guard<std::mutex> submit_lock(port.submit_mutex);

    // Find a reusable wavebuf slot, applying backpressure until one frees up so
    // the guest is paced by the renderer's consumption rate. Slot states live
    // under driver_mutex, so the wait must use that same lock with the search as
    // its predicate or a wakeup landing in between is lost.
    const auto find_slot = [&]() {
        for (int i = 0; i < port.num_slots; i++) {
            const AudioDriverWaveBufState st = port.wavebufs[i].state;
            if (st == AudioDriverWaveBufState_Free || st == AudioDriverWaveBufState_Done)
                return i;
        }
        return -1;
    };

    std::unique_lock<std::mutex> dlock(driver_mutex);
    int slot = find_slot();
    while (slot < 0) {
        if (out_port.stopping)
            return;
        wait_cond.wait_for(dlock, std::chrono::microseconds(port.len_microseconds * 2 + 1000));
        if (out_port.stopping)
            return;
        slot = find_slot();
    }
    void *const dst = static_cast<u8 *>(port.pool) + slot * port.slot_bytes;
    memcpy(dst, buffer, out_port.len_bytes);
    armDCacheFlush(dst, out_port.len_bytes);

    const int nb_sample = out_port.len_bytes / (port.channels * static_cast<int>(sizeof(int16_t)));
    AudioDriverWaveBuf &wb = port.wavebufs[slot];
    wb = AudioDriverWaveBuf{};
    wb.data_pcm16 = static_cast<s16 *>(dst);
    wb.size = out_port.len_bytes;
    wb.start_sample_offset = 0;
    wb.end_sample_offset = nb_sample;
    wb.state = AudioDriverWaveBufState_Free;

    if (!audrvVoiceAddWaveBuf(&driver, port.voice_id, &wb)) {
        LOG_WARN("audrvVoiceAddWaveBuf failed for voice {}", port.voice_id);
        return;
    }
    port.submitted_samples += nb_sample;

    // Restart the voice if it stopped after draining its queue.
    if (!audrvVoiceIsPlaying(&driver, port.voice_id))
        audrvVoiceStart(&driver, port.voice_id);

}

void SwitchAudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    auto &port = static_cast<SwitchAudioOutPort &>(out_port);
    if (port.voice_id < 0)
        return;
    std::lock_guard<std::mutex> dlock(driver_mutex);
    audrvVoiceSetVolume(&driver, port.voice_id, volume);
}

int SwitchAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    auto &port = static_cast<SwitchAudioOutPort &>(out_port);
    if (port.voice_id < 0)
        return 0;
    std::lock_guard<std::mutex> dlock(driver_mutex);
    const uint32_t played_low = audrvVoiceGetPlayedSampleCount(&driver, port.voice_id);
    if (port.played_samples_initialized && played_low < port.last_played_samples)
        port.played_samples_epoch += uint64_t{ 1 } << 32;
    port.played_samples_initialized = true;
    port.last_played_samples = played_low;

    const uint64_t played = port.played_samples_epoch + played_low;
    const uint64_t pending = port.submitted_samples > played ? port.submitted_samples - played : 0;
    return static_cast<int>(std::min<uint64_t>(pending, std::numeric_limits<int>::max()));
}

void SwitchAudioAdapter::switch_state(const bool pause) {
    std::lock_guard<std::mutex> vlock(voice_mutex);
    std::lock_guard<std::mutex> dlock(driver_mutex);
    for (int i = 0; i < static_cast<int>(voice_used.size()); i++) {
        if (voice_used[i])
            audrvVoiceSetPaused(&driver, i, pause);
    }
    audrvUpdate(&driver);
}

void SwitchAudioAdapter::wake_all_ports() {
    wait_cond.notify_all();
}

void SwitchAudioAdapter::reap_detached_pools() {
    for (auto it = pending_pool_releases.begin(); it != pending_pool_releases.end();) {
        if (driver.in_mempools[it->mempool_id].state != AudioRendererMemPoolState_Detached) {
            ++it;
            continue;
        }
        audrvMemPoolRemove(&driver, it->mempool_id);
        free(it->pool);
        it = pending_pool_releases.erase(it);
    }
}

SwitchAudioOutPort::~SwitchAudioOutPort() {
    {
        std::lock_guard<std::mutex> dlock(adapter.driver_mutex);
        if (voice_id >= 0) {
            audrvVoiceStop(&adapter.driver, voice_id);
            audrvVoiceDrop(&adapter.driver, voice_id);
        }
        if (mempool_id >= 0) {
            audrvMemPoolDetach(&adapter.driver, mempool_id);
            adapter.pending_pool_releases.push_back({ mempool_id, pool });
            pool = nullptr;
        }
        audrvUpdate(&adapter.driver);
    }
    adapter.free_voice(voice_id);
    if (pool) {
        free(pool);
        pool = nullptr;
    }
}
