#include "settings.h"

#include <inttypes.h>
#include <stdbool.h>

#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE   "settings"
#define NVS_KEY_VOLUME  "vol_pct"
#define NVS_KEY_WARN    "warn_mm"
#define NVS_KEY_ALERT   "alert_mm"

static volatile int      s_volume_percent    = SETTINGS_DEFAULT_VOLUME_PERCENT;
static volatile uint16_t s_proximity_warn_mm  = SETTINGS_DEFAULT_PROXIMITY_WARN_MM;
static volatile uint16_t s_proximity_alert_mm = SETTINGS_DEFAULT_PROXIMITY_ALERT_MM;

static nvs_handle_t s_nvs;
static bool         s_nvs_ok;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void persist_i32(const char *key, int32_t value)
{
    if (!s_nvs_ok) {
        return;
    }
    esp_err_t ret = nvs_set_i32(s_nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(s_nvs);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "no se pudo guardar %s=%" PRId32 " en NVS: %s", key, value, esp_err_to_name(ret));
    }
}

esp_err_t settings_init(void)
{
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open fallo (%s) — uso defaults en memoria, sin persistencia", esp_err_to_name(ret));
        s_nvs_ok = false;
        return ret;
    }
    s_nvs_ok = true;

    int32_t v;
    if (nvs_get_i32(s_nvs, NVS_KEY_VOLUME, &v) == ESP_OK) {
        s_volume_percent = clamp_int((int)v, 0, 100);
    }
    if (nvs_get_i32(s_nvs, NVS_KEY_WARN, &v) == ESP_OK) {
        s_proximity_warn_mm = (uint16_t)clamp_int((int)v, SETTINGS_PROXIMITY_WARN_MIN_MM, SETTINGS_PROXIMITY_WARN_MAX_MM);
    }
    if (nvs_get_i32(s_nvs, NVS_KEY_ALERT, &v) == ESP_OK) {
        s_proximity_alert_mm = (uint16_t)clamp_int((int)v, SETTINGS_PROXIMITY_ALERT_MIN_MM, SETTINGS_PROXIMITY_ALERT_MAX_MM);
    }

    ESP_LOGI(TAG, "cargado: volumen=%d%% warn=%umm alert=%umm",
             s_volume_percent, s_proximity_warn_mm, s_proximity_alert_mm);
    return ESP_OK;
}

int settings_get_volume_percent(void)
{
    return s_volume_percent;
}

uint16_t settings_get_proximity_warn_mm(void)
{
    return s_proximity_warn_mm;
}

uint16_t settings_get_proximity_alert_mm(void)
{
    return s_proximity_alert_mm;
}

// Tarea de un solo disparo para el tono de validación de volumen —
// audio_test_tone() bloquea ~400ms escribiendo directo al I2S, y
// settings_set_volume_percent() se llama desde la tarea de recepción de
// components/link (link_receive_loop): bloquearla ahí retrasaría cualquier
// otro mensaje entrante mientras suena el tono, así que se dispara aparte.
static void volume_test_tone_task(void *arg)
{
    audio_test_tone();
    vTaskDelete(NULL);
}

esp_err_t settings_set_volume_percent(int vol_percent)
{
    vol_percent = clamp_int(vol_percent, 0, 100);
    s_volume_percent = vol_percent;
    persist_i32(NVS_KEY_VOLUME, vol_percent);

    esp_err_t ret = audio_set_volume(vol_percent);

    if (xTaskCreate(volume_test_tone_task, "vol_test_tone", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no se pudo crear la tarea del tono de prueba, sigo sin el");
    }

    return ret;
}

esp_err_t settings_set_proximity_warn_mm(int warn_mm)
{
    warn_mm = clamp_int(warn_mm, SETTINGS_PROXIMITY_WARN_MIN_MM, SETTINGS_PROXIMITY_WARN_MAX_MM);
    if (warn_mm - (int)s_proximity_alert_mm < SETTINGS_PROXIMITY_MIN_GAP_MM) {
        ESP_LOGW(TAG, "rechazado warn_mm=%d: queda a menos de %dmm del alert_mm actual (%u)",
                 warn_mm, SETTINGS_PROXIMITY_MIN_GAP_MM, s_proximity_alert_mm);
        return ESP_ERR_INVALID_ARG;
    }

    s_proximity_warn_mm = (uint16_t)warn_mm;
    persist_i32(NVS_KEY_WARN, warn_mm);
    return ESP_OK;
}

esp_err_t settings_set_proximity_alert_mm(int alert_mm)
{
    alert_mm = clamp_int(alert_mm, SETTINGS_PROXIMITY_ALERT_MIN_MM, SETTINGS_PROXIMITY_ALERT_MAX_MM);
    if ((int)s_proximity_warn_mm - alert_mm < SETTINGS_PROXIMITY_MIN_GAP_MM) {
        ESP_LOGW(TAG, "rechazado alert_mm=%d: queda a menos de %dmm del warn_mm actual (%u)",
                 alert_mm, SETTINGS_PROXIMITY_MIN_GAP_MM, s_proximity_warn_mm);
        return ESP_ERR_INVALID_ARG;
    }

    s_proximity_alert_mm = (uint16_t)alert_mm;
    persist_i32(NVS_KEY_ALERT, alert_mm);
    return ESP_OK;
}
