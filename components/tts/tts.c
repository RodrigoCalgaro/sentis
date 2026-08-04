#include "tts.h"
#include "audio.h"
#include "esp_log.h"
#include "espeak-ng/speak_lib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "tts";
static bool     s_initialized  = false;
static int      s_espeak_rate  = 22050;   // rate read from phondata at init time
static uint32_t s_resamp_phase = 0;       // persistent phase accumulator across chunks
static int16_t  s_resamp_out[2048];       // resampled output staging buffer (BSS)

// -----------------------------------------------------------------------------
// synth_callback — invocada por eSpeak-NG con cada chunk de PCM sintetizado.
//
// eSpeak genera PCM a s_espeak_rate Hz (leído de los datos binarios — 22050 Hz
// cuando los datos provienen del MSI oficial).  audio_play_pcm() espera
// AUDIO_SAMPLE_RATE (16000 Hz).  Si las tasas difieren se aplica un convertidor
// de tasa de muestra por acumulador de fase (nearest-neighbor):
//
//   phase_step = espeak_rate / audio_rate  (Q16 fixed-point)
//   Para cada muestra de salida: lee src[phase >> 16], avanza phase += step
//   Al agotar el chunk: phase -= numsamples<<16  (guarda fracción para próximo chunk)
// -----------------------------------------------------------------------------
static int synth_callback(short *wav, int numsamples, espeak_EVENT *events)
{
    if (!wav || numsamples <= 0) return 0;

    if (s_espeak_rate == AUDIO_SAMPLE_RATE) {
        audio_play_pcm((const int16_t *)wav, (size_t)numsamples);
        return 0;
    }

    // SRC: espeak_rate → AUDIO_SAMPLE_RATE via phase accumulator
    const uint32_t phase_step = ((uint32_t)s_espeak_rate * 65536U) / (uint32_t)AUDIO_SAMPLE_RATE;
    int out_count = 0;

    while (out_count < (int)(sizeof(s_resamp_out) / sizeof(s_resamp_out[0]))) {
        int in_pos = (int)(s_resamp_phase >> 16);
        if (in_pos >= numsamples) {
            s_resamp_phase -= ((uint32_t)numsamples << 16);
            break;
        }
        s_resamp_out[out_count++] = wav[in_pos];
        s_resamp_phase += phase_step;
    }

    if (out_count > 0)
        audio_play_pcm(s_resamp_out, (size_t)out_count);
    return 0;
}

// =============================================================================
// tts_init
// =============================================================================
esp_err_t tts_init(const char *data_path)
{
    s_resamp_phase = 0;

    int sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, data_path, 0);
    if (sample_rate < 0) {
        ESP_LOGE(TAG, "espeak_Initialize failed — verificar que existen los archivos "
                       "en %s (ejecutar scripts/setup_espeak.ps1)", data_path);
        return ESP_FAIL;
    }

    s_espeak_rate = sample_rate;
    if (sample_rate != AUDIO_SAMPLE_RATE) {
        ESP_LOGI(TAG, "eSpeak rate=%d Hz → resampleando a %d Hz (SRC Q16 nearest-neighbor)",
                 sample_rate, AUDIO_SAMPLE_RATE);
    }

    espeak_SetSynthCallback(synth_callback);

    espeak_ERROR err = espeak_SetVoiceByName("es");
    if (err != EE_OK) {
        ESP_LOGE(TAG, "espeak_SetVoiceByName('es') error %d — "
                       "verificar que lang/es existe en %s", err, data_path);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TTS listo — voz es (español), %d Hz", sample_rate);
    return ESP_OK;
}

// =============================================================================
// tts_speak — task wrapper
//
// eSpeak-NG usa >39 KB de stack en TranslateClause + MakePhonemeList (buffers
// locales de análisis fonético diseñados para desktop).  La tarea main no puede
// tener ese tamaño de stack durante toda su vida.  Solución: crear una tarea
// temporal con 64 KB de stack solo para la síntesis; la tarea se destruye al
// terminar y el stack se libera al heap.  tts_speak() bloquea con semáforo
// hasta que la síntesis termina, por lo que se comporta sincrónicamente para
// el llamador.
// =============================================================================
typedef struct {
    const char       *text;
    SemaphoreHandle_t done;
    esp_err_t         result;
} synth_args_t;

static void synth_task(void *arg)
{
    synth_args_t *a = (synth_args_t *)arg;

    espeak_ERROR err = espeak_Synth(
        a->text, strlen(a->text) + 1,
        0, POS_CHARACTER, 0,
        espeakCHARS_UTF8, NULL, NULL);

    if (err != EE_OK) {
        ESP_LOGE(TAG, "espeak_Synth error: %d", err);
        a->result = ESP_FAIL;
    } else {
        espeak_Synchronize();
        a->result = ESP_OK;
    }

    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

esp_err_t tts_speak(const char *text)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "tts_init() no ejecutado");
        return ESP_ERR_INVALID_STATE;
    }
    if (!text || *text == '\0') return ESP_OK;

    ESP_LOGI(TAG, "\"%s\"", text);

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return ESP_ERR_NO_MEM;

    // s_resamp_phase se resetea en cada llamado para evitar acumulación entre frases
    s_resamp_phase = 0;

    synth_args_t args = { .text = text, .done = done, .result = ESP_FAIL };

    // 64 KB de stack para el motor de síntesis (TranslateClause + MakePhonemeList
    // consumen ~39 KB en la cadena de llamadas más profunda)
    BaseType_t ok = xTaskCreate(synth_task, "tts_synth", 65536, &args, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "no hay memoria para tarea de síntesis");
        vSemaphoreDelete(done);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return args.result;
}
