// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "audio/impl/null_audio.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

uint64_t now_microseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

NullAudioAdapter::NullAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

bool NullAudioAdapter::init() {
    return true;
}

AudioOutPortPtr NullAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    if ((nb_channels != 1 && nb_channels != 2) || freq <= 0 || nb_sample <= 0)
        return nullptr;

    auto port = std::make_shared<AudioOutPort>();
    port->len = nb_sample;
    port->freq = freq;
    port->mode = nb_channels == 1 ? 0 : 1;
    port->len_bytes = nb_sample * nb_channels * static_cast<int>(sizeof(int16_t));
    port->len_microseconds = static_cast<uint64_t>(nb_sample) * 1'000'000ULL / static_cast<uint64_t>(freq);
    return port;
}

void NullAudioAdapter::audio_output(AudioOutPort &out_port, const void *) {
    const uint64_t now = now_microseconds();
    const uint64_t deadline = out_port.last_output + out_port.len_microseconds;
    if (out_port.last_output != 0 && now < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(deadline - now));
        out_port.last_output = deadline;
    } else {
        out_port.last_output = now;
    }
}

int NullAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    if (out_port.last_output == 0 || out_port.len_microseconds == 0 || out_port.freq <= 0)
        return 0;

    const uint64_t now = now_microseconds();
    const uint64_t elapsed = now - out_port.last_output;
    if (elapsed >= out_port.len_microseconds)
        return 0;

    const uint64_t remaining_us = out_port.len_microseconds - elapsed;
    const int remaining = static_cast<int>(remaining_us * static_cast<uint64_t>(out_port.freq) / 1'000'000ULL);
    return std::clamp(remaining, 0, out_port.len);
}
