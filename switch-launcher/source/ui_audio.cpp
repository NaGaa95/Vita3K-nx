#include "ui_audio.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
constexpr int SampleRate = 48000;
SDL_AudioDeviceID s_device = 0;
bool s_enabled = true;
std::vector<int16_t> s_navigate;
std::vector<int16_t> s_confirm;
std::vector<int16_t> s_back;

std::vector<int16_t> makeTone(float startHz, float endHz, int milliseconds, float volume)
{
	const int frames = SampleRate * milliseconds / 1000;
	std::vector<int16_t> samples(static_cast<size_t>(frames) * 2);
	double phase = 0.0;
	for (int frame = 0; frame < frames; ++frame)
	{
		const float progress = frames > 1 ? static_cast<float>(frame) / (frames - 1) : 0.0f;
		const float frequency = startHz + (endHz - startHz) * progress;
		phase += 6.283185307179586 * frequency / SampleRate;
		const float attack = std::min(1.0f, progress * 10.0f);
		const float release = std::max(0.0f, 1.0f - progress);
		const float envelope = attack * release * release;
		const int16_t value = static_cast<int16_t>(std::sin(phase) * envelope * volume * 32767.0f);
		samples[static_cast<size_t>(frame) * 2] = value;
		samples[static_cast<size_t>(frame) * 2 + 1] = value;
	}
	return samples;
}

const std::vector<int16_t>& samplesFor(UiSound sound)
{
	switch (sound)
	{
	case UiSound::Navigate: return s_navigate;
	case UiSound::Confirm: return s_confirm;
	case UiSound::Back: return s_back;
	}
	return s_navigate;
}
} // namespace

bool uiAudioInit()
{
	if (s_device)
		return true;
	SDL_AudioSpec requested{};
	requested.freq = SampleRate;
	requested.format = AUDIO_S16SYS;
	requested.channels = 2;
	requested.samples = 512;
	s_device = SDL_OpenAudioDevice(nullptr, 0, &requested, nullptr, 0);
	if (!s_device)
		return false;
	s_navigate = makeTone(920.0f, 1040.0f, 18, 0.10f);
	s_confirm = makeTone(620.0f, 980.0f, 42, 0.16f);
	s_back = makeTone(760.0f, 420.0f, 48, 0.14f);
	SDL_PauseAudioDevice(s_device, 0);
	return true;
}

void uiAudioPlay(UiSound sound)
{
	if (!s_device || !s_enabled)
		return;
	if (SDL_GetQueuedAudioSize(s_device) > SampleRate * sizeof(int16_t))
		SDL_ClearQueuedAudio(s_device);
	const auto& samples = samplesFor(sound);
	SDL_QueueAudio(s_device, samples.data(), static_cast<Uint32>(samples.size() * sizeof(samples[0])));
}

void uiAudioSetEnabled(bool enabled)
{
	s_enabled = enabled;
	if (!enabled && s_device)
		SDL_ClearQueuedAudio(s_device);
}

void uiAudioShutdown()
{
	if (s_device)
	{
		SDL_ClearQueuedAudio(s_device);
		SDL_CloseAudioDevice(s_device);
		s_device = 0;
	}
	s_navigate.clear();
	s_confirm.clear();
	s_back.clear();
}
