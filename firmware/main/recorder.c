#include "recorder.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_in.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "storage.h"

static const char *TAG = "recorder";

#define REC_DIR          STORAGE_MOUNT_POINT "/REC"
#define BYTES_PER_FRAME  6
#define BYTES_PER_SEC    (AUDIO_SAMPLE_RATE * BYTES_PER_FRAME)

// Audio waits here for the card. 4 MiB holds about 14 s.
#define RING_BYTES       (4u * 1024 * 1024)
#define RING_MASK        (RING_BYTES - 1)

// The header is padded to one sector and every write is a whole number of
// sectors and of frames, so FatFs never has to patch partial sectors.
#define HEADER_BYTES     512
#define CHUNK_BYTES      32256
// A new file is started after an hour, well below the 4 GB FAT32 limit.
#ifndef SPLIT_CHUNKS
#define SPLIT_CHUNKS     (3600u * BYTES_PER_SEC / CHUNK_BYTES)
#endif

#define SYNC_PERIOD_US   (5 * 1000000LL)
#define MIN_FREE_BYTES   (16ull * 1024 * 1024)

typedef enum { CMD_START, CMD_STOP, CMD_MARK } cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint64_t frame; // capture position of a marker
} cmd_t;

static QueueHandle_t s_cmds;
static uint8_t *s_ring;
static uint8_t *s_chunk;

// Ring indices run freely and are masked on use. The audio task owns s_head,
// the writer task owns s_tail.
static volatile uint32_t s_head;
static volatile uint32_t s_tail;
static volatile bool s_capturing;
static volatile uint32_t s_dropped_frames;
static volatile uint64_t s_frames_captured;

// Writer task state
static int s_fd = -1;
static uint32_t s_rec_no;
static uint32_t s_part;
static uint32_t s_part_chunks;
static uint64_t s_part_bytes;
static uint64_t s_total_bytes;
static uint64_t s_part_start_frame;
static uint64_t s_free_at_start;
static uint32_t s_marker_count;
static int64_t s_last_sync_us;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static rec_status_t s_status;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = v >> 24;
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// RIFF header with a JUNK chunk that pads it to exactly one sector.
static void build_header(uint8_t *h, uint32_t data_bytes)
{
    memset(h, 0, HEADER_BYTES);
    memcpy(h, "RIFF", 4);
    put32(h + 4, data_bytes == UINT32_MAX ? UINT32_MAX : data_bytes + HEADER_BYTES - 8);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "JUNK", 4);
    put32(h + 16, HEADER_BYTES - 52);
    memcpy(h + 480, "fmt ", 4);
    put32(h + 484, 16);
    put16(h + 488, 1); // PCM
    put16(h + 490, AUDIO_CHANNELS);
    put32(h + 492, AUDIO_SAMPLE_RATE);
    put32(h + 496, BYTES_PER_SEC);
    put16(h + 500, BYTES_PER_FRAME);
    put16(h + 502, 24);
    memcpy(h + 504, "data", 4);
    put32(h + 508, data_bytes);
}

static bool write_sizes(int fd, uint32_t data_bytes)
{
    uint8_t v[4];
    put32(v, data_bytes + HEADER_BYTES - 8);
    if (lseek(fd, 4, SEEK_SET) < 0 || write(fd, v, 4) != 4) {
        return false;
    }
    put32(v, data_bytes);
    if (lseek(fd, 508, SEEK_SET) < 0 || write(fd, v, 4) != 4) {
        return false;
    }
    return true;
}

// A power cut leaves the last file with placeholder sizes in its header.
static void repair_headers(void)
{
    DIR *dir = opendir(REC_DIR);
    if (!dir) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcasecmp(e->d_name + len - 4, ".wav") != 0) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof(path), REC_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size < HEADER_BYTES) {
            continue;
        }
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            continue;
        }
        uint8_t h[HEADER_BYTES];
        uint32_t data = (uint32_t)(st.st_size - HEADER_BYTES);
        data -= data % BYTES_PER_FRAME;
        if (read(fd, h, sizeof(h)) == sizeof(h) && memcmp(h, "RIFF", 4) == 0 &&
            memcmp(h + 12, "JUNK", 4) == 0 && memcmp(h + 504, "data", 4) == 0 &&
            get32(h + 508) != data) {
            ESP_LOGW(TAG, "repairing header of %s (%lu bytes of audio)", e->d_name,
                     (unsigned long)data);
            write_sizes(fd, data);
            fsync(fd);
        }
        close(fd);
    }
    closedir(dir);
}

static void set_state(rec_state_t state)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.state = state;
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_message(const char *msg)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.message, msg, sizeof(s_status.message));
    portEXIT_CRITICAL(&s_status_lock);
}

static void part_name(char *out, size_t len, const char *ext)
{
    if (s_part == 1) {
        snprintf(out, len, "REC_%04lu.%s", (unsigned long)s_rec_no, ext);
    } else {
        snprintf(out, len, "REC_%04lu_%lu.%s", (unsigned long)s_rec_no, (unsigned long)s_part, ext);
    }
}

static bool open_part(void)
{
    char name[40];
    char path[80];
    part_name(name, sizeof(name), "wav");
    snprintf(path, sizeof(path), REC_DIR "/%s", name);

    s_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "cannot create %s: errno %d", path, errno);
        return false;
    }
    // Placeholder sizes let most players read to the end of the file if the
    // header is never finalised.
    uint8_t h[HEADER_BYTES];
    build_header(h, UINT32_MAX);
    if (write(s_fd, h, sizeof(h)) != sizeof(h)) {
        close(s_fd);
        s_fd = -1;
        return false;
    }
    s_part_bytes = 0;
    s_part_chunks = 0;
    s_part_start_frame = s_total_bytes / BYTES_PER_FRAME;
    s_last_sync_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.file_name, name, sizeof(s_status.file_name));
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "recording to %s", path);
    return true;
}

static bool close_part(void)
{
    if (s_fd < 0) {
        return true;
    }
    bool ok = write_sizes(s_fd, (uint32_t)s_part_bytes);
    ok = fsync(s_fd) == 0 && ok;
    ok = close(s_fd) == 0 && ok;
    s_fd = -1;
    return ok;
}

static uint32_t next_recording_number(void)
{
    uint32_t n = 1;
    nvs_handle_t nvs;
    bool have_nvs = nvs_open("recorder", NVS_READWRITE, &nvs) == ESP_OK;
    if (have_nvs) {
        nvs_get_u32(nvs, "next", &n);
    }
    // Never reuse a number that already has a file, whatever the counter says.
    for (;; n++) {
        char path[80];
        struct stat st;
        snprintf(path, sizeof(path), REC_DIR "/REC_%04lu.wav", (unsigned long)n);
        if (stat(path, &st) != 0) {
            break;
        }
    }
    if (have_nvs) {
        nvs_set_u32(nvs, "next", n + 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return n;
}

static void finish(const char *message)
{
    s_capturing = false;
    set_state(REC_STOPPING);
    if (!close_part() && !message[0]) {
        message = "WRITE ERROR";
    }
    set_message(message);
    storage_update_space();
    set_state(REC_IDLE);
    ESP_LOGI(TAG, "stopped%s%s", message[0] ? ": " : "", message);
}

static void handle_start(void)
{
    set_state(REC_STARTING);
    set_message("");

    if (storage_state() != STORAGE_READY) {
        storage_mount();
    }
    if (storage_state() != STORAGE_READY) {
        set_message(storage_state() == STORAGE_UNSUPPORTED_FS ? "CARD NOT FORMATTED" : "NO SD CARD");
        set_state(REC_IDLE);
        return;
    }
    storage_update_space();
    uint64_t total = 0;
    storage_get_space(&total, &s_free_at_start);
    if (s_free_at_start < MIN_FREE_BYTES + CHUNK_BYTES) {
        set_message("CARD FULL");
        set_state(REC_IDLE);
        return;
    }

    mkdir(REC_DIR, 0775);
    s_rec_no = next_recording_number();
    s_part = 1;
    s_total_bytes = 0;
    s_marker_count = 0;
    if (!open_part()) {
        set_message("CANNOT CREATE FILE");
        set_state(REC_IDLE);
        return;
    }

    // Start from an empty buffer; the audio task is not feeding yet.
    s_tail = s_head;
    s_dropped_frames = 0;
    s_frames_captured = 0;
    portENTER_CRITICAL(&s_status_lock);
    s_status.markers = 0;
    portEXIT_CRITICAL(&s_status_lock);
    s_capturing = true;
    set_state(REC_RECORDING);
}

static void handle_mark(uint64_t frame)
{
    if (s_fd < 0) {
        return;
    }
    char name[40];
    char path[80];
    part_name(name, sizeof(name), "txt");
    snprintf(path, sizeof(path), REC_DIR "/%s", name);
    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    // Audacity label track: start, end, text. Time is relative to this file.
    double t = frame > s_part_start_frame
                   ? (double)(frame - s_part_start_frame) / AUDIO_SAMPLE_RATE
                   : 0.0;
    s_marker_count++;
    fprintf(f, "%.3f\t%.3f\tM%lu\n", t, t, (unsigned long)s_marker_count);
    fclose(f);

    portENTER_CRITICAL(&s_status_lock);
    s_status.markers = s_marker_count;
    portEXIT_CRITICAL(&s_status_lock);
}

// Moves n bytes from the ring to the card. Returns false on a write error.
static bool write_from_ring(uint32_t n)
{
    uint32_t tail = s_tail;
    uint32_t off = tail & RING_MASK;
    uint32_t first = RING_BYTES - off < n ? RING_BYTES - off : n;
    // The SD driver is slow when handed PSRAM, so go through internal RAM.
    memcpy(s_chunk, s_ring + off, first);
    memcpy(s_chunk + first, s_ring, n - first);

    if (write(s_fd, s_chunk, n) != (ssize_t)n) {
        ESP_LOGE(TAG, "write failed: errno %d", errno);
        return false;
    }
    s_tail = tail + n;
    s_part_bytes += n;
    s_total_bytes += n;
    return true;
}

static void pump(void)
{
    while (s_head - s_tail >= CHUNK_BYTES) {
        if (!write_from_ring(CHUNK_BYTES)) {
            finish("WRITE ERROR");
            return;
        }
        if (++s_part_chunks >= SPLIT_CHUNKS) {
            if (!close_part()) {
                finish("WRITE ERROR");
                return;
            }
            s_part++;
            if (!open_part()) {
                finish("CANNOT CREATE FILE");
                return;
            }
        }
        uint64_t used = s_total_bytes + (uint64_t)s_part * HEADER_BYTES;
        if (used + MIN_FREE_BYTES > s_free_at_start) {
            finish("CARD FULL");
            return;
        }
    }
    int64_t now = esp_timer_get_time();
    if (now - s_last_sync_us >= SYNC_PERIOD_US) {
        s_last_sync_us = now;
        if (fsync(s_fd) != 0) {
            finish("WRITE ERROR");
        }
    }
}

static void handle_stop(void)
{
    // Stop the feed first, then write out whatever is still buffered.
    s_capturing = false;
    set_state(REC_STOPPING);
    bool ok = true;
    while (ok && s_head != s_tail) {
        uint32_t left = s_head - s_tail;
        ok = write_from_ring(left > CHUNK_BYTES ? CHUNK_BYTES : left);
    }
    finish(ok ? "" : "WRITE ERROR");
}

static void writer_task(void *arg)
{
    if (storage_state() == STORAGE_READY) {
        repair_headers();
        storage_update_space();
    }

    int idle_ticks = 0;
    for (;;) {
        bool recording = s_fd >= 0;
        cmd_t cmd;
        if (xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(recording ? 20 : 500))) {
            if (cmd.type == CMD_START && !recording) {
                handle_start();
            } else if (cmd.type == CMD_STOP && recording) {
                handle_stop();
            } else if (cmd.type == CMD_MARK && recording) {
                handle_mark(cmd.frame);
            }
            continue;
        }
        if (recording) {
            pump();
        } else if (++idle_ticks >= 60) {
            // Keeps the free-space figure current, e.g. after files were deleted.
            idle_ticks = 0;
            storage_update_space();
        }
    }
}

esp_err_t recorder_init(void)
{
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_chunk = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    if (!s_ring || !s_chunk || !s_cmds) {
        return ESP_ERR_NO_MEM;
    }
    // Above the LVGL task, so drawing never holds up the card.
    BaseType_t ok = xTaskCreatePinnedToCore(writer_task, "rec_writer", 8192, NULL, 6, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void send(cmd_type_t type, uint64_t frame)
{
    const cmd_t cmd = {.type = type, .frame = frame};
    xQueueSend(s_cmds, &cmd, 0);
}

void recorder_start(void)
{
    send(CMD_START, 0);
}

void recorder_stop(void)
{
    send(CMD_STOP, 0);
}

void recorder_add_marker(void)
{
    send(CMD_MARK, s_frames_captured);
}

void recorder_get_status(rec_status_t *out)
{
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);

    out->dropped_frames = s_dropped_frames;
    out->buffer_pct = (uint8_t)((uint64_t)(s_head - s_tail) * 100 / RING_BYTES);

    uint64_t total = 0, free_bytes = 0;
    bool have_space = storage_get_space(&total, &free_bytes);
    if (out->state == REC_RECORDING || out->state == REC_STOPPING) {
        uint64_t captured = s_frames_captured;
        out->elapsed_s = (uint32_t)(captured / AUDIO_SAMPLE_RATE);
        uint64_t used = captured * BYTES_PER_FRAME;
        free_bytes = s_free_at_start > used ? s_free_at_start - used : 0;
        have_space = true;
    } else {
        out->elapsed_s = 0;
    }
    out->remaining_s = have_space && free_bytes > MIN_FREE_BYTES
                           ? (uint32_t)((free_bytes - MIN_FREE_BYTES) / BYTES_PER_SEC)
                           : 0;
}

void recorder_feed(const int32_t *frames, int n_frames)
{
    if (!s_capturing) {
        return;
    }
    uint32_t head = s_head;
    uint32_t need = (uint32_t)n_frames * BYTES_PER_FRAME;
    if (RING_BYTES - (head - s_tail) < need) {
        s_dropped_frames += n_frames;
        return;
    }
    for (int i = 0; i < n_frames * AUDIO_CHANNELS; i++) {
        // Bits 31..8 of the slot are the sample; WAV wants it little-endian.
        uint32_t v = (uint32_t)frames[i];
        s_ring[head++ & RING_MASK] = (uint8_t)(v >> 8);
        s_ring[head++ & RING_MASK] = (uint8_t)(v >> 16);
        s_ring[head++ & RING_MASK] = (uint8_t)(v >> 24);
    }
    s_head = head;
    s_frames_captured += n_frames;
}
