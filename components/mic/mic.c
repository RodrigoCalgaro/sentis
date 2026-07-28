#include "mic.h"
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "mic";

// Cada chunk ESP-SR es 480 samples mono.
// El ES8311 ADC entrega estéreo entrelazado: [L0, R0, L1, R1, ...].
// Necesitamos 480 frames estéreo = 960 int16 del bus I2S.
#define STEREO_BUF_SAMPLES  (MIC_CHUNK_SAMPLES * 2)

static mic_data_cb_t s_data_cb    = NULL;
static TaskHandle_t  s_task       = NULL;
static int16_t       s_stereo_buf[STEREO_BUF_SAMPLES];
static int16_t       s_mono_buf  [MIC_CHUNK_SAMPLES];

static void mic_task(void *arg)
{
    ESP_LOGI(TAG, "capture task started — %d samples/chunk @ %d Hz mono",
             MIC_CHUNK_SAMPLES, MIC_SAMPLE_RATE);

    while (1) {
        // Leer un chunk estéreo completo del I2S RX (ES8311 ADC).
        // portMAX_DELAY: bloqueamos hasta tener los datos, sin quemar CPU.
        esp_err_t ret = audio_read_pcm_stereo(s_stereo_buf,
                                               STEREO_BUF_SAMPLES,
                                               portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s read: %s", esp_err_to_name(ret));
            continue;
        }

        // Downmix estéreo → mono: promedio de canal L y canal R.
        // El ES8311 envía [L0, R0, L1, R1, ...].
        for (int i = 0; i < MIC_CHUNK_SAMPLES; i++) {
            int32_t avg = ((int32_t)s_stereo_buf[i * 2] +
                           (int32_t)s_stereo_buf[i * 2 + 1]) >> 1;
            s_mono_buf[i] = (int16_t)avg;
        }

        s_data_cb(s_mono_buf, MIC_CHUNK_SAMPLES);
    }
}

esp_err_t mic_init(mic_data_cb_t data_cb)
{
    if (data_cb == NULL) return ESP_ERR_INVALID_ARG;
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;

    s_data_cb = data_cb;

    BaseType_t ok = xTaskCreate(mic_task, "mic", 4096, NULL, 6, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialized — ES8311 ADC I2S0-RX");
    return ESP_OK;
}

void mic_deinit(void)
{
    if (s_task == NULL) return;
    vTaskDelete(s_task);
    s_task    = NULL;
    s_data_cb = NULL;
    ESP_LOGI(TAG, "deinitialized");
}

bool mic_is_running(void)
{
    return s_task != NULL;
}
