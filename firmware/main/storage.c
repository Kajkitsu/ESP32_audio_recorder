#include "storage.h"

#include "board.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

static storage_state_t s_state = STORAGE_NO_CARD;
static sdmmc_card_t *s_card;

storage_state_t storage_mount(void)
{
    if (s_state == STORAGE_READY) {
        return s_state;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_PIN_SD_CS;
    slot.host_id = BOARD_SPI_HOST;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 32 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err == ESP_OK) {
        s_state = STORAGE_READY;
        sdmmc_card_print_info(stdout, s_card);
    } else if (err == ESP_FAIL) {
        // The card initialised but FatFs found no filesystem it can mount.
        s_state = STORAGE_UNSUPPORTED_FS;
        ESP_LOGW(TAG, "card present, filesystem not supported");
    } else {
        s_state = STORAGE_NO_CARD;
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
    }
    return s_state;
}

storage_state_t storage_state(void)
{
    return s_state;
}

bool storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (s_state != STORAGE_READY) {
        return false;
    }
    return esp_vfs_fat_info(STORAGE_MOUNT_POINT, total_bytes, free_bytes) == ESP_OK;
}
