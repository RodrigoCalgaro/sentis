/*
 * Firmware minimo del co-procesador (ESP32-C6): expone WiFi al ESP32-P4
 * via esp_hosted sobre SDIO. No hace nada propio mas alla de inicializar
 * NVS y el event loop — el resto (WiFi SoftAP, OTA, heartbeat, version)
 * lo maneja esp_hosted internamente una vez que ESP_HOSTED_CP=y.
 *
 * Basado en el ejemplo oficial examples/ota/coprocessor_ota/cp/main/main.c
 * de espressif/esp-hosted-mcu.
 */

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "sentis_cp";

void app_main(void)
{
    ESP_LOGI(TAG, "SENTIS co-procesador (ESP32-C6) arrancando...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
}
