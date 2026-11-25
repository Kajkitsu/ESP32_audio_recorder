#include "board.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

// Height of the LVGL draw buffer in lines. One buffer is one SPI transfer, so
// anything up to this height reaches the panel in one piece: an SD card write
// on the shared bus cannot land in the middle of it. Tall enough for one
// oscilloscope trace.
#define DRAW_BUF_LINES 66

#define TOUCH_POLL_MS  10

static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;

// Touch state, written by touch_task and read by LVGL, guarded by s_touch_lock.
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_touch_down;
static bool s_touch_press_pending; // a press began since LVGL last looked
static lv_point_t s_touch_point;

esp_err_t board_bus_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = BOARD_PIN_SPI_MOSI,
        .miso_io_num = BOARD_PIN_SPI_MISO,
        .sclk_io_num = BOARD_PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * DRAW_BUF_LINES * 2 + 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    // Adding the device drives the LCD chip select high, which the SD card
    // needs while it is being initialised on the same bus.
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = BOARD_PIN_LCD_CS,
        .dc_gpio_num = BOARD_PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io, &s_lcd_io),
        TAG, "lcd io");
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    return ledc_channel_config(&channel);
}

void board_set_backlight(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Polls the touch controller from its own task. The LVGL task can spend a
// long time waiting for the SD card to release the shared SPI bus, and a short
// tap would be over before it read the controller.
static void touch_task(void *arg)
{
    esp_lcd_touch_handle_t tp = arg;
    bool was_down = false;
    int misses = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));

        esp_lcd_touch_point_data_t point;
        uint8_t count = 0;
        bool touched = esp_lcd_touch_read_data(tp) == ESP_OK &&
                       esp_lcd_touch_get_data(tp, &point, &count, 1) == ESP_OK && count > 0;
        // One empty or failed read does not end a press: that would turn a
        // single tap into two, and the second one could stop a recording.
        misses = touched ? 0 : misses + 1;
        bool down = touched || (was_down && misses < 2);

        portENTER_CRITICAL(&s_touch_lock);
        s_touch_down = down;
        if (touched) {
            s_touch_point.x = point.x;
            s_touch_point.y = point.y;
        }
        if (down && !was_down) {
            s_touch_press_pending = true;
        }
        portEXIT_CRITICAL(&s_touch_lock);
        was_down = down;
    }
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    portENTER_CRITICAL(&s_touch_lock);
    bool pending = s_touch_press_pending;
    s_touch_press_pending = false;
    bool down = s_touch_down;
    data->point = s_touch_point;
    portEXIT_CRITICAL(&s_touch_lock);

    // A tap that began and ended between two reads is still reported: the
    // press now, the release in a second read straight after.
    data->state = pending || down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = pending && !down;
}

static esp_err_t touch_init(lv_disp_t *disp)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "i2c bus");

    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_panel_io_handle_t tp_io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io), TAG, "touch io");

    // The board wires neither reset nor (usably) interrupt, so the controller
    // is polled, as in Waveshare's own example.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_touch_handle_t tp;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp), TAG, "touch");

    static lv_indev_drv_t indev_drv;
    lvgl_port_lock(0);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "lvgl touch");

    // Above the LVGL task, which busy-waits while a flush is pending.
    BaseType_t ok = xTaskCreate(touch_task, "touch", 3072, tp, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t board_display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    // GPIO0 is the panel reset on the schematic but also the BOOT button, so
    // it is left alone and the panel is reset by command only.
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = false,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl display");

    // A dead touch controller must not stop the recorder from starting.
    esp_err_t err = touch_init(disp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
    }

    board_set_backlight(80);
    return ESP_OK;
}
