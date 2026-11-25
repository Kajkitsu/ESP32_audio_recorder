// Writes the input to WAV files on the SD card (48 kHz, 24 bit, stereo).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    REC_IDLE,
    REC_STARTING,
    REC_RECORDING,
    REC_STOPPING,
} rec_state_t;

typedef struct {
    rec_state_t state;
    char file_name[40];      // current or last file, without the directory
    uint32_t elapsed_s;      // length of the current recording
    uint32_t remaining_s;    // recording time left on the card, 0 without a card
    uint32_t dropped_frames; // audio lost because the card was too slow
    uint8_t buffer_pct;      // fill level of the buffer in front of the card
    uint32_t markers;        // markers set in the current recording
    char message[32];        // why the last start or recording failed, or ""
} rec_status_t;

// Allocates the buffer, repairs files left open by a power cut and starts the
// writer task. Call once, after the card mount attempt.
esp_err_t recorder_init(void);

// Requests are asynchronous; follow them with recorder_get_status().
void recorder_start(void);
void recorder_stop(void);
void recorder_add_marker(void);

void recorder_get_status(rec_status_t *out);

// Card operations since the previous call and the longest of them, for the
// diagnostic log.
void recorder_take_stats(uint32_t *writes, uint32_t *max_write_ms);

// Called by the audio task with raw I2S frames (two 32-bit slots per frame).
void recorder_feed(const int32_t *frames, int n_frames);
