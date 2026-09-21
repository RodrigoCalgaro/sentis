#include "wifi.h"

#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static volatile int s_connected_stations = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT) {
        return;
    }
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        s_connected_stations++;
        ESP_LOGI(TAG, "celular conectado " MACSTR " (AID=%d, total=%d)",
                 MAC2STR(event->mac), event->aid, s_connected_stations);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        if (s_connected_stations > 0) {
            s_connected_stations--;
        }
        ESP_LOGI(TAG, "celular desconectado " MACSTR " (AID=%d, reason=%d, total=%d)",
                 MAC2STR(event->mac), event->aid, event->reason, s_connected_stations);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS sin espacio/version vieja, borrando y reintentando");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            return ret;
        }
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t wifi_init(void)
{
    esp_err_t ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap fallo");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);

    // esp_hosted pulsa el reset del C6 en GPIO54 como parte de esp_wifi_init()
    // (confirmado en su fuente: eh_host_port_reset_slave(), en
    // managed_components/espressif__esp_hosted/port/os/idf/src/
    // eh_host_port_power.c) y NUNCA libera el pin despues — queda como
    // salida digital forzando un nivel fijo. Ese es el MISMO pin que
    // vision_init() usa despues para el XCLK de la camara (ver
    // board_config.h) — confirmado en hardware (2026-09-18) que sin este
    // reset explicito la camara se queda sin frames ("frame timeout"
    // indefinido) apenas arranca. gpio_reset_pin() lo vuelve al estado
    // default (IOMUX, flotante) para que vision_init() lo pueda reclamar
    // limpio. Se hace pase lo que pase con esp_wifi_init() (el pulso de
    // reset ya ocurrio de cualquier forma).
    gpio_reset_pin((gpio_num_t)BOARD_CAM_XCLK_GPIO);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init fallo (co-procesador C6/esp_hosted no responde?): %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_handler_instance_register fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = CONFIG_WIFI_SSID,
            .ssid_len = strlen(CONFIG_WIFI_SSID),
            .channel = CONFIG_WIFI_CHANNEL,
            .password = CONFIG_WIFI_PASSWORD,
            .max_connection = CONFIG_WIFI_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    if (strlen(CONFIG_WIFI_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SoftAP arriba — SSID:%s canal:%d IP:192.168.4.1",
             CONFIG_WIFI_SSID, CONFIG_WIFI_CHANNEL);
    return ESP_OK;
}

bool wifi_is_client_connected(void)
{
    return s_connected_stations > 0;
}
