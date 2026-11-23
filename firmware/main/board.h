// Waveshare ESP32-S3-Touch-LCD-2: pins and on-board peripherals.
#pragma once

#include "driver/spi_master.h"
#include "esp_err.h"
#include "lvgl.h"

// SPI bus shared by the LCD and the microSD slot
#define BOARD_SPI_HOST      SPI2_HOST
#define BOARD_PIN_SPI_MOSI  38
#define BOARD_PIN_SPI_SCLK  39
#define BOARD_PIN_SPI_MISO  40
#define BOARD_PIN_LCD_CS    45
#define BOARD_PIN_LCD_DC    42
#define BOARD_PIN_LCD_BL    1
#define BOARD_PIN_SD_CS     41

// Touch controller (I2C bus shared with the IMU)
#define BOARD_PIN_I2C_SDA   48
#define BOARD_PIN_I2C_SCL   47

// PCM1808 module, which is the I2S master
#define BOARD_PIN_I2S_LRCK  2
#define BOARD_PIN_I2S_BCK   4
#define BOARD_PIN_I2S_DATA  6

#define BOARD_LCD_H_RES     240
#define BOARD_LCD_V_RES     320

// Step 1: SPI bus and the LCD's bus device. Sends nothing to the LCD yet,
// because the SD card must be switched to SPI mode before any other traffic.
esp_err_t board_bus_init(void);

// Step 2 (after the SD card mount attempt): panel, backlight, LVGL, touch.
esp_err_t board_display_init(void);

void board_set_backlight(int percent);
