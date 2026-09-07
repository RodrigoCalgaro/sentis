#include "ocr.h"
#include "ocr_preprocess.h"
#include "vision.h"
#include "tts.h"
#include "pp_ocr_v6.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>
#include <filesystem>

static const char *TAG = "ocr";

#ifndef CONFIG_BSP_SD_MOUNT_POINT
#define CONFIG_BSP_SD_MOUNT_POINT "/sdcard"
#endif

// -----------------------------------------------------------------------------
// model_file_looks_valid — chequeo defensivo previo a instanciar Det/Rec.
//
// pp_ocr_v6::Det::Det()/Rec::Rec() (vendorizado, managed_components/
// espressif__pp_ocr_v6/pp_ocr_v6.cpp) llaman dl::Model::minimize() de forma
// incondicional apenas construyen el modelo, SIN chequear si la carga del
// .espdl falló. Si el archivo no existe o está vacío/corrupto,
// dl::Model queda en un estado inválido y minimize() crashea el equipo entero
// (Guru Meditation / Load access fault) — un simple archivo faltante en la SD
// reinicia el dispositivo en un loop infinito en cada boot, tumbando también
// haptics/LiDAR/audio que dependen de que app_main() termine de arrancar.
//
// Mitigación de nuestro lado ya que no podemos parchear el código vendorizado
// de forma sostenible: validar existencia + tamaño + firma mágica de cada
// .espdl ANTES de construir Det/Rec, replicando la misma verificación que
// hace fbs_loader.cpp (get_model_format) para no instanciar nada si va a
// fallar. Si algo no está bien, OCR queda deshabilitado pero el resto del
// sistema arranca normal — mismo patrón "no fatal" que tts_init/stt_init.
// -----------------------------------------------------------------------------
static bool model_file_looks_valid(const char *filename)
{
    auto path = std::filesystem::path(CONFIG_BSP_SD_MOUNT_POINT) / CONFIG_PP_OCR_V6_MODEL_SDCARD_DIR / filename;

    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "no se pudo abrir %s", path.c_str());
        return false;
    }

    char magic[5] = {0};
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    if (n != 4) {
        ESP_LOGE(TAG, "%s: archivo vacío o truncado", path.c_str());
        return false;
    }

    static const char *kValidMagics[] = {"EDL1", "EDL2", "PDL1", "PDL2", "PDL3"};
    for (const char *m : kValidMagics) {
        if (strcmp(magic, m) == 0) return true;
    }
    ESP_LOGE(TAG, "%s: firma inválida \"%s\" (¿copia incompleta a la SD?)", path.c_str(), magic);
    return false;
}

// ocr_preprocess_rgb565_to_rgb888 ya no hace downsampling (el ISP entrega
// color real por píxel, ya no hace falta promediar bloques para debayer) —
// con el modo RAW10 binning actual la corrección de orientación es un
// espejado horizontal puro (ver comentario en ocr_preprocess.h), así que las
// dimensiones coinciden con el frame crudo (VISION_FRAME_W x VISION_FRAME_H).
static constexpr int kImgW = VISION_FRAME_W; // 1280
static constexpr int kImgH = VISION_FRAME_H; // 960

// Mismo umbral que usa pp_ocr_v6::PPOCRV6 internamente para descartar
// reconocimientos de baja confianza (no usamos PPOCRV6 directamente porque
// no expone puntos de interrupción entre cajas — ver nota en ocr_task).
static constexpr float kRecScoreThreshold = pp_ocr_v6::PPOCRV6::default_rec_score_threshold;

static pp_ocr_v6::Det *s_det = nullptr;
static pp_ocr_v6::Rec *s_rec = nullptr;
static SemaphoreHandle_t s_start_sem = nullptr;
static volatile bool s_stop_req = false;
static volatile bool s_reading = false;
static bool s_initialized = false;

// -----------------------------------------------------------------------------
// ocr_task — tarea de vida larga (creada una sola vez en ocr_init). Queda
// bloqueada en s_start_sem hasta que ocr_reading_start() la despierta; al
// terminar un ciclo de lectura vuelve a esperar, nunca se destruye.
//
// "stop reading" (s_stop_req) se revisa entre cada etapa del pipeline. La
// única etapa que NO se interrumpe es una locución de tts_speak() ya en
// curso — no existe cancelación en el componente tts, así que el peor caso
// es terminar la frase actual antes de detenerse.
// -----------------------------------------------------------------------------
static void ocr_task(void *arg)
{
    uint8_t *raw = (uint8_t *)heap_caps_malloc(VISION_FRAME_SZ, MALLOC_CAP_SPIRAM);
    uint8_t *rgb = (uint8_t *)heap_caps_malloc((size_t)kImgW * kImgH * 3, MALLOC_CAP_SPIRAM);
    if (!raw || !rgb) {
        ESP_LOGE(TAG, "sin memoria PSRAM para buffers de captura (raw=%p rgb=%p)", raw, rgb);
        vTaskDelete(NULL);
        return;
    }

    dl::image::img_t img = {
        .data = rgb,
        .width = (uint16_t)kImgW,
        .height = (uint16_t)kImgH,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };

    while (true) {
        xSemaphoreTake(s_start_sem, portMAX_DELAY);
        s_reading = true;
        s_stop_req = false;
        // Pausar la heurística de posición de vision_task durante la lectura:
        // su resultado no se usa (proximity_task ignora vision mientras hay
        // lectura OCR activa) y libera CPU/PSRAM para Det/Rec — ver
        // vision_set_analysis_paused() en vision.h.
        vision_set_analysis_paused(true);
        ESP_LOGI(TAG, "lectura iniciada");

        while (!s_stop_req) {
            if (!vision_copy_display_frame(raw, VISION_FRAME_SZ)) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            ocr_preprocess_rgb565_to_rgb888(raw, VISION_FRAME_W, VISION_FRAME_H, rgb);
            if (s_stop_req) break;

            // DIAG TEMPORAL — investigando un abort() en __cxa_allocate_exception
            // visto tras ~46s de lectura continua, sin que "cajas detectadas"
            // (más abajo) llegue a imprimirse ni una vez. Sospecha: una sola
            // llamada a s_det->run() (vendorizado) nunca retorna, en vez de
            // acumularse por muchos ciclos. Este "iniciando" antes del run()
            // confirma si el problema está DENTRO de esa llamada. Quitar una
            // vez confirmada/resuelta la causa.
            ESP_LOGW(TAG, "DIAG: iniciando s_det->run()");
            auto boxes = s_det->run(img);
            ESP_LOGI(TAG, "%d caja(s) de texto detectadas", (int)boxes.size());
            // DIAG TEMPORAL — investigando un abort() en __cxa_allocate_exception
            // (throw sin CONFIG_COMPILER_CXX_EXCEPTIONS) visto tras ~46s de
            // lectura continua. Sospecha: fragmentación de PSRAM por los
            // vector<Box>/std::string reservados en cada ciclo. Este log deja
            // ver si la PSRAM libre/bloque contiguo cae progresivamente ciclo
            // a ciclo. Quitar una vez confirmada/resuelta la causa.
            ESP_LOGW(TAG, "DIAG: PSRAM libre=%u bloque_max=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            if (s_stop_req) break;

            for (const auto &box : boxes) {
                if (s_stop_req) break;
                float score = 0.0f;
                std::string text = s_rec->run(img, box, &score);
                if (s_stop_req) break;
                // Log incondicional (diagnóstico): así se ve si el reconocedor
                // realmente falla (texto vacío/score bajo) o si el filtro de
                // umbral está descartando algo que sí valdría la pena hablar.
                ESP_LOGI(TAG, "recognizer: score=%.2f texto=\"%s\"", score, text.c_str());
                if (score >= kRecScoreThreshold && !text.empty()) {
                    tts_speak(text.c_str());
                }
                // El wrapper vendorizado PPOCRV6::run() cede CPU entre cajas
                // (pp_ocr_v6.cpp) — este loop lo reimplementa manualmente para
                // tener puntos de corte por s_stop_req (ver comentario arriba)
                // y ese yield se había perdido. Sin él, una imagen con varias
                // cajas de texto puede monopolizar la CPU sin ceder por
                // suficiente tiempo como para inanir IDLE (ver crash de
                // watchdog documentado en sentis.c/proximity_task).
                vTaskDelay(1);
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

    // Validar los .espdl ANTES de tocar Det/Rec — ver comentario de
    // model_file_looks_valid(): si esto se salta y el archivo falta o está
    // roto, el equipo entero crashea y reinicia en loop en cada boot.
    if (!model_file_looks_valid("pp_ocr_v6_det_s8.espdl") ||
        !model_file_looks_valid("pp_ocr_v6_rec_s16.espdl")) {
        ESP_LOGE(TAG,
                 "modelos pp_ocr_v6 no encontrados/inválidos en %s/%s — "
                 "copiar sdcard_files/models/ a la SD (ver sdcard_files/README.md). "
                 "OCR deshabilitado, el resto del sistema sigue funcionando.",
                 CONFIG_BSP_SD_MOUNT_POINT, CONFIG_PP_OCR_V6_MODEL_SDCARD_DIR);
        return ESP_ERR_NOT_FOUND;
    }

    s_start_sem = xSemaphoreCreateBinary();
    if (!s_start_sem) return ESP_ERR_NO_MEM;

    s_det = new pp_ocr_v6::Det();
    s_rec = new pp_ocr_v6::Rec();

    // Pineada al core 1 junto con vision_task — separada a propósito del core 0
    // (mic_task/lidar_task). Ver nota de pinning en components/mic/mic.c.
    BaseType_t ok = xTaskCreatePinnedToCore(ocr_task, "ocr_reading", 32768, NULL, 3, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "no hay memoria para la tarea de lectura");
        delete s_det;
        delete s_rec;
        s_det = nullptr;
        s_rec = nullptr;
        vSemaphoreDelete(s_start_sem);
        s_start_sem = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "listo — modelos pp_ocr_v6 cargados, esperando \"start reading\"");
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
