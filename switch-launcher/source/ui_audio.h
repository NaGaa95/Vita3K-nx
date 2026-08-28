#pragma once

enum class UiSound
{
	Navigate,
	Confirm,
	Back,
};

bool uiAudioInit();
void uiAudioSetEnabled(bool enabled);
void uiAudioPlay(UiSound sound);
void uiAudioShutdown();
