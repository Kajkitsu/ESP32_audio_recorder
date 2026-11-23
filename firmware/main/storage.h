// microSD card on the shared SPI bus.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STORAGE_MOUNT_POINT "/sdcard"

typedef enum {
    STORAGE_NO_CARD,        // no card, or it does not answer
    STORAGE_UNSUPPORTED_FS, // card answers but holds no FAT filesystem (e.g. exFAT)
    STORAGE_READY,
} storage_state_t;

// Tries to mount the card. Call after board_bus_init() and, at boot, before
// board_display_init().
storage_state_t storage_mount(void);

storage_state_t storage_state(void);

// Capacity and free space in bytes. Returns false when no card is mounted.
bool storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes);
