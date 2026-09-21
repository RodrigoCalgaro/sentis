#pragma once

#include "esp_err.h"
#include <stdbool.h>

// =============================================================================
// NOT CALLED from main/sentis.c as of Fase 2 (ver
// sentis-stability-integration-plan.md) — nothing in the active firmware
// needs the SD card anymore (STT/OCR models moved to the phone, TTS voice
// data moved to a flash partition, the boot alert.wav was dropped). Kept
// here as available infrastructure in case something needs SD storage
// again later. If so, re-check the known SDMMC-vs-esp_hosted controller
// conflict documented in project_wifi_companion_app.md before relying on
// it while the C6 radio is active — the workaround in storage.c improved
// the symptom but did not fully resolve it as of the last investigation.
// =============================================================================

// All files on the SD card are accessible at this VFS prefix (e.g. "/sdcard/alert.wav").
#define STORAGE_MOUNT_POINT  "/sdcard"

// Initialize SDMMC host and mount the FAT filesystem on the microSD card.
// The card must be formatted as FAT32 (or FAT16 for cards < 2 GB).
// Non-fatal: returns ESP_OK only if the card was found and mounted successfully.
esp_err_t storage_init(void);

bool storage_is_mounted(void);
