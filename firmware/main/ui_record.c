#include "ui_record.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "audio_in.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "recorder.h"
#include "storage.h"

#define WAVE_W         232
#define WAVE_H         64
#define METER_W        150
#define METER_H        14
#define METER_MIN_DB   (-60)
#define WARN_DB        (-12.0f)
#define HOT_DB         (-3.0f)
#define PEAK_HOLD_US   1500000
#define REFRESH_MS     50
#define SCOPE_PERIOD_TICKS   2
#define SCOPE_FRAMES_PER_PX  2
#define UNLOCK_HOLD_US 2000000
#define DIM_AFTER_MS   30000
#define BACKLIGHT_ON   80
#define BACKLIGHT_DIM  8

#define COLOR_BG       lv_color_hex(0x000000)
#define COLOR_PANEL    lv_color_hex(0x101820)
#define COLOR_GRID     lv_color_hex(0x2A3440)
#define COLOR_TEXT     lv_color_hex(0xE0E0E0)
#define COLOR_DIM      lv_color_hex(0x808890)
#define COLOR_OK       lv_color_hex(0x30D060)
#define COLOR_WARN     lv_color_hex(0xF0C020)
#define COLOR_HOT      lv_color_hex(0xFF7020)
#define COLOR_CLIP     lv_color_hex(0xFF2020)
#define COLOR_BUTTON   lv_color_hex(0x2A3440)
#define COLOR_REC      lv_color_hex(0xC01818)

typedef enum { LEVEL_OK, LEVEL_WARN, LEVEL_HOT, LEVEL_CLIP } level_t;

static lv_obj_t *s_status;
static lv_obj_t *s_info;
static lv_obj_t *s_canvas[AUDIO_CHANNELS];
static lv_color_t *s_canvas_buf[AUDIO_CHANNELS];
static lv_obj_t *s_bar[AUDIO_CHANNELS];
static lv_obj_t *s_hold_mark[AUDIO_CHANNELS];
static lv_obj_t *s_db_label[AUDIO_CHANNELS];
static lv_obj_t *s_clip_banner;
static lv_obj_t *s_clip_title;
static lv_obj_t *s_clip_detail;
static lv_obj_t *s_card_label;
static lv_obj_t *s_rec_btn;
static lv_obj_t *s_rec_btn_label;
static lv_obj_t *s_mark_btn;
static lv_obj_t *s_lock_overlay;
static lv_obj_t *s_lock_label;

static float s_hold_db[AUDIO_CHANNELS] = {-120.0f, -120.0f};
static int64_t s_hold_us[AUDIO_CHANNELS];
static int s_bar_level[AUDIO_CHANNELS] = {-1, -1};
static int s_hold_x[AUDIO_CHANNELS] = {-1, -1};
static int s_scope_tick;
static bool s_locked;
static int64_t s_unlock_press_us;
static int s_backlight = BACKLIGHT_ON;
static rec_state_t s_shown_state = REC_IDLE;
static bool s_shown_state_valid;

static lv_color_t level_color(level_t level)
{
    switch (level) {
    case LEVEL_CLIP:
        return COLOR_CLIP;
    case LEVEL_HOT:
        return COLOR_HOT;
    case LEVEL_WARN:
        return COLOR_WARN;
    default:
        return COLOR_OK;
    }
}

static level_t level_of(float db, bool clipped)
{
    if (clipped) {
        return LEVEL_CLIP;
    }
    if (db >= HOT_DB) {
        return LEVEL_HOT;
    }
    if (db >= WARN_DB) {
        return LEVEL_WARN;
    }
    return LEVEL_OK;
}

// Labels are redrawn (and sent over the shared SPI bus) on every set_text,
// so skip the call when nothing changed.
static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

// Draws 10 ms of one channel as an oscilloscope trace. The whole canvas is
// redrawn, so nothing has to move smoothly and a late frame goes unnoticed.
static void scope_draw(int ch, const int16_t *frames, level_t level)
{
    lv_color_t *px = s_canvas_buf[ch];
    const int mid = WAVE_H / 2;
    for (int i = 0; i < WAVE_W * WAVE_H; i++) {
        px[i] = (i / WAVE_W == mid) ? COLOR_GRID : COLOR_PANEL;
    }
    if (!frames) {
        return;
    }

    // Start at a rising zero crossing so a steady tone stands still.
    int start = 0;
    const int search = AUDIO_SCOPE_FRAMES - WAVE_W * SCOPE_FRAMES_PER_PX;
    for (int i = 1; i < search; i++) {
        if (frames[(i - 1) * AUDIO_CHANNELS + ch] < 0 && frames[i * AUDIO_CHANNELS + ch] >= 0) {
            start = i;
            break;
        }
    }

    lv_color_t color = level_color(level);
    int prev = frames[start * AUDIO_CHANNELS + ch];
    for (int x = 0; x < WAVE_W; x++) {
        // Include the previous sample so neighbouring columns join up.
        int lo = prev, hi = prev;
        for (int k = 0; k < SCOPE_FRAMES_PER_PX; k++) {
            int s = frames[(start + x * SCOPE_FRAMES_PER_PX + k) * AUDIO_CHANNELS + ch];
            if (s < lo) {
                lo = s;
            }
            if (s > hi) {
                hi = s;
            }
            prev = s;
        }
        // One pixel more than the sample range, so the trace is 2 px thick.
        int y_top = mid - (hi * mid) / 32768 - 1;
        int y_bot = mid - (lo * mid) / 32768;
        if (y_top < 0) {
            y_top = 0;
        }
        if (y_bot > WAVE_H - 1) {
            y_bot = WAVE_H - 1;
        }
        for (int y = y_top; y <= y_bot; y++) {
            px[y * WAVE_W + x] = color;
        }
    }
}

static void format_duration(char *out, size_t len, uint32_t seconds)
{
    snprintf(out, len, "%luh%02lum", (unsigned long)(seconds / 3600),
             (unsigned long)(seconds / 60 % 60));
}

static void update_meters(const audio_levels_t *lv, int64_t now)
{
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        float db = lv->signal_ok ? lv->peak_db[ch] : -120.0f;
        lv_bar_set_value(s_bar[ch], db < METER_MIN_DB ? METER_MIN_DB : (int)db, LV_ANIM_OFF);
        level_t level = level_of(db, lv->clipping_now[ch]);
        if ((int)level != s_bar_level[ch]) {
            s_bar_level[ch] = level;
            lv_obj_set_style_bg_color(s_bar[ch], level_color(level), LV_PART_INDICATOR);
        }

        if (db >= s_hold_db[ch] || now - s_hold_us[ch] > PEAK_HOLD_US) {
            s_hold_db[ch] = db;
            s_hold_us[ch] = now;
        }
        float hold = s_hold_db[ch] < METER_MIN_DB ? METER_MIN_DB : s_hold_db[ch];
        int x = (int)((hold - METER_MIN_DB) * (METER_W - 2) / (0 - METER_MIN_DB));
        if (x != s_hold_x[ch]) {
            s_hold_x[ch] = x;
            lv_obj_set_x(s_hold_mark[ch], x);
        }

        char text[16];
        if (s_hold_db[ch] <= -100.0f) {
            snprintf(text, sizeof(text), "-inf");
        } else {
            snprintf(text, sizeof(text), "%.1f", s_hold_db[ch]);
        }
        set_text(s_db_label[ch], text);
    }
}

static void update_clip_banner(const audio_levels_t *lv)
{
    uint32_t l = lv->clip_blocks[0];
    uint32_t r = lv->clip_blocks[1];
    if (!l && !r) {
        lv_obj_add_flag(s_clip_banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    bool now = lv->clipping_now[0] || lv->clipping_now[1];
    const char *who = l && r ? "Both" : (l ? "Left" : "Right");
    // Each counted block is 10 ms long.
    uint32_t tenths = (l > r ? l : r) / 10;
    char detail[48];
    snprintf(detail, sizeof(detail), "%s distorted %lu.%lu s - tap to hide", who,
             (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
    set_text(s_clip_title, now ? "INPUT TOO LOUD" : "WAS TOO LOUD");
    set_text(s_clip_detail, detail);
    lv_obj_set_style_bg_color(s_clip_banner, now ? COLOR_CLIP : lv_color_hex(0x801010), 0);
    lv_obj_clear_flag(s_clip_banner, LV_OBJ_FLAG_HIDDEN);
}

static void update_status(const audio_levels_t *lv, const rec_status_t *rec)
{
    char text[64];
    lv_color_t color = COLOR_OK;

    if (rec->state == REC_RECORDING) {
        snprintf(text, sizeof(text), "REC  %02lu:%02lu:%02lu", (unsigned long)(rec->elapsed_s / 3600),
                 (unsigned long)(rec->elapsed_s / 60 % 60), (unsigned long)(rec->elapsed_s % 60));
        color = COLOR_CLIP;
    } else if (rec->state == REC_STARTING) {
        snprintf(text, sizeof(text), "STARTING...");
        color = COLOR_WARN;
    } else if (rec->state == REC_STOPPING) {
        snprintf(text, sizeof(text), "SAVING...");
        color = COLOR_WARN;
    } else if (!lv->signal_ok) {
        snprintf(text, sizeof(text), "NO ADC SIGNAL");
        color = COLOR_CLIP;
    } else {
        snprintf(text, sizeof(text), "READY");
    }
    set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, color, 0);

    lv_color_t info_color = COLOR_DIM;
    if (rec->state == REC_RECORDING || rec->state == REC_STOPPING) {
        if (!lv->signal_ok) {
            snprintf(text, sizeof(text), "NO ADC SIGNAL - recording silence");
            info_color = COLOR_CLIP;
        } else if (rec->markers) {
            snprintf(text, sizeof(text), "%s   %lu marks", rec->file_name, (unsigned long)rec->markers);
        } else {
            snprintf(text, sizeof(text), "%s", rec->file_name);
        }
    } else if (rec->message[0]) {
        snprintf(text, sizeof(text), "%s", rec->message);
        info_color = COLOR_HOT;
    } else if (!lv->signal_ok) {
        snprintf(text, sizeof(text), "check LRCK / BCK / DATA wires");
    } else if (rec->file_name[0]) {
        snprintf(text, sizeof(text), "saved %s", rec->file_name);
    } else if (lv->measured_rate_hz) {
        snprintf(text, sizeof(text), "input %lu Hz, 24 bit, stereo", (unsigned long)lv->measured_rate_hz);
    } else {
        snprintf(text, sizeof(text), "measuring input...");
    }
    set_text(s_info, text);
    lv_obj_set_style_text_color(s_info, info_color, 0);

    // Bottom line: lost audio beats everything else, because it means the
    // recording has gaps.
    lv_color_t card_color = COLOR_DIM;
    char left[16];
    format_duration(left, sizeof(left), rec->remaining_s);
    if (rec->dropped_frames) {
        uint32_t tenths = rec->dropped_frames / (AUDIO_SAMPLE_RATE / 10);
        snprintf(text, sizeof(text), "AUDIO LOST %lu.%lu s (slow card)", (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10));
        card_color = COLOR_CLIP;
    } else if (rec->state == REC_RECORDING || rec->state == REC_STOPPING) {
        snprintf(text, sizeof(text), "%s left   buffer %u%%", left, rec->buffer_pct);
    } else if (storage_state() == STORAGE_READY) {
        uint64_t total = 0, free_bytes = 0;
        if (storage_get_space(&total, &free_bytes)) {
            snprintf(text, sizeof(text), "SD %lu.%lu GB free   %s",
                     (unsigned long)(free_bytes / 1000000000ULL),
                     (unsigned long)(free_bytes % 1000000000ULL / 100000000ULL), left);
        } else {
            snprintf(text, sizeof(text), "SD card: reading...");
        }
    } else if (storage_state() == STORAGE_UNSUPPORTED_FS) {
        snprintf(text, sizeof(text), "SD CARD NOT FORMATTED");
        card_color = COLOR_HOT;
    } else {
        snprintf(text, sizeof(text), "NO SD CARD");
        card_color = COLOR_HOT;
    }
    set_text(s_card_label, text);
    lv_obj_set_style_text_color(s_card_label, card_color, 0);

    if (!s_shown_state_valid || rec->state != s_shown_state) {
        s_shown_state = rec->state;
        s_shown_state_valid = true;
        bool recording = rec->state == REC_RECORDING;
        bool busy = rec->state == REC_STARTING || rec->state == REC_STOPPING;
        lv_label_set_text(s_rec_btn_label, recording || rec->state == REC_STOPPING ? "STOP" : "REC");
        lv_obj_set_style_bg_color(s_rec_btn, recording ? COLOR_BUTTON : COLOR_REC, 0);
        if (busy) {
            lv_obj_add_state(s_rec_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_rec_btn, LV_STATE_DISABLED);
        }
        if (recording) {
            lv_obj_clear_state(s_mark_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_mark_btn, LV_STATE_DISABLED);
        }
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    audio_levels_t lv;
    audio_in_get_levels(&lv);
    rec_status_t rec;
    recorder_get_status(&rec);
    int64_t now = esp_timer_get_time();

    update_meters(&lv, now);
    update_clip_banner(&lv);
    update_status(&lv, &rec);

    // The traces are the most expensive thing on screen; 10 per second is
    // plenty for a shape display.
    static int16_t frames[AUDIO_SCOPE_FRAMES * AUDIO_CHANNELS];
    static bool had_signal = true;
    if (++s_scope_tick >= SCOPE_PERIOD_TICKS) {
        s_scope_tick = 0;
        bool have = audio_in_get_scope(frames);
        if (have || had_signal) {
            for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
                scope_draw(ch, have ? frames : NULL, (level_t)s_bar_level[ch]);
                lv_obj_invalidate(s_canvas[ch]);
            }
        }
        had_signal = have;
    }

    // A locked recorder is usually left alone, so stop lighting the room.
    int backlight = s_locked && lv_disp_get_inactive_time(NULL) > DIM_AFTER_MS ? BACKLIGHT_DIM
                                                                                : BACKLIGHT_ON;
    if (backlight != s_backlight) {
        s_backlight = backlight;
        board_set_backlight(backlight);
    }
}

static void set_locked(bool locked)
{
    s_locked = locked;
    if (locked) {
        lv_label_set_text(s_lock_label, "LOCKED\nhold 2 s to unlock");
        lv_obj_clear_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void clip_clicked_cb(lv_event_t *e)
{
    audio_in_clear_clip();
}

static void rec_clicked_cb(lv_event_t *e)
{
    rec_status_t rec;
    recorder_get_status(&rec);
    if (rec.state == REC_IDLE) {
        recorder_start();
    } else if (rec.state == REC_RECORDING) {
        recorder_stop();
    }
}

static void mark_clicked_cb(lv_event_t *e)
{
    recorder_add_marker();
}

static void lock_clicked_cb(lv_event_t *e)
{
    set_locked(true);
}

static void lock_overlay_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int64_t now = esp_timer_get_time();
    if (code == LV_EVENT_PRESSED) {
        s_unlock_press_us = now;
        lv_label_set_text(s_lock_label, "LOCKED\nkeep holding...");
    } else if (code == LV_EVENT_PRESSING) {
        if (s_unlock_press_us && now - s_unlock_press_us >= UNLOCK_HOLD_US) {
            s_unlock_press_us = 0;
            set_locked(false);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_unlock_press_us = 0;
        lv_label_set_text(s_lock_label, "LOCKED\nhold 2 s to unlock");
    }
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

static lv_obj_t *make_button(lv_obj_t *parent, int x, int w, lv_color_t color, const char *text,
                             lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, 270);
    lv_obj_set_size(btn, w, 46);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (label_out) {
        *label_out = label;
    }
    return btn;
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

static void make_clip_banner(lv_obj_t *scr)
{
    s_clip_banner = lv_obj_create(scr);
    lv_obj_remove_style_all(s_clip_banner);
    lv_obj_set_size(s_clip_banner, BOARD_LCD_H_RES, 46);
    lv_obj_set_pos(s_clip_banner, 0, 220);
    lv_obj_set_style_bg_color(s_clip_banner, COLOR_CLIP, 0);
    lv_obj_set_style_bg_opa(s_clip_banner, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_clip_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_clip_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_clip_banner, clip_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_clip_title = make_label(s_clip_banner, &lv_font_montserrat_20, lv_color_hex(0xFFFFFF), 0, 2, "");
    lv_obj_align(s_clip_title, LV_ALIGN_TOP_MID, 0, 2);
    s_clip_detail = make_label(s_clip_banner, &lv_font_montserrat_14, lv_color_hex(0xFFFFFF), 0, 26, "");
    lv_obj_align(s_clip_detail, LV_ALIGN_TOP_MID, 0, 26);
}

static void make_lock_overlay(lv_obj_t *scr)
{
    // Covers the whole screen and swallows every touch while locked.
    s_lock_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_lock_overlay);
    lv_obj_set_size(s_lock_overlay, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_pos(s_lock_overlay, 0, 0);
    lv_obj_add_flag(s_lock_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_lock_overlay, lock_overlay_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *panel = lv_obj_create(s_lock_overlay);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, BOARD_LCD_H_RES, 52);
    lv_obj_set_pos(panel, 0, 268);
    lv_obj_set_style_bg_color(panel, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    s_lock_label = lv_label_create(panel);
    lv_obj_set_style_text_font(s_lock_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lock_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_align(s_lock_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lock_label, "LOCKED\nhold 2 s to unlock");
    lv_obj_center(s_lock_label);
}

void ui_record_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_status = make_label(scr, &lv_font_montserrat_20, COLOR_OK, 4, 2, "STARTING");
    s_info = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 4, 27, "");

    make_channel(scr, 0, "L", 46, 184);
    make_channel(scr, 1, "R", 112, 204);

    // Scale marks under the meters: the two thresholds and full scale.
    make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 20, 221, "-60");
    make_label(scr, &lv_font_montserrat_14, COLOR_WARN, 128, 221, "-12");
    make_label(scr, &lv_font_montserrat_14, COLOR_HOT, 158, 221, "0 dB");

    s_card_label = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, 4, 244, "");

    make_clip_banner(scr);

    s_rec_btn = make_button(scr, 4, 80, COLOR_REC, "REC", rec_clicked_cb, &s_rec_btn_label);
    s_mark_btn = make_button(scr, 88, 72, COLOR_BUTTON, "MARK", mark_clicked_cb, NULL);
    make_button(scr, 164, 72, COLOR_BUTTON, "LOCK", lock_clicked_cb, NULL);

    make_lock_overlay(scr);

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    lvgl_port_unlock();
}
