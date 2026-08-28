// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

#include "../state.h"

class NullAudioAdapter final : public AudioAdapter {
public:
    explicit NullAudioAdapter(AudioState &audio_state);

    bool init() override;
    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override;
    void audio_output(AudioOutPort &out_port, const void *buffer) override;
    int get_rest_sample(AudioOutPort &out_port) override;
    bool handles_output_pacing() const override { return true; }
};
