// SPDX-License-Identifier: MIT
// Startup Plugin - Audio Playback

#pragma once

#include <stdbool.h>

int audio_init(void);
void audio_cleanup(void);
void audio_play_file(const char* path);
bool audio_is_finished(void);
void audio_stop(void);
