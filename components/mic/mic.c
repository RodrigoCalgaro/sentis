#include "mic.h"
#include "board_config.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mic";

// INMP441: salida de 24 bits, MSB-first, dentro de un slot de 32 bits (Philips).
// Con L/R a GND transmite en el slot izquierdo. Configuramos I2S1 en modo mono
// 32-bit, así el driver nos entrega una sola muestra int32 por frame.
//
// Conversión a int16: los 24 bits útiles están en [31:8]. >>16 daría escala
// exacta, pero el INMP441 es de nivel bajo (-26 dBFS @ 94 dB SPL), así que
// usamos un shift menor (ganancia 2^(16-shift)) con saturación.
#define MIC_GAIN_SHIFT  11   // ganancia x16 (+24 dB); 14 = x4, 16 = x1

static mic_data_cb_t     s_data_cb = NULL;
static TaskHandle_t      s_task    = NULL;
static i2s_chan_handle_t s_rx      = NULL;
static int32_t           s_raw_buf [MIC_CHUNK_SAMPLES];
static int16_t           s_mono_buf[MIC_CHUNK_SAMPLES];

static void mic_task(void *arg)
{
    ESP_LOGI(TAG, "capture task started — %d samples/chunk @ %d Hz mono",
             MIC_CHUNK_SAMPLES, MIC_SAMPLE_RATE);

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx, s_raw_buf, sizeof(s_raw_buf),
                                         &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read != sizeof(s_raw_buf)) {
            ESP_LOGW(TAG, "i2s read: %s (%u bytes)", esp_err_to_name(ret), (unsigned)bytes_read);
            continue;
        }

        for (int i = 0; i < MIC_CHUNK_SAMPLES; i++) {
            int32_t v = s_raw_buf[i] >> MIC_GAIN_SHIFT;
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < INT16_MIN) v = INT16_MIN;
            s_mono_buf[i] = (int16_t)v;
        }

        s_data_cb(s_mono_buf, MIC_CHUNK_SAMPLES);
    }
}

esp_err_t mic_init(mic_data_cb_t data_cb)
{
    if (data_cb == NULL) return ESP_ERR_INVALID_ARG;
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;

    // I2S1 como master RX: el ESP genera BCLK y WS, el INMP441 sólo responde.
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(BOARD_MIC_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = MIC_CHUNK_SAMPLES;
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_MIC_SCK_GPIO,
            .ws   = BOARD_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = BOARD_MIC_SD_GPIO,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // L/R del INMP441 a GND

    ret = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (ret == ESP_OK) ret = i2s_channel_enable(s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S1 RX init: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return ret;
    }

    s_data_cb = data_cb;

    // Pineada al core 0 junto con lidar_task — separada a propósito del core 1
    // (vision_task/ocr_task), que corre inferencia pesada de OCR/visión y
    // puede monopolizarlo por tramos largos. Ver crash de watchdog documentado
    // en main/sentis.c (proximity_task).
    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 6, &s_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        s_task = NULL;
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialized — INMP441 I2S%d SCK=%d WS=%d SD=%d",
             BOARD_MIC_I2S_NUM, BOARD_MIC_SCK_GPIO, BOARD_MIC_WS_GPIO, BOARD_MIC_SD_GPIO);
    return ESP_OK;
}

void mic_deinit(void)
{
    if (s_task == NULL) return;
    vTaskDelete(s_task);
    s_task = NULL;
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx      = NULL;
    s_data_cb = NULL;
    ESP_LOGI(TAG, "deinitialized");
}

bool mic_is_running(void)
{
    return s_task != NULL;
}
