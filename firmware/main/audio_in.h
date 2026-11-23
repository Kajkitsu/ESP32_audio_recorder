// Stereo input from the PCM1808 module (I2S master, 48 kHz, 24 bit).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_SAMPLE_RATE    48000
#define AUDIO_CHANNELS       2
// One waveform column covers this many frames (100 ms).
#define AUDIO_FRAMES_PER_COL 4800

// One column of the scrolling waveform: sample range per channel, 16-bit scale.
typedef struct {
    int16_t min[AUDIO_CHANNELS];
    int16_t max[AUDIO_CHANNELS];
    bool clipped[AUDIO_CHANNELS];
} audio_col_t;

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

// Moves pending waveform columns into cols and returns how many were copied.
int audio_in_read_cols(audio_col_t *cols, int max);

void audio_in_clear_clip(void);
