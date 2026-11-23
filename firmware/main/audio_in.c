#include "audio_in.h"

#include <math.h>
#include <string.h>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_in";

// 10 ms per read. 480 frames of 2 x 32 bit fit one DMA descriptor (4092 B).
#define BLOCK_FRAMES   480
// A 24-bit sample at or beyond this magnitude counts as clipped.
#define CLIP_THRESHOLD 0x7FFF00
#define FULL_SCALE     8388608.0f
#define COL_FIFO_LEN   32
#define SILENCE_DB     (-120.0f)

static i2s_chan_handle_t s_rx;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// Everything below is shared with the UI task and guarded by s_lock.
static int32_t s_peak[AUDIO_CHANNELS];
static uint64_t s_sum_sq[AUDIO_CHANNELS];
static uint32_t s_sum_count;
static uint32_t s_clip_blocks[AUDIO_CHANNELS];
static int64_t s_last_clip_us[AUDIO_CHANNELS];
static bool s_signal_ok;
static uint32_t s_measured_rate;
static audio_col_t s_cols[COL_FIFO_LEN];
static int s_col_head;
static int s_col_count;

static void push_col(const audio_col_t *col)
{
    portENTER_CRITICAL(&s_lock);
    s_cols[s_col_head] = *col;
    s_col_head = (s_col_head + 1) % COL_FIFO_LEN;
    if (s_col_count < COL_FIFO_LEN) {
        s_col_count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void audio_task(void *arg)
{
    static int32_t buf[BLOCK_FRAMES * AUDIO_CHANNELS];

    audio_col_t col = {0};
    int col_frames = 0;
    int64_t rate_t0 = esp_timer_get_time();
    uint32_t rate_frames = 0;

    for (;;) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(200));
        if (err != ESP_OK || got == 0) {
            // The module is the clock master: no clock means no data at all.
            portENTER_CRITICAL(&s_lock);
            s_signal_ok = false;
            s_measured_rate = 0;
            portEXIT_CRITICAL(&s_lock);
            rate_t0 = esp_timer_get_time();
            rate_frames = 0;
            continue;
        }

        int frames = got / (sizeof(int32_t) * AUDIO_CHANNELS);
        int32_t peak[AUDIO_CHANNELS] = {0};
        uint64_t sum_sq[AUDIO_CHANNELS] = {0};
        bool clipped[AUDIO_CHANNELS] = {false};

        for (int i = 0; i < frames; i++) {
            for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
                // The 24-bit sample sits in the top three bytes of the slot.
                int32_t s = buf[i * AUDIO_CHANNELS + ch] >> 8;
                int32_t mag = s < 0 ? -s : s;
                if (mag > peak[ch]) {
                    peak[ch] = mag;
                }
                if (mag >= CLIP_THRESHOLD) {
                    clipped[ch] = true;
                }
                int32_t s16 = s >> 8;
                sum_sq[ch] += (uint64_t)((int64_t)s16 * s16);

                if (col_frames == 0) {
                    col.min[ch] = col.max[ch] = (int16_t)s16;
                    col.clipped[ch] = false;
                } else {
                    if (s16 < col.min[ch]) {
                        col.min[ch] = (int16_t)s16;
                    }
                    if (s16 > col.max[ch]) {
                        col.max[ch] = (int16_t)s16;
                    }
                }
                if (mag >= CLIP_THRESHOLD) {
                    col.clipped[ch] = true;
                }
            }
            if (++col_frames >= AUDIO_FRAMES_PER_COL) {
                push_col(&col);
                col_frames = 0;
            }
        }

        int64_t now = esp_timer_get_time();
        rate_frames += frames;
        uint32_t rate = 0;
        if (now - rate_t0 >= 2000000) {
            rate = (uint32_t)((uint64_t)rate_frames * 1000000 / (now - rate_t0));
            rate_t0 = now;
            rate_frames = 0;
        }

        portENTER_CRITICAL(&s_lock);
        s_signal_ok = true;
        if (rate) {
            s_measured_rate = rate;
        }
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            if (peak[ch] > s_peak[ch]) {
                s_peak[ch] = peak[ch];
            }
            s_sum_sq[ch] += sum_sq[ch];
            if (clipped[ch]) {
                s_clip_blocks[ch]++;
                s_last_clip_us[ch] = now;
            }
        }
        s_sum_count += frames;
        portEXIT_CRITICAL(&s_lock);

        if (rate) {
            ESP_LOGI(TAG, "measured sample rate: %lu Hz", (unsigned long)rate);
        }
    }
}

esp_err_t audio_in_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = BLOCK_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_PIN_I2S_BCK,
            .ws = BOARD_PIN_I2S_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_PIN_I2S_DATA,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio_in", 4096, NULL,
                                            configMAX_PRIORITIES - 2, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static float to_db(float ratio)
{
    return ratio > 0.000001f ? 20.0f * log10f(ratio) : SILENCE_DB;
}

void audio_in_get_levels(audio_levels_t *out)
{
    int32_t peak[AUDIO_CHANNELS];
    uint64_t sum_sq[AUDIO_CHANNELS];
    uint32_t count;
    int64_t last_clip[AUDIO_CHANNELS];

    portENTER_CRITICAL(&s_lock);
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        peak[ch] = s_peak[ch];
        sum_sq[ch] = s_sum_sq[ch];
        last_clip[ch] = s_last_clip_us[ch];
        out->clip_blocks[ch] = s_clip_blocks[ch];
        s_peak[ch] = 0;
        s_sum_sq[ch] = 0;
    }
    count = s_sum_count;
    s_sum_count = 0;
    out->signal_ok = s_signal_ok;
    out->measured_rate_hz = s_measured_rate;
    portEXIT_CRITICAL(&s_lock);

    int64_t now = esp_timer_get_time();
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        out->peak_db[ch] = to_db((float)peak[ch] / FULL_SCALE);
        // The squares were summed on the 16-bit scale.
        float mean = count ? (float)((double)sum_sq[ch] / count) : 0.0f;
        out->rms_db[ch] = to_db(sqrtf(mean) / 32768.0f);
        out->clipping_now[ch] = last_clip[ch] != 0 && now - last_clip[ch] < 500000;
    }
}

int audio_in_read_cols(audio_col_t *cols, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    while (s_col_count > 0 && n < max) {
        int tail = (s_col_head - s_col_count + COL_FIFO_LEN) % COL_FIFO_LEN;
        cols[n++] = s_cols[tail];
        s_col_count--;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void audio_in_clear_clip(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_clip_blocks, 0, sizeof(s_clip_blocks));
    memset(s_last_clip_us, 0, sizeof(s_last_clip_us));
    portEXIT_CRITICAL(&s_lock);
}
