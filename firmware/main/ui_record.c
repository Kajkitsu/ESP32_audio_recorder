#include "ui_record.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "audio_in.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "storage.h"

#define WAVE_W        232
#define WAVE_H        64
#define METER_W       150
#define METER_H       14
#define METER_MIN_DB  (-60)
#define WARN_DB       (-12.0f)
#define HOT_DB        (-3.0f)
#define PEAK_HOLD_US  1500000
#define REFRESH_MS    50

#define COLOR_BG      lv_color_hex(0x000000)
#define COLOR_PANEL   lv_color_hex(0x101820)
#define COLOR_GRID    lv_color_hex(0x2A3440)
#define COLOR_TEXT    lv_color_hex(0xE0E0E0)
#define COLOR_DIM     lv_color_hex(0x808890)
#define COLOR_OK      lv_color_hex(0x30D060)
#define COLOR_WARN    lv_color_hex(0xF0C020)
#define COLOR_HOT     lv_color_hex(0xFF7020)
#define COLOR_CLIP    lv_color_hex(0xFF2020)

static lv_obj_t *s_status;
static lv_obj_t *s_info;
static lv_obj_t *s_canvas[AUDIO_CHANNELS];
static lv_color_t *s_canvas_buf[AUDIO_CHANNELS];
static lv_obj_t *s_bar[AUDIO_CHANNELS];
static lv_obj_t *s_hold_mark[AUDIO_CHANNELS];
static lv_obj_t *s_db_label[AUDIO_CHANNELS];
static lv_obj_t *s_clip_banner;
static lv_obj_t *s_clip_label;
static lv_obj_t *s_card_label;
static lv_obj_t *s_touch_label;
static lv_obj_t *s_touch_dot;

static float s_hold_db[AUDIO_CHANNELS] = {-120.0f, -120.0f};
static int64_t s_hold_us[AUDIO_CHANNELS];
static int s_card_refresh_ticks;

static lv_color_t level_color(float db, bool clipped)
{
    if (clipped) {
        return COLOR_CLIP;
    }
    if (db >= HOT_DB) {
        return COLOR_HOT;
    }
    if (db >= WARN_DB) {
        return COLOR_WARN;
    }
    return COLOR_OK;
}

static float col_peak_db(const audio_col_t *col, int ch)
{
    int lo = -col->min[ch];
    int hi = col->max[ch];
    int mag = lo > hi ? lo : hi;
    if (mag <= 0) {
        return -120.0f;
    }
    // Good enough for picking a colour; the meters use the exact value.
    float ratio = (float)mag / 32768.0f;
    return ratio >= 0.7079f ? -3.0f : (ratio >= 0.2512f ? -12.0f : -60.0f);
}

// Scrolls one channel's waveform left by a pixel and draws the new column.
static void wave_push(int ch, const audio_col_t *col)
{
    lv_color_t *px = s_canvas_buf[ch];
    memmove(px, px + 1, (WAVE_W * WAVE_H - 1) * sizeof(lv_color_t));

    const int mid = WAVE_H / 2;
    int y_top = mid - (col->max[ch] * mid) / 32768;
    int y_bot = mid - (col->min[ch] * mid) / 32768;
    if (y_top < 0) {
        y_top = 0;
    }
    if (y_bot > WAVE_H - 1) {
        y_bot = WAVE_H - 1;
    }
    lv_color_t wave = level_color(col_peak_db(col, ch), col->clipped[ch]);
    for (int y = 0; y < WAVE_H; y++) {
        lv_color_t c = COLOR_PANEL;
        if (y >= y_top && y <= y_bot) {
            c = wave;
        } else if (y == mid) {
            c = COLOR_GRID;
        }
        px[y * WAVE_W + WAVE_W - 1] = c;
    }
}

static void format_db(char *out, size_t len, float db)
{
    if (db <= -100.0f) {
        snprintf(out, len, "-inf");
    } else {
        snprintf(out, len, "%.1f", db);
    }
}

static void update_card_label(void)
{
    uint64_t total = 0, free_bytes = 0;
    switch (storage_state()) {
    case STORAGE_READY:
        if (storage_get_space(&total, &free_bytes)) {
            // 48 kHz, 24 bit, stereo: 288 000 bytes per second.
            uint32_t minutes = (uint32_t)(free_bytes / 288000 / 60);
            lv_label_set_text_fmt(s_card_label, "SD %lu.%lu GB free  ~%luh%02lum",
                                  (unsigned long)(free_bytes / 1000000000ULL),
                                  (unsigned long)(free_bytes % 1000000000ULL / 100000000ULL),
                                  (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
        } else {
            lv_label_set_text(s_card_label, "SD READ ERROR");
        }
        break;
    case STORAGE_UNSUPPORTED_FS:
        lv_label_set_text(s_card_label, "SD: UNSUPPORTED FILESYSTEM");
        break;
    default:
        lv_label_set_text(s_card_label, "NO SD CARD");
        break;
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    audio_levels_t lv;
    audio_in_get_levels(&lv);
    int64_t now = esp_timer_get_time();

    audio_col_t cols[8];
    int n = audio_in_read_cols(cols, 8);
    for (int i = 0; i < n; i++) {
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            wave_push(ch, &cols[i]);
        }
    }
    if (n > 0) {
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            lv_obj_invalidate(s_canvas[ch]);
        }
    }

    bool clipping = false;
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        float db = lv.signal_ok ? lv.peak_db[ch] : -120.0f;
        int bar_val = db < METER_MIN_DB ? METER_MIN_DB : (int)db;
        lv_bar_set_value(s_bar[ch], bar_val, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar[ch], level_color(db, lv.clipping_now[ch]),
                                  LV_PART_INDICATOR);

        if (db >= s_hold_db[ch] || now - s_hold_us[ch] > PEAK_HOLD_US) {
            s_hold_db[ch] = db;
            s_hold_us[ch] = now;
        }
        float hold = s_hold_db[ch] < METER_MIN_DB ? METER_MIN_DB : s_hold_db[ch];
        int x = (int)((hold - METER_MIN_DB) * (METER_W - 2) / (0 - METER_MIN_DB));
        lv_obj_set_x(s_hold_mark[ch], x);

        char text[16];
        format_db(text, sizeof(text), s_hold_db[ch]);
        lv_label_set_text(s_db_label[ch], text);
        clipping |= lv.clipping_now[ch];
    }

    if (!lv.signal_ok) {
        lv_label_set_text(s_status, "NO ADC SIGNAL");
        lv_obj_set_style_text_color(s_status, COLOR_CLIP, 0);
        lv_label_set_text(s_info, "check LRCK / BCK / DATA wires");
    } else {
        lv_label_set_text(s_status, "READY");
        lv_obj_set_style_text_color(s_status, COLOR_OK, 0);
        if (lv.measured_rate_hz) {
            lv_label_set_text_fmt(s_info, "input %lu Hz, 24 bit, stereo",
                                  (unsigned long)lv.measured_rate_hz);
        } else {
            lv_label_set_text(s_info, "measuring input...");
        }
    }

    if (lv.clip_blocks[0] || lv.clip_blocks[1]) {
        lv_label_set_text_fmt(s_clip_label, "CLIP  L:%lu  R:%lu   tap to clear",
                              (unsigned long)lv.clip_blocks[0], (unsigned long)lv.clip_blocks[1]);
        lv_obj_clear_flag(s_clip_banner, LV_OBJ_FLAG_HIDDEN);
        // Solid while it is happening, dimmer once it is only history.
        lv_obj_set_style_bg_opa(s_clip_banner, clipping ? LV_OPA_COVER : LV_OPA_60, 0);
    } else {
        lv_obj_add_flag(s_clip_banner, LV_OBJ_FLAG_HIDDEN);
    }

    if (++s_card_refresh_ticks >= 5000 / REFRESH_MS) {
        s_card_refresh_ticks = 0;
        update_card_label();
    }
}

static void clip_clicked_cb(lv_event_t *e)
{
    audio_in_clear_clip();
}

// Bring-up aid: shows where the panel reports a touch.
static void touch_probe_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_label_set_text_fmt(s_touch_label, "touch %d, %d", (int)p.x, (int)p.y);
    lv_obj_set_pos(s_touch_dot, p.x - 4, p.y - 4);
    lv_obj_clear_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                            int x, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, text);
    return label;
}

static void make_channel(lv_obj_t *scr, int ch, const char *name, int wave_y, int meter_y)
{
    size_t bytes = WAVE_W * WAVE_H * sizeof(lv_color_t);
    s_canvas_buf[ch] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_canvas_buf[ch]);
    for (int i = 0; i < WAVE_W * WAVE_H; i++) {
        s_canvas_buf[ch][i] = (i / WAVE_W == WAVE_H / 2) ? COLOR_GRID : COLOR_PANEL;
    }
    s_canvas[ch] = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas[ch], s_canvas_buf[ch], WAVE_W, WAVE_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_canvas[ch], 4, wave_y);
    make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 8, wave_y + 2, name);

    make_label(scr, &lv_font_montserrat_14, COLOR_TEXT, 4, meter_y - 1, name);
    s_bar[ch] = lv_bar_create(scr);
    lv_obj_set_size(s_bar[ch], METER_W, METER_H);
    lv_obj_set_pos(s_bar[ch], 20, meter_y);
    lv_bar_set_range(s_bar[ch], METER_MIN_DB, 0);
    lv_obj_set_style_radius(s_bar[ch], 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar[ch], 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar[ch], COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar[ch], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar[ch], COLOR_OK, LV_PART_INDICATOR);

    // Peak-hold tick, a child of the bar so it moves in the bar's coordinates.
    s_hold_mark[ch] = lv_obj_create(s_bar[ch]);
    lv_obj_remove_style_all(s_hold_mark[ch]);
    lv_obj_set_size(s_hold_mark[ch], 2, METER_H);
    lv_obj_clear_flag(s_hold_mark[ch], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_hold_mark[ch], COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(s_hold_mark[ch], LV_OPA_COVER, 0);

    s_db_label[ch] = make_label(scr, &lv_font_montserrat_14, COLOR_TEXT, 178, meter_y - 1, "-inf");
}

void ui_record_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_status = make_label(scr, &lv_font_montserrat_20, COLOR_OK, 4, 4, "STARTING");
    s_info = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 4, 30, "");

    make_channel(scr, 0, "L", 50, 190);
    make_channel(scr, 1, "R", 118, 212);

    // Scale marks under the meters: the two thresholds and full scale.
    make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 20, 230, "-60");
    make_label(scr, &lv_font_montserrat_14, COLOR_WARN, 128, 230, "-12");
    make_label(scr, &lv_font_montserrat_14, COLOR_HOT, 158, 230, "0 dB");

    s_touch_label = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 4, 260, "touch the screen");
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, touch_probe_cb, LV_EVENT_PRESSING, NULL);

    s_clip_banner = lv_obj_create(scr);
    lv_obj_remove_style_all(s_clip_banner);
    lv_obj_set_size(s_clip_banner, BOARD_LCD_H_RES, 34);
    lv_obj_set_pos(s_clip_banner, 0, 252);
    lv_obj_set_style_bg_color(s_clip_banner, COLOR_CLIP, 0);
    lv_obj_set_style_bg_opa(s_clip_banner, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_clip_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_clip_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_clip_banner, clip_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_clip_label = lv_label_create(s_clip_banner);
    lv_obj_set_style_text_font(s_clip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_clip_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_clip_label);

    s_card_label = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 4, 298, "");
    update_card_label();

    s_touch_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_touch_dot);
    lv_obj_set_size(s_touch_dot, 8, 8);
    lv_obj_set_style_bg_color(s_touch_dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_touch_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_touch_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    lvgl_port_unlock();
}
