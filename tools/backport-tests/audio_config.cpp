#include <audio/state.h>
#include <util/lock_and_find.h>

#include <cassert>
#include <iostream>

struct EmuEnv {
    struct {
        std::mutex mutex;
        AudioOutPortPtrs out_ports;
        int opened = 0;
        AudioOutPortPtr open_port(int, int, int) {
            ++opened;
            return std::make_shared<AudioOutPort>();
        }
        void set_volume(AudioOutPort &port, float volume) { port.volume = volume; }
    } audio;
};
#define EXPORT(type, name, ...) type name(EmuEnv &emuenv __VA_OPT__(, ) __VA_ARGS__)
#define TRACY_FUNC(...) ((void)0)
#define RET_ERROR(error) static_cast<int>(error)
#define CALL_EXPORT(name, ...) name(emuenv, __VA_ARGS__)
#include "audio_config.inc"

int main() {
    EmuEnv env;
    auto port = std::make_shared<AudioOutPort>();
    port->type = SCE_AUDIO_OUT_PORT_TYPE_BGM;
    port->len = 256;
    port->freq = 44100;
    port->mode = SCE_AUDIO_OUT_MODE_STEREO;
    port->left_channel_volume = 123;
    port->right_channel_volume = 456;
    port->volume = 0.25f;
    env.audio.out_ports[7] = port;
    assert(sceAudioOutGetConfig(env, 7, SCE_AUDIO_OUT_CONFIG_TYPE_LEN) == 256);
    assert(sceAudioOutGetConfig(env, 7, SCE_AUDIO_OUT_CONFIG_TYPE_FREQ) == 44100);
    assert(sceAudioOutGetConfig(env, 7, SCE_AUDIO_OUT_CONFIG_TYPE_MODE) == SCE_AUDIO_OUT_MODE_STEREO);
    assert(sceAudioOutGetConfig(env, 8, SCE_AUDIO_OUT_CONFIG_TYPE_LEN) == static_cast<int>(SCE_AUDIO_OUT_ERROR_INVALID_PORT));
    assert(sceAudioOutGetConfig(env, 7, static_cast<SceAudioOutConfigType>(99)) == static_cast<int>(SCE_AUDIO_OUT_ERROR_INVALID_CONF_TYPE));
    assert(sceAudioOutSetConfig(env, 7, -1, -1, -1) == 0);
    assert(env.audio.opened == 0 && env.audio.out_ports.at(7) == port);
    assert(sceAudioOutSetConfig(env, 7, 512, 48000, SCE_AUDIO_OUT_MODE_MONO) == 0);
    port = env.audio.out_ports.at(7);
    assert(port->type == SCE_AUDIO_OUT_PORT_TYPE_BGM);
    assert(port->left_channel_volume == 123 && port->right_channel_volume == 456 && port->volume == 0.25f);
    assert(port->len == 512 && port->freq == 48000 && port->mode == SCE_AUDIO_OUT_MODE_MONO);
    port->type = SCE_AUDIO_OUT_PORT_TYPE_MAIN;
    assert(sceAudioOutSetConfig(env, 7, -1, 44100, -1) == static_cast<int>(SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ));
    assert(env.audio.opened == 1);
    std::cout << "Audio config: queries, invalid ports/types, unchanged config and preserved type/volume passed\n";
}
