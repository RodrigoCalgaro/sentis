#include "bus.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "bus";

static i2c_master_bus_handle_t s_i2c = NULL;

esp_err_t bus_i2c_init(void)
{
    if (s_i2c) return ESP_OK;

    const i2c_master_bus_config_t cfg = {
        .i2c_port          = BOARD_I2C_NUM,
        .sda_io_num        = BOARD_I2C_SDA_GPIO,
        .scl_io_num        = BOARD_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&cfg, &s_i2c);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C%d ready (SDA=GPIO%d SCL=GPIO%d)",
             BOARD_I2C_NUM, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    return ESP_OK;
}

i2c_master_bus_handle_t bus_get_i2c(void)
{
    return s_i2c;
}
