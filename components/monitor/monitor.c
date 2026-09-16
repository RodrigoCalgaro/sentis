#include "monitor.h"
#include "vision.h"
#include "sdkconfig.h"

#if CONFIG_MONITOR_ENABLED

#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <string.h>

#define TAG "monitor"

#define STT_TEXT_MAX      64

// El monitor es solo una vista previa de desarrollo — no necesita la
// resolución completa del sensor. Con scale=2 (640x480) medido en hardware
// quedaban ~5.8 MB libres de PSRAM tras el init, ~330 KB por debajo de lo que
// pide OCR (raw 2.34 MB + rgb888 3.51 MB = 5.86 MB) — insuficiente por poco.
// scale=4 (320x240) baja el buffer de captura a ~150 KB, liberando ~480 KB
// más de margen real (ver vision_copy_display_frame_scaled()).
#define MONITOR_SCALE      4
#define MONITOR_W          (VISION_FRAME_W / MONITOR_SCALE)
#define MONITOR_H          (VISION_FRAME_H / MONITOR_SCALE)
#define MONITOR_FRAME_SZ   ((size_t)MONITOR_W * MONITOR_H * 2)

#define JPEG_OUT_SIZE     (32 * 1024)

// Puerto USB OTG nativo (full-speed, ~1 MB/s útiles) y quality=30:
//   JPEG 320×240 color (YUV420) sale bien por debajo de JPEG_OUT_SIZE, con
//   margen de sobra sobre FRAME_INTERVAL_MS.
//
// FRAME_INTERVAL_MS en 3000 (no 1000) porque el encoder JPEG por hardware es
// un motor DMA propio que compite por el bus de PSRAM con el DMA del ISP —
// visto en hardware como "ISP: fifo overflow" mientras monitor_task corre su
// ciclo de copiar+codificar. Espaciar los ciclos reduce cuán seguido chocan;
// no es una solución de fondo (requeriría tocar el buffering CSI/ISP en
// vision.c), pero para una vista previa de desarrollo alcanza con bajar la
// frecuencia de colisión.
#define FRAME_INTERVAL_MS  3000

// =============================================================================
// Protocolo de framing — dos tipos de paquete:
//
//   Tipo JPEG (camera frame):
//     [4]  magic: AB CD EF 01
//     [4]  tamano JPEG en bytes (uint32 LE)
//     [1]  zona: 0=NONE 1=LEFT 2=CENTER 3=RIGHT
//     [1]  pad
//     [N]  datos JPEG
//
//   Tipo TEXT (STT result):
//     [4]  magic: AB CD EF 02
//     [4]  longitud del texto (uint32 LE, incluyendo NUL terminal)
//     [1]  pad
//     [1]  pad
//     [N]  texto UTF-8 terminado en NUL
//
// Ver tools/monitor_viewer.py para el parser en Python.
// =============================================================================
static const uint8_t MAGIC_JPEG[4] = {0xAB, 0xCD, 0xEF, 0x01};
static const uint8_t MAGIC_TEXT[4] = {0xAB, 0xCD, 0xEF, 0x02};

static jpeg_encoder_handle_t  s_enc        = NULL;
static uint8_t               *s_frame_copy = NULL;
static uint8_t               *s_jpeg_out   = NULL;
static uint8_t                s_row_tmp[MONITOR_W * 2];  // 1 fila RGB565, en RAM interna

// El sensor está montado rotado respecto a la vista del usuario — vision.c
// entrega el frame en la orientación cruda del sensor. El pipeline de OCR ya
// corrige esto (espejado vertical, ver ocr_preprocess.c); acá se aplica la
// misma corrección mano a mano pero sin desempacar a RGB888, para que lo que
// se vea en monitor_viewer.py sea representativo de lo que recibe el modelo,
// no el crudo del sensor. Al ser un espejado vertical puro (intercambio de
// filas completas, sin tocar el orden de los píxeles dentro de cada fila) se
// hace in-place con una sola fila de scratch — evita un segundo buffer de
// PSRAM del tamaño de s_frame_copy, que ya de por sí se redujo a
// MONITOR_W×MONITOR_H para competir menos con los buffers de captura de OCR.
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

// Último texto STT recibido, protegido por mutex ligero.
static SemaphoreHandle_t s_stt_mutex  = NULL;
static char              s_stt_text[STT_TEXT_MAX] = {0};
static bool              s_stt_dirty = false;  // hay texto nuevo no enviado aún

// usb_serial_jtag_write_bytes() encola en un ring buffer sin conversión de
// saltos de línea (binary-safe para JPEG). A diferencia de una UART física,
// acá SÍ puede fallar en encolar si el host no está leyendo (no hay "cable
// al vacío"): con timeout acotado, si no entra devuelve 0 en vez de colgar
// monitor_task — se descarta el frame y se reintenta en el próximo ciclo.
static bool write_chunk(const void *data, size_t len, TickType_t timeout)
{
    return usb_serial_jtag_write_bytes(data, len, timeout) == (int)len;
}

static void send_jpeg_frame(uint32_t jpeg_size, uint8_t zone)
{
    uint8_t hdr[10];
    hdr[0] = MAGIC_JPEG[0]; hdr[1] = MAGIC_JPEG[1];
    hdr[2] = MAGIC_JPEG[2]; hdr[3] = MAGIC_JPEG[3];
    hdr[4] = (jpeg_size >>  0) & 0xFF;
    hdr[5] = (jpeg_size >>  8) & 0xFF;
    hdr[6] = (jpeg_size >> 16) & 0xFF;
    hdr[7] = (jpeg_size >> 24) & 0xFF;
    hdr[8] = zone;
    hdr[9] = 0;

    if (!write_chunk(hdr, sizeof(hdr), pdMS_TO_TICKS(200))) {
        return;
    }
    write_chunk(s_jpeg_out, jpeg_size, pdMS_TO_TICKS(500));
}

static void send_text_frame(const char *text)
{
    uint32_t len = (uint32_t)(strlen(text) + 1);  // incluye NUL
    uint8_t hdr[10];
    hdr[0] = MAGIC_TEXT[0]; hdr[1] = MAGIC_TEXT[1];
    hdr[2] = MAGIC_TEXT[2]; hdr[3] = MAGIC_TEXT[3];
    hdr[4] = (len >>  0) & 0xFF;
    hdr[5] = (len >>  8) & 0xFF;
    hdr[6] = (len >> 16) & 0xFF;
    hdr[7] = (len >> 24) & 0xFF;
    hdr[8] = 0;
    hdr[9] = 0;

    if (!write_chunk(hdr, sizeof(hdr), pdMS_TO_TICKS(200))) {
        return;
    }
    write_chunk(text, len, pdMS_TO_TICKS(200));
}

static void monitor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "monitor activo (puerto USB OTG) — python tools/monitor_viewer.py <COMX>");

    // mirror_rgb565_inplace mantiene las dimensiones de MONITOR_W/H (solo
    // espeja, no transpone), así que el encoder usa el mismo ancho/alto que el
    // frame reducido.
    const jpeg_encode_cfg_t enc_cfg = {
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,  // más compresión, prioriza banda del enlace
        .image_quality = 30,   // ~3 KB/frame a 115200 baud → ~3 fps
        .width         = MONITOR_W,
        .height        = MONITOR_H,
    };

    while (1) {
        // Sin host conectado al puerto OTG, ni copiar ni codificar tiene sentido
        // (usb_serial_jtag_write_bytes fallaría igual) — se evita el gasto de CPU/PSRAM.
        if (!usb_serial_jtag_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
            continue;
        }

        if (!vision_copy_display_frame_scaled(s_frame_copy, MONITOR_FRAME_SZ, MONITOR_SCALE)) {
            vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
            continue;
        }
        mirror_rgb565_inplace(s_frame_copy, MONITOR_W, MONITOR_H);

        uint32_t out_size = 0;
        esp_err_t ret = jpeg_encoder_process(s_enc, &enc_cfg,
                                             s_frame_copy, MONITOR_FRAME_SZ,
                                             s_jpeg_out, JPEG_OUT_SIZE,
                                             &out_size);
        if (ret != ESP_OK || out_size == 0) {
            ESP_LOGW(TAG, "jpeg encode: %s (out=%"PRIu32")", esp_err_to_name(ret), out_size);
            vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
            continue;
        }

        ESP_LOGD(TAG, "%"PRIu32" bytes zona=%d", out_size, (int)vision_get_obstacle_side());
        send_jpeg_frame(out_size, (uint8_t)vision_get_obstacle_side());

        // Enviar texto STT si hay uno nuevo pendiente.
        if (xSemaphoreTake(s_stt_mutex, 0) == pdTRUE) {
            if (s_stt_dirty) {
                send_text_frame(s_stt_text);
                s_stt_dirty = false;
            }
            xSemaphoreGive(s_stt_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
    }
}

esp_err_t monitor_init(void)
{
    s_stt_mutex     = xSemaphoreCreateMutex();
    s_frame_copy    = heap_caps_malloc(MONITOR_FRAME_SZ,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_jpeg_out      = heap_caps_malloc(JPEG_OUT_SIZE,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_frame_copy || !s_jpeg_out) {
        ESP_LOGE(TAG, "sin PSRAM para buffers (%u + %d bytes)",
                 (unsigned)MONITOR_FRAME_SZ, JPEG_OUT_SIZE);
        return ESP_ERR_NO_MEM;
    }

    const jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 500,
    };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&eng_cfg, &s_enc));

    // Instalar el driver del puerto USB OTG nativo (USB_SERIAL_JTAG), separado
    // del UART0 que usa la consola de logs / idf.py monitor. TX de 64 KB para
    // encolar un frame JPEG completo sin bloquear monitor_task.
    usb_serial_jtag_driver_config_t usj_cfg = {
        .tx_buffer_size = 64 * 1024,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_cfg));

    xTaskCreate(monitor_task, "monitor", 4096, NULL, 2, NULL);
    return ESP_OK;
}

void monitor_set_stt_text(const char *text)
{
    if (text == NULL || s_stt_mutex == NULL) return;
    if (xSemaphoreTake(s_stt_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(s_stt_text, text, STT_TEXT_MAX - 1);
        s_stt_text[STT_TEXT_MAX - 1] = '\0';
        s_stt_dirty = true;
        xSemaphoreGive(s_stt_mutex);
    }
}

#else

esp_err_t monitor_init(void)       { return ESP_OK; }
void      monitor_set_stt_text(const char *text) { (void)text; }

#endif  // CONFIG_MONITOR_ENABLED
