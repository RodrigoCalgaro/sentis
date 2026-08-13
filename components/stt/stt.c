#include "stt.h"
#include "mic.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "stt";

// =============================================================================
// Tabla de comandos — editar aquí para agregar o cambiar frases reconocidas.
// El ID es arbitrario (entero > 0); el texto es la frase en inglés que el
// modelo MultiNet espera (palabras sueltas o frases cortas, inglés americano).
// =============================================================================
static const struct {
    int         id;
    const char *phrase;
} s_commands[] = {
    {1, "stop"},
    {2, "turn left"},
    {3, "turn right"},
    {4, "alert on"},
    {5, "help"},
    {6, "start reading"},
    {7, "stop reading"},
};

static srmodel_list_t      *s_models    = NULL;
static esp_mn_iface_t      *s_multinet  = NULL;
static model_iface_data_t  *s_mn_model  = NULL;
static stt_result_cb_t      s_result_cb = NULL;
static bool                 s_initialized = false;

esp_err_t stt_init(stt_result_cb_t result_cb)
{
    if (result_cb == NULL) return ESP_ERR_INVALID_ARG;
    if (s_initialized)     return ESP_ERR_INVALID_STATE;

    s_result_cb = result_cb;

    // -------------------------------------------------------------------------
    // Cargar lista de modelos desde la microSD (/sdcard/sr_model/).
    // Requiere que storage_init() haya completado antes de llamar a stt_init().
    // Si la tarjeta no está insertada o el directorio no existe, retorna NULL.
    // -------------------------------------------------------------------------
    s_models = esp_srmodel_init("/sdcard/sr_model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — "
                      "verificar que la SD esta insertada y que "
                      "/sdcard/sr_model/mn7_en/ y /sdcard/sr_model/fst/ existen");
        return ESP_FAIL;
    }

    // -------------------------------------------------------------------------
    // MultiNet — reconocimiento de comandos en inglés (mn7_en).
    // esp_srmodel_filter busca el primer modelo cuyo nombre comienza con
    // ESP_MN_PREFIX dentro del directorio cargado.
    // -------------------------------------------------------------------------
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, "en");
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "no se encontro modelo MultiNet en la particion");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "modelo MultiNet: %s", mn_name);

    s_multinet = esp_mn_handle_from_name(mn_name);
    s_mn_model = s_multinet->create(mn_name, 6000);
    if (s_mn_model == NULL) {
        ESP_LOGE(TAG, "multinet->create fallido");
        return ESP_FAIL;
    }

    // -------------------------------------------------------------------------
    // Registrar comandos. esp_mn_commands_alloc() limpia la lista previa;
    // luego se agregan las frases y se llama a commands_update para
    // recompilar el grafo de reconocimiento.
    // -------------------------------------------------------------------------
    esp_mn_commands_alloc(s_multinet, s_mn_model);
    int n_commands = (int)(sizeof(s_commands) / sizeof(s_commands[0]));
    for (int i = 0; i < n_commands; i++) {
        esp_err_t ret = esp_mn_commands_add(s_commands[i].id, s_commands[i].phrase);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "no se pudo agregar comando '%s': %s",
                     s_commands[i].phrase, esp_err_to_name(ret));
        }
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err != NULL) {
        for (int i = 0; i < err->num; i++) {
            ESP_LOGW(TAG, "comando rechazado: '%s'", err->phrases[i]);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "listo — %d comandos registrados", n_commands);
    return ESP_OK;
}

void stt_feed(const int16_t *mono_samples, size_t count)
{
    if (!s_initialized) return;
    if (count != MIC_CHUNK_SAMPLES) return;  // ESP-SR espera exactamente 480

    // detect() consume el buffer y retorna el estado del reconocedor.
    // La función NO bloquea — procesa sincrónicamente el chunk.
    esp_mn_state_t state = s_multinet->detect(s_mn_model, (int16_t *)mono_samples);

    if (state == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *results = s_multinet->get_results(s_mn_model);
        if (results == NULL || results->num == 0) return;

        stt_result_t result = {
            .command_id = results->command_id[0],
        };
        // Buscar el texto legible en la tabla de comandos en lugar de usar
        // results->string, que contiene la transcripción fonética del modelo
        // (e.g. " STnP" en lugar de "stop").
        const char *label = results->string;  // fallback: fonética si no matchea
        int n = (int)(sizeof(s_commands) / sizeof(s_commands[0]));
        for (int i = 0; i < n; i++) {
            if (s_commands[i].id == result.command_id) {
                label = s_commands[i].phrase;
                break;
            }
        }
        // snprintf trunca y termina en NUL siempre, a diferencia de strncpy
        // (que además dispara -Wstringop-truncation en builds -O2/PERF).
        // El truncamiento es intencional aquí (label puede venir de
        // results->string, hasta 255 bytes, hacia un buffer de 64) — GCC no
        // puede ver que es a propósito, de ahí el pragma puntual.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(result.text, STT_COMMAND_TEXT_MAX, "%s", label);
#pragma GCC diagnostic pop

        ESP_LOGI(TAG, "detectado: id=%d  \"%s\"  (score=%.2f)",
                 result.command_id, result.text,
                 (results->num > 0) ? results->prob[0] : 0.0f);

        s_result_cb(&result);
    }
}

void stt_deinit(void)
{
    if (!s_initialized) return;
    if (s_multinet && s_mn_model) {
        s_multinet->destroy(s_mn_model);
        s_mn_model = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "deinitializado");
}
