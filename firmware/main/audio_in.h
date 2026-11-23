// Stereo input from the PCM1808 module (I2S master, 48 kHz, 24 bit).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_SAMPLE_RATE    48000
#define AUDIO_CHANNELS       2
// Length of the snapshot handed to the oscilloscope view (20 ms).
#define AUDIO_SCOPE_FRAMES   960

typedef struct {
    // Since the previous call. -120 means digital silence or no data.
    float peak_db[AUDIO_CHANNELS];
    float rms_db[AUDIO_CHANNELS];
    // Number of 10 ms blocks that contained full-scale samples, since the
    // last audio_in_clear_clip().
    uint32_t clip_blocks[AUDIO_CHANNELS];
    // True while a channel has clipped within the last half second.
    bool clipping_now[AUDIO_CHANNELS];
    // False when the ADC module delivers no clock.
    bool signal_ok;
    // Sample rate measured from the incoming data, 0 until known.
    uint32_t measured_rate_hz;
} audio_levels_t;

esp_err_t audio_in_start(void);

// Levels accumulated since the previous call; the call starts a new interval.
void audio_in_get_levels(audio_levels_t *out);

// Copies the most recent AUDIO_SCOPE_FRAMES frames into out, interleaved
// L, R on the 16-bit scale. Returns false while there is no input.
bool audio_in_get_scope(int16_t *out);

void audio_in_clear_clip(void);
