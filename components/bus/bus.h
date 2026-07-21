#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// Initialize the shared I2C bus (idempotent — safe to call from multiple components).
// The bus is created once on the first call; subsequent calls return ESP_OK immediately.
esp_err_t bus_i2c_init(void);

// Return the shared I2C master bus handle.
// Returns NULL if bus_i2c_init() was never called successfully.
i2c_master_bus_handle_t bus_get_i2c(void);
