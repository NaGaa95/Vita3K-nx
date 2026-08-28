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

#include <audio/state.h>

#include <audio/impl/null_audio.h>
#ifdef __SWITCH__
#include <audio/impl/switch_audio.h>
#else
#include <audio/impl/cubeb_audio.h>
#endif
// SDL owns the capture stream on every platform, whatever the output backend is.
#include <audio/impl/sdl_audio.h>

#include <util/log.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

bool AudioState::init(const std::string &adapter_name) {
    set_backend(adapter_name);
    return static_cast<bool>(adapter);
}

void AudioState::stop_all_ports() {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto &[_, port] : out_ports) {
            port->stopping = true;
        }
    }
    if (adapter)
        adapter->wake_all_ports();
    if (silent_adapter)
        silent_adapter->wake_all_ports();
}

void AudioState::deinit() {
    stop_all_ports();

    const std::lock_guard<std::mutex> lock(mutex);

    out_ports.clear();

    if (in_port.running) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream *>(in_port.id));
        in_port.id = nullptr;
        in_port.running = false;
        in_port.len_bytes = 0;
    }

    next_port_id = 1;
}

void AudioState::set_backend(const std::string &adapter_name) {
    std::string backend_name = adapter_name;
#ifdef __SWITCH__
    // audren is the only backend on Horizon, and the launcher does not write the
    // key, so this always arrives as Vita3K's "SDL" default.
    backend_name = "Switch";
#endif

    if (backend_name == this->audio_backend && adapter)
        return;

    stop_all_ports();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        out_ports.clear();
    }
    silent_adapter.reset();
    adapter.reset();
#ifdef __SWITCH__
    adapter = std::make_unique<SwitchAudioAdapter>(*this);
#else
    if (backend_name == "SDL") {
        adapter = std::make_unique<SDLAudioAdapter>(*this);
    } else if (backend_name == "Cubeb") {
        adapter = std::make_unique<CubebAudioAdapter>(*this);
    } else {
        LOG_ERROR("Unknown audio adapter {}; using silent audio", backend_name);
    }
#endif
    this->audio_backend = backend_name;

    if (adapter && !adapter->init()) {
        LOG_WARN("Audio adapter {} failed to initialize; using silent audio", backend_name);
        adapter.reset();
    }

    if (!adapter) {
        adapter = std::make_unique<NullAudioAdapter>(*this);
        if (!adapter->init())
            adapter.reset();
    }
}

AudioOutPortPtr AudioState::open_port(int nb_channels, int freq, int nb_sample) {
    if (!adapter)
        return nullptr;

    AudioAdapter *port_backend = adapter.get();
    AudioOutPortPtr port = port_backend->open_port(nb_channels, freq, nb_sample);
    if (!port) {
        LOG_WARN("Audio adapter {} could not open a port; using silent pacing for this port", audio_backend);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (!silent_adapter) {
                silent_adapter = std::make_unique<NullAudioAdapter>(*this);
                if (!silent_adapter->init())
                    silent_adapter.reset();
            }
            port_backend = silent_adapter.get();
        }
        if (!port_backend)
            return nullptr;
        port = port_backend->open_port(nb_channels, freq, nb_sample);
    }
    if (!port)
        return nullptr;

    port->backend = port_backend;
    set_volume(*port, port->volume);
    return port;
}

void AudioState::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    AudioAdapter *const port_backend = out_port.backend ? out_port.backend : adapter.get();
    if (!port_backend)
        return;

    port_backend->audio_output(out_port, buffer);

    if (out_port.stopping)
        return;
    if (port_backend->handles_output_pacing())
        return;

    uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t diff = now - out_port.last_output;
    uint64_t to_wait = out_port.len_microseconds - diff;
    if (diff < out_port.len_microseconds && to_wait > 1000) {
        // This is what we should be waiting to be perfectly accurate
        // However, doing so would cause the host audio buffer to often lack samples to output
        // This is because the PS Vita and the host audio parameters do not match exactly
        // So instead only wait 50% of the time
        // also don't sleep for less than 0.5 ms
        to_wait /= 2;
        std::this_thread::sleep_for(std::chrono::microseconds(to_wait));
        out_port.last_output = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    } else {
        out_port.last_output = now;
    }
}

void AudioState::set_volume(AudioOutPort &out_port, float volume) {
    out_port.volume = volume;
    AudioAdapter *const port_backend = out_port.backend ? out_port.backend : adapter.get();
    if (port_backend)
        port_backend->set_volume(out_port, volume * global_volume);
}

void AudioState::set_global_volume(float volume) {
    global_volume = volume;
    //  Update adapter volume for each port.
    const std::lock_guard lock(mutex);
    for (const auto &[_, port] : out_ports) {
        AudioAdapter *const port_backend = port->backend ? port->backend : adapter.get();
        if (port_backend)
            port_backend->set_volume(*port, port->volume * volume);
    }
}

void AudioState::switch_state(const bool pause) {
    if (adapter)
        adapter->switch_state(pause);
    if (silent_adapter)
        silent_adapter->switch_state(pause);
}

int AudioState::get_rest_sample(AudioOutPort &out_port) {
    AudioAdapter *const port_backend = out_port.backend ? out_port.backend : adapter.get();
    return port_backend ? port_backend->get_rest_sample(out_port) : 0;
}

void AudioState::wake_all_ports() {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto &[_, port] : out_ports) {
            port->stopping = true;
        }
    }
    if (adapter)
        adapter->wake_all_ports();
    if (silent_adapter)
        silent_adapter->wake_all_ports();
}
