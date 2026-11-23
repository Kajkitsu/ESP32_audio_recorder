#include "audio_in.h"
#include "board.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "storage.h"
#include "ui_record.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Order matters on the shared SPI bus: the SD card has to be put into SPI
    // mode before the first byte goes to the LCD.
    ESP_ERROR_CHECK(board_bus_init());
    storage_mount();
    ESP_ERROR_CHECK(board_display_init());

    err = audio_in_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio input failed to start: %s", esp_err_to_name(err));
    }

    ui_record_create();
}
