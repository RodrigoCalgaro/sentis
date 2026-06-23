#pragma once

#include "esp_err.h"
#include <stdint.h>

// Initialize UART and start the background streaming task.
esp_err_t lidar_init(void);

// Return the most recent distance reading in millimetres (0 until first frame).
uint16_t lidar_get_distance_mm(void);
