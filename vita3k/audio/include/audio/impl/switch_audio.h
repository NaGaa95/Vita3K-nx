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

#pragma once

#include "../state.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <switch.h>
// libnx defines BIT()/BITL() function-like macros that collide with identifiers
// elsewhere in the tree; the audio backend does not need them.
#undef BIT
#undef BITL

// Native Nintendo Switch audio backend built on the Horizon audio renderer
// (audren). Each Vita output port maps to one audren voice; the renderer
// performs sample-rate conversion and mixing in hardware, matching Vita3K's
// multi-port push model. PCM pushed by the guest is copied into a per-port
// memory-pool ring of wavebufs and queued to the voice.
class SwitchAudioAdapter : public AudioAdapter {
public:
    explicit SwitchAudioAdapter(AudioState &audio_state);
    ~SwitchAudioAdapter() override;

    bool init() override;
    void switch_state(const bool pause) override;
    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override;
    void audio_output(AudioOutPort &out_port, const void *buffer) override;
    void set_volume(AudioOutPort &out_port, float volume) override;
    int get_rest_sample(AudioOutPort &out_port) override;
    void wake_all_ports() override;
    bool handles_output_pacing() const override { return true; }

    // Serialises every audrv* call between the guest audio threads and the
    // renderer-update thread.
    std::mutex driver_mutex;
    AudioDriver driver{};

    int alloc_voice();
    void free_voice(int voice_id);

    // Backpressure: the update thread notifies this after each renderer frame so
    // audio_output threads that ran out of free wavebuf slots can re-check.
    std::condition_variable wait_cond;

    // Pools whose detach has been requested. The renderer applies updates on
    // its own 5 ms frame cadence, so a port close cannot wait the detach out;
    // the update thread removes each pool and frees its buffer once the state
    // reads Detached. Until then both must stay alive: Remove refuses an
    // attached pool (leaking the slot), and the service still DMA-reads its
    // memory. Guarded by driver_mutex.
    struct PendingPoolRelease {
        int mempool_id;
        void *pool;
    };
    std::vector<PendingPoolRelease> pending_pool_releases;

    // Removes and frees every pending pool the renderer has finished with.
    // driver_mutex must be held.
    void reap_detached_pools();

private:
    void update_thread_func();

    bool initialized = false;
    std::atomic<bool> running{ false };
    std::thread update_thread;

    std::mutex voice_mutex;
    std::vector<bool> voice_used;
};

struct SwitchAudioOutPort : public AudioOutPort {
    SwitchAudioAdapter &adapter;
    int voice_id = -1;
    int channels = 2;
    int freq = 48000;

    // memory-pool-backed ring of PCM slots and their wavebufs
    void *pool = nullptr;
    size_t pool_size = 0;
    int mempool_id = -1;
    size_t slot_bytes = 0;
    int num_slots = 0;
    std::vector<AudioDriverWaveBuf> wavebufs;
    std::mutex submit_mutex;

    // total samples ever submitted, for get_rest_sample() bookkeeping
    uint64_t submitted_samples = 0;
    uint64_t played_samples_epoch = 0;
    uint32_t last_played_samples = 0;
    bool played_samples_initialized = false;

    explicit SwitchAudioOutPort(SwitchAudioAdapter &adapter)
        : adapter(adapter) {}
    ~SwitchAudioOutPort() override;
};
