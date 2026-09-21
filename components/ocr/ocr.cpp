#include "ocr.h"
#include "vision.h"
#include "link.h"
#include "driver/jpeg_encode.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include <inttypes.h>

static const char *TAG = "ocr";

// -----------------------------------------------------------------------------
// Fase 2 (migración a app companion, ver sentis-stability-integration-plan.md):
// este componente ya NO corre inferencia on-device (pp_ocr_v6 se retiró por
// crashes y baja precisión — ver project_ocr_pp_ocr_v6.md). Ahora es
// exclusivamente captura + codificación JPEG por hardware + pedido/respuesta
// por components/link hacia la app Android (ML Kit hace el OCR real del lado
// del teléfono).
//
// Fase 3 (decisión del usuario 2026-09-21): el texto reconocido ya NO se
// locuta acá — la app companion lo habla con el TTS nativo de Android
// (mejor cadencia que eSpeak-NG para lectura en vivo, ver
// VoiceCommandRecognizer/OcrTextRecognizer del lado Android). El componente
// tts del ESP32 sigue activo para todo lo demás (ej. "Sentis Encendido" al
// arrancar, en main/sentis.c) — funciona standalone sin celular conectado.
// -----------------------------------------------------------------------------

// Buffer de salida JPEG — dimensionado con margen sobre lo medido en
// components/monitor/monitor.c (que a 320x240/quality=30 usa 32KB): a
// 1280x960 (16x más píxeles) y quality=60 (mejor legibilidad para el OCR del
// celular que la vista previa de desarrollo), un frame típico con texto real
// entra cómodo. Si jpeg_encoder_process no entra en este buffer, falla
// limpio (log + se salta ese frame) — no hay corrupción de memoria.
#define OCR_JPEG_OUT_SIZE   (512 * 1024)
#define OCR_REQUEST_TIMEOUT_MS  8000

static SemaphoreHandle_t s_start_sem = nullptr;
static volatile bool s_stop_req = false;
static volatile bool s_reading = false;
static bool s_initialized = false;

static jpeg_encoder_handle_t s_enc = nullptr;
static uint8_t *s_raw      = nullptr;  // RGB565, VISION_FRAME_SZ (captura directa de vision)
static uint8_t *s_jpeg_out = nullptr;  // salida JPEG codificada
static uint8_t  s_row_tmp[VISION_FRAME_W * 2];  // 1 fila RGB565, en RAM interna

// -----------------------------------------------------------------------------
// mirror_rgb565_inplace — el sensor está montado rotado respecto a la vista
// del usuario (ver components/ocr_preprocess original, ahora retirado, y el
// mismo comentario en components/monitor/monitor.c). Con el modo de captura
// actual (RAW10 binning 1280x960) la corrección es un espejado vertical puro
// — sin esto, el frame que recibe la app companion queda al revés respecto a
// lo que el usuario está mirando, y ML Kit no es invariante a esa
// orientación (mismo problema, ya documentado, que tenía pp_ocr_v6 antes de
// aplicar esta misma corrección). Si se cambia de modo de captura, volver a
// verificar con una foto real (tools/monitor_viewer.py) antes de asumir que
// esto sigue siendo correcto.
// -----------------------------------------------------------------------------
static void mirror_rgb565_inplace(uint8_t *buf, int w, int h)
{
    size_t row_bytes = (size_t)w * 2;
    for (int y = 0; y < h / 2; y++) {
        uint8_t *row_top = buf + (size_t)y * row_bytes;
        uint8_t *row_bot = buf + (size_t)(h - 1 - y) * row_bytes;
        memcpy(s_row_tmp, row_top, row_bytes);
        memcpy(row_top, row_bot, row_bytes);
        memcpy(row_bot, s_row_tmp, row_bytes);
    }
}

// -----------------------------------------------------------------------------
// ocr_task — tarea de vida larga (creada una sola vez en ocr_init). Queda
// bloqueada en s_start_sem hasta que ocr_reading_start() la despierta; al
// terminar un ciclo de lectura vuelve a esperar, nunca se destruye.
//
// "stop reading" (s_stop_req) se revisa entre cada etapa del pipeline —
// desde que la locución se mudó al celular (Fase 3), no queda ninguna etapa
// bloqueante larga de este lado; el peor caso es esperar un
// link_request_ocr_text() en curso (hasta OCR_REQUEST_TIMEOUT_MS).
// -----------------------------------------------------------------------------
static void ocr_task(void *arg)
{
    // Orden de campos igual a la declaración de jpeg_encode_cfg_t (driver/
    // jpeg_encode.h) — a diferencia de C, el compilador de C++ (-Werror,
    // -Wmissing-field-initializers) exige orden e inicialización completa
    // en un inicializador designado.
    const jpeg_encode_cfg_t enc_cfg = {
        .height        = VISION_FRAME_H,
        .width         = VISION_FRAME_W,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 60,
        .pixel_reverse = false,
    };

    while (true) {
        xSemaphoreTake(s_start_sem, portMAX_DELAY);
        s_reading = true;
        s_stop_req = false;
        // Pausar la heurística de posición de vision_task durante la lectura:
        // su resultado no se usa (proximity_task ignora vision mientras hay
        // lectura OCR activa) y libera CPU para el resto del pipeline — ver
        // vision_set_analysis_paused() en vision.h.
        vision_set_analysis_paused(true);
        ESP_LOGI(TAG, "lectura iniciada");

        while (!s_stop_req) {
            if (!vision_copy_display_frame(s_raw, VISION_FRAME_SZ)) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            mirror_rgb565_inplace(s_raw, VISION_FRAME_W, VISION_FRAME_H);
            if (s_stop_req) break;

            uint32_t jpeg_size = 0;
            esp_err_t enc_ret = jpeg_encoder_process(s_enc, &enc_cfg,
                                                       s_raw, VISION_FRAME_SZ,
                                                       s_jpeg_out, OCR_JPEG_OUT_SIZE,
                                                       &jpeg_size);
            if (enc_ret != ESP_OK || jpeg_size == 0) {
                ESP_LOGW(TAG, "jpeg encode: %s (out=%" PRIu32 ")",
                         esp_err_to_name(enc_ret), jpeg_size);
                vTaskDelay(pdMS_TO_TICKS(400));
                continue;
            }
            if (s_stop_req) break;

            char text[LINK_OCR_TEXT_MAX];
            esp_err_t ret = link_request_ocr_text(s_jpeg_out, jpeg_size, text, sizeof(text),
                                                   pdMS_TO_TICKS(OCR_REQUEST_TIMEOUT_MS));
            if (ret == ESP_OK) {
                // La app ya lo locutó con el TTS de Android (Fase 3) — acá
                // solo se loguea para diagnóstico (idf.py monitor).
                ESP_LOGI(TAG, "app respondio: \"%s\"", text);
            } else if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "sin celular conectado — pausando capturas");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            } else if (ret == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "la app no respondio a tiempo (%d ms)", OCR_REQUEST_TIMEOUT_MS);
            } else {
                ESP_LOGW(TAG, "link_request_ocr_text: %s", esp_err_to_name(ret));
            }

            vTaskDelay(pdMS_TO_TICKS(400)); // cooldown entre ciclos de captura
        }

        vision_set_analysis_paused(false);
        ESP_LOGI(TAG, "lectura detenida");
        s_reading = false;
    }
}

esp_err_t ocr_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    s_start_sem = xSemaphoreCreateBinary();
    if (!s_start_sem) return ESP_ERR_NO_MEM;

    s_raw = (uint8_t *)heap_caps_malloc(VISION_FRAME_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_jpeg_out = (uint8_t *)heap_caps_malloc(OCR_JPEG_OUT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_raw || !s_jpeg_out) {
        ESP_LOGE(TAG, "sin PSRAM para buffers (raw=%p jpeg_out=%p)", s_raw, s_jpeg_out);
        heap_caps_free(s_raw);
        heap_caps_free(s_jpeg_out);
        s_raw = nullptr;
        s_jpeg_out = nullptr;
        vSemaphoreDelete(s_start_sem);
        s_start_sem = nullptr;
        return ESP_ERR_NO_MEM;
    }

    const jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 500,
        .flags         = {},
    };
    esp_err_t enc_ret = jpeg_new_encoder_engine(&eng_cfg, &s_enc);
    if (enc_ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine: %s", esp_err_to_name(enc_ret));
        heap_caps_free(s_raw);
        heap_caps_free(s_jpeg_out);
        s_raw = nullptr;
        s_jpeg_out = nullptr;
        vSemaphoreDelete(s_start_sem);
        s_start_sem = nullptr;
        return enc_ret;
    }

    // Pineada al core 1 junto con vision_task — separada a propósito del core 0
    // (mic_task/lidar_task). Ver nota de pinning en components/mic/mic.c.
    BaseType_t ok = xTaskCreatePinnedToCore(ocr_task, "ocr_reading", 8192, NULL, 3, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "no hay memoria para la tarea de lectura");
        jpeg_del_encoder_engine(s_enc);
        s_enc = nullptr;
        heap_caps_free(s_raw);
        heap_caps_free(s_jpeg_out);
        s_raw = nullptr;
        s_jpeg_out = nullptr;
        vSemaphoreDelete(s_start_sem);
        s_start_sem = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "listo — captura+JPEG+link, esperando \"start reading\"");
    return ESP_OK;
}

void ocr_reading_start(void)
{
    if (!s_initialized || s_reading) return;
    xSemaphoreGive(s_start_sem);
}

void ocr_reading_stop(void)
{
    s_stop_req = true;
}

bool ocr_is_reading(void)
{
    return s_reading;
}
