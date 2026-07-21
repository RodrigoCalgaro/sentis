#pragma once

#include "esp_err.h"
#include <stdbool.h>

// All files on the SD card are accessible at this VFS prefix (e.g. "/sdcard/alert.wav").
#define STORAGE_MOUNT_POINT  "/sdcard"

// Initialize SDMMC host and mount the FAT filesystem on the microSD card.
// The card must be formatted as FAT32 (or FAT16 for cards < 2 GB).
// Non-fatal: returns ESP_OK only if the card was found and mounted successfully.
esp_err_t storage_init(void);

bool storage_is_mounted(void);
