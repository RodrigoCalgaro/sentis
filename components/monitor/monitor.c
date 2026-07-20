#include "monitor.h"
#include "vision.h"
#include "sdkconfig.h"

#if CONFIG_MONITOR_ENABLED

#include "esp_log.h"
#include "driver/uart.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

#define TAG "monitor"

#define JPEG_OUT_SIZE     (200 * 1024)

// A 921600 baud (~92 KB/s útiles) y quality=30:
//   JPEG 800×640 gray ≈ 15–25 KB → tiempo transmisión ≈ 160–270 ms
//   Con FRAME_INTERVAL_MS=500 hay margen suficiente incluso en el peor caso.
#define FRAME_INTERVAL_MS  1000

// =============================================================================
// Protocolo de framing — igual que antes, documentado en tools/monitor_viewer.py
// =============================================================================
static const uint8_t MAGIC[4] = {0xAB, 0xCD, 0xEF, 0x01};

static jpeg_encoder_handle_t  s_enc        = NULL;
static uint8_t               *s_frame_copy = NULL;
static uint8_t               *s_jpeg_out   = NULL;

// uart_write_bytes() escribe al FIFO del UART sin conversión de saltos de línea,
// por lo que es binary-safe para los datos JPEG.
static void send_frame(uint32_t jpeg_size, uint8_t zone)
{
    uint8_t hdr[10];
    hdr[0] = MAGIC[0]; hdr[1] = MAGIC[1];
    hdr[2] = MAGIC[2]; hdr[3] = MAGIC[3];
    hdr[4] = (jpeg_size >>  0) & 0xFF;
    hdr[5] = (jpeg_size >>  8) & 0xFF;
    hdr[6] = (jpeg_size >> 16) & 0xFF;
    hdr[7] = (jpeg_size >> 24) & 0xFF;
    hdr[8] = zone;
    hdr[9] = 0;

    uart_write_bytes(UART_NUM_0, hdr, sizeof(hdr));
    uart_write_bytes(UART_NUM_0, s_jpeg_out, (int)jpeg_size);
}

static void monitor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "monitor activo (%d baud) — python tools/monitor_viewer.py <COMX>",
             CONFIG_ESP_CONSOLE_UART_BAUDRATE);

    const jpeg_encode_cfg_t enc_cfg = {
        .src_type      = JPEG_ENCODE_IN_FORMAT_GRAY,
        .sub_sample    = JPEG_DOWN_SAMPLING_GRAY,
        .image_quality = 30,   // ~3 KB/frame a 115200 baud → ~3 fps
        .width         = VISION_FRAME_W,
        .height        = VISION_FRAME_H,
    };

    while (1) {
        if (!vision_copy_display_frame(s_frame_copy, VISION_FRAME_SZ)) {
            vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
            continue;
        }

        uint32_t out_size = 0;
        esp_err_t ret = jpeg_encoder_process(s_enc, &enc_cfg,
                                             s_frame_copy, VISION_FRAME_SZ,
                                             s_jpeg_out, JPEG_OUT_SIZE,
                                             &out_size);
        if (ret != ESP_OK || out_size == 0) {
            ESP_LOGW(TAG, "jpeg encode: %s (out=%"PRIu32")", esp_err_to_name(ret), out_size);
            vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
            continue;
        }

        ESP_LOGD(TAG, "%"PRIu32" bytes zona=%d", out_size, (int)vision_get_obstacle_side());
        send_frame(out_size, (uint8_t)vision_get_obstacle_side());
        vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
    }
}

esp_err_t monitor_init(void)
{
    s_frame_copy = heap_caps_malloc(VISION_FRAME_SZ,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_jpeg_out   = heap_caps_malloc(JPEG_OUT_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_frame_copy || !s_jpeg_out) {
        ESP_LOGE(TAG, "sin PSRAM para buffers (%d + %d bytes)",
                 VISION_FRAME_SZ, JPEG_OUT_SIZE);
        return ESP_ERR_NO_MEM;
    }

    const jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 500,
    };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&eng_cfg, &s_enc));

    // Instalar el driver UART0 con TX buffer de 64 KB para que uart_write_bytes()
    // pueda encolar un frame JPEG completo sin bloquearse. El driver coexiste con
    // el console VFS: ESP_LOG* sigue usando la ruta directa al FIFO mientras que
    // uart_write_bytes() usa el ring buffer con drenado por interrupción.
    uart_driver_install(UART_NUM_0,
                        512,        // RX buffer
                        64 * 1024,  // TX buffer — suficiente para un frame JPEG
                        0, NULL, 0);

    xTaskCreate(monitor_task, "monitor", 4096, NULL, 2, NULL);
    return ESP_OK;
}

#else

esp_err_t monitor_init(void) { return ESP_OK; }

#endif  // CONFIG_MONITOR_ENABLED
