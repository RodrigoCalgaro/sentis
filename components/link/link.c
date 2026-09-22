#include "link.h"

#include <string.h>

#include "audio.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "link";

// Mismo tamaño de chunk que MIC_CHUNK_SAMPLES (components/mic/mic.h) — no
// se incluye mic.h desde aca para no crear una dependencia de componente
// que link no necesita para nada mas.
#define LINK_AUDIO_CHUNK_SAMPLES 480

// Tope de seguridad por chunk de LINK_MSG_TTS_AUDIO — 100 ms a 16 kHz. La
// app manda el WAV sintetizado por el TTS de Android cortado en chunks de
// este tamaño (ver TtsSpeaker.kt); si mandara uno más grande, se trunca acá
// y se descarta el resto (mismo criterio que LINK_MSG_OCR_RESULT).
#define LINK_TTS_AUDIO_MAX_SAMPLES 1600

#define LINK_MAGIC 0x314E4553u  // "SEN1"

typedef enum {
    LINK_MSG_AUDIO = 1,
    LINK_MSG_OCR_REQUEST = 2,
    LINK_MSG_OCR_RESULT = 3,
    LINK_MSG_COMMAND = 4,
    LINK_MSG_TTS_AUDIO = 5,
    LINK_MSG_COLOR_REQUEST = 6,
    LINK_MSG_COLOR_RESULT = 7,
} link_msg_type_t;

// Payload de un chunk de audio TTS recibido del celular. count <=
// LINK_TTS_AUDIO_MAX_SAMPLES siempre — ver LINK_MSG_TTS_AUDIO en link_receive_loop.
typedef struct {
    int16_t samples[LINK_TTS_AUDIO_MAX_SAMPLES];
    size_t  count;
} tts_audio_chunk_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;
    uint8_t  pad[3];
    uint32_t size;
} link_header_t;

static SemaphoreHandle_t s_mutex;           // guarda s_client_fd + serializa sends
static SemaphoreHandle_t s_ocr_result_sem;  // dada por la recepcion cuando llega OCR_RESULT
static SemaphoreHandle_t s_color_result_sem;  // dada por la recepcion cuando llega COLOR_RESULT
static QueueHandle_t s_audio_queue;
static QueueHandle_t s_tts_audio_queue;
static link_command_cb_t s_command_cb;
static int s_client_fd = -1;
static char s_ocr_result_text[LINK_OCR_TEXT_MAX];
static char s_color_result_text[LINK_COLOR_TEXT_MAX];

// Scratch estatico para el chunk de LINK_MSG_TTS_AUDIO entrante — sizeof(tts_audio_chunk_t)
// (~3.2 KB) no entra comodo en el stack de 4 KB de link_server_task, así que
// vive fuera de él (mismo criterio que s_row_tmp en components/ocr/ocr.cpp).
// Seguro de reusar entre mensajes: xQueueSend copia el contenido antes de
// que el siguiente mensaje lo pise.
static tts_audio_chunk_t s_tts_recv_scratch;

static esp_err_t send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n <= 0) {
            return ESP_FAIL;
        }
        p += n;
        len -= (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        int n = recv(fd, p, len, 0);
        if (n <= 0) {
            return ESP_FAIL;
        }
        p += n;
        len -= (size_t)n;
    }
    return ESP_OK;
}

static void drain_bytes(int fd, uint32_t len)
{
    uint8_t scratch[128];
    while (len > 0) {
        size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
        if (recv_all(fd, scratch, chunk) != ESP_OK) {
            return;
        }
        len -= (uint32_t)chunk;
    }
}

// Manda un mensaje framed. ESP_ERR_NOT_FOUND si no hay celular conectado.
static esp_err_t link_send_framed(uint8_t type, const void *payload, uint32_t len)
{
    esp_err_t ret;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_fd < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        link_header_t hdr = { .magic = LINK_MAGIC, .type = type, .pad = {0}, .size = len };
        ret = send_all(s_client_fd, &hdr, sizeof(hdr));
        if (ret == ESP_OK && len > 0) {
            ret = send_all(s_client_fd, payload, len);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "send fallo (tipo=%d) — la tarea de recepcion va a notar la desconexion", type);
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

bool link_is_client_connected(void)
{
    bool connected;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    connected = (s_client_fd >= 0);
    xSemaphoreGive(s_mutex);
    return connected;
}

void link_send_audio(const int16_t *samples, size_t count)
{
    if (!s_audio_queue || !samples) {
        return;
    }
    int16_t buf[LINK_AUDIO_CHUNK_SAMPLES] = {0};
    size_t n = count < LINK_AUDIO_CHUNK_SAMPLES ? count : LINK_AUDIO_CHUNK_SAMPLES;
    memcpy(buf, samples, n * sizeof(int16_t));
    // No bloqueante: si esta lleno (sin celular conectado o el sender no
    // llega), se descarta este chunk — no queremos frenar al productor.
    xQueueSend(s_audio_queue, buf, 0);
}

esp_err_t link_request_ocr_text(const uint8_t *jpeg, size_t jpeg_len,
                                 char *out_text, size_t out_text_max,
                                 TickType_t timeout_ticks)
{
    if (out_text && out_text_max > 0) {
        out_text[0] = '\0';
    }
    if (!link_is_client_connected()) {
        return ESP_ERR_NOT_FOUND;
    }

    // Drenar una señal vieja por si quedó de un pedido anterior que hizo
    // timeout justo cuando la respuesta llegaba.
    xSemaphoreTake(s_ocr_result_sem, 0);

    esp_err_t ret = link_send_framed(LINK_MSG_OCR_REQUEST, jpeg, (uint32_t)jpeg_len);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_ocr_result_sem, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (out_text && out_text_max > 0) {
        strlcpy(out_text, s_ocr_result_text, out_text_max);
    }
    return ESP_OK;
}

esp_err_t link_request_color(const uint8_t *jpeg, size_t jpeg_len,
                              char *out_text, size_t out_text_max,
                              TickType_t timeout_ticks)
{
    if (out_text && out_text_max > 0) {
        out_text[0] = '\0';
    }
    if (!link_is_client_connected()) {
        return ESP_ERR_NOT_FOUND;
    }

    // Drenar una señal vieja por si quedó de un pedido anterior que hizo
    // timeout justo cuando la respuesta llegaba (mismo criterio que
    // link_request_ocr_text()).
    xSemaphoreTake(s_color_result_sem, 0);

    esp_err_t ret = link_send_framed(LINK_MSG_COLOR_REQUEST, jpeg, (uint32_t)jpeg_len);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_color_result_sem, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (out_text && out_text_max > 0) {
        strlcpy(out_text, s_color_result_text, out_text_max);
    }
    return ESP_OK;
}

static void link_receive_loop(int fd)
{
    for (;;) {
        link_header_t hdr;
        if (recv_all(fd, &hdr, sizeof(hdr)) != ESP_OK) {
            return;
        }
        if (hdr.magic != LINK_MAGIC) {
            ESP_LOGW(TAG, "magic invalido en el header, cierro la conexion");
            return;
        }

        switch (hdr.type) {
        case LINK_MSG_COMMAND: {
            link_command_t cmd = {0};
            uint32_t to_read = hdr.size < sizeof(cmd) ? hdr.size : sizeof(cmd);
            if (to_read > 0 && recv_all(fd, &cmd, to_read) != ESP_OK) {
                return;
            }
            if (hdr.size > to_read) {
                drain_bytes(fd, hdr.size - to_read);
            }
            cmd.text[LINK_COMMAND_TEXT_MAX - 1] = '\0';
            ESP_LOGI(TAG, "comando recibido: id=%d texto=\"%s\"", cmd.command_id, cmd.text);
            if (s_command_cb) {
                s_command_cb(&cmd);
            }
            break;
        }
        case LINK_MSG_TTS_AUDIO: {
            uint32_t max_bytes = LINK_TTS_AUDIO_MAX_SAMPLES * sizeof(int16_t);
            uint32_t to_read = hdr.size < max_bytes ? hdr.size : max_bytes;
            if (to_read > 0 && recv_all(fd, s_tts_recv_scratch.samples, to_read) != ESP_OK) {
                return;
            }
            if (hdr.size > to_read) {
                drain_bytes(fd, hdr.size - to_read);
            }
            s_tts_recv_scratch.count = to_read / sizeof(int16_t);
            // Bloqueante a proposito (a diferencia de link_send_audio, que
            // descarta si no hay lugar): acá perder un chunk se escucha como
            // una palabra que falta, así que conviene frenar la recepcion
            // del socket hasta que la reproduccion libere lugar — eso a su
            // vez aplica back-pressure TCP sobre el celular, que es quien
            // debe pausar el envio, no el ESP32 quien debe callarse a mitad
            // de frase.
            if (s_tts_recv_scratch.count > 0) {
                xQueueSend(s_tts_audio_queue, &s_tts_recv_scratch, portMAX_DELAY);
            }
            break;
        }
        case LINK_MSG_OCR_RESULT: {
            uint32_t to_read = hdr.size < (LINK_OCR_TEXT_MAX - 1) ? hdr.size : (LINK_OCR_TEXT_MAX - 1);
            char text[LINK_OCR_TEXT_MAX] = {0};
            if (to_read > 0 && recv_all(fd, text, to_read) != ESP_OK) {
                return;
            }
            if (hdr.size > to_read) {
                drain_bytes(fd, hdr.size - to_read);
            }
            text[to_read] = '\0';
            ESP_LOGI(TAG, "resultado OCR recibido: \"%s\"", text);
            strlcpy(s_ocr_result_text, text, sizeof(s_ocr_result_text));
            xSemaphoreGive(s_ocr_result_sem);
            break;
        }
        case LINK_MSG_COLOR_RESULT: {
            uint32_t to_read = hdr.size < (LINK_COLOR_TEXT_MAX - 1) ? hdr.size : (LINK_COLOR_TEXT_MAX - 1);
            char text[LINK_COLOR_TEXT_MAX] = {0};
            if (to_read > 0 && recv_all(fd, text, to_read) != ESP_OK) {
                return;
            }
            if (hdr.size > to_read) {
                drain_bytes(fd, hdr.size - to_read);
            }
            text[to_read] = '\0';
            ESP_LOGI(TAG, "resultado de color recibido: \"%s\"", text);
            strlcpy(s_color_result_text, text, sizeof(s_color_result_text));
            xSemaphoreGive(s_color_result_sem);
            break;
        }
        default:
            ESP_LOGW(TAG, "tipo de mensaje desconocido (%d, %u bytes), descartando", hdr.type, hdr.size);
            drain_bytes(fd, hdr.size);
            break;
        }
    }
}

static void link_server_task(void *arg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() fallo — abandono link_server_task");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(LINK_TCP_PORT),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen fallo en el puerto %d — abandono link_server_task", LINK_TCP_PORT);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "escuchando en el puerto %d", LINK_TCP_PORT);

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        ESP_LOGI(TAG, "celular conectado (fd=%d)", client_fd);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_fd = client_fd;
        xSemaphoreGive(s_mutex);

        link_receive_loop(client_fd);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_fd = -1;
        xSemaphoreGive(s_mutex);
        close(client_fd);
        ESP_LOGI(TAG, "celular desconectado");
    }
}

static void link_audio_sender_task(void *arg)
{
    int16_t chunk[LINK_AUDIO_CHUNK_SAMPLES];
    for (;;) {
        if (xQueueReceive(s_audio_queue, chunk, portMAX_DELAY) == pdTRUE) {
            link_send_framed(LINK_MSG_AUDIO, chunk, sizeof(chunk));
        }
    }
}

// link_tts_playback_task — reproduce por el parlante del ESP32 el audio TTS
// que manda el celular (LINK_MSG_TTS_AUDIO). audio_play_pcm() es bloqueante
// (dura lo mismo que el chunk en tiempo real), así que esta tarea reproduce
// a ritmo real mientras link_receive_loop sigue drenando el socket — el
// desacople entre ambas es exactamente s_tts_audio_queue.
//
// El scratch de destino del dequeue es estatico por el mismo motivo que
// s_tts_recv_scratch (sizeof(tts_audio_chunk_t) no entra comodo en un stack
// chico) — no hay condicion de carrera porque xQueueReceive es la unica
// escritora y esta tarea es la unica lectora.
static tts_audio_chunk_t s_tts_play_scratch;

static void link_tts_playback_task(void *arg)
{
    for (;;) {
        if (xQueueReceive(s_tts_audio_queue, &s_tts_play_scratch, portMAX_DELAY) == pdTRUE) {
            audio_play_pcm(s_tts_play_scratch.samples, s_tts_play_scratch.count);
        }
    }
}

esp_err_t link_init(link_command_cb_t cb)
{
    s_command_cb = cb;
    s_mutex = xSemaphoreCreateMutex();
    s_ocr_result_sem = xSemaphoreCreateBinary();
    s_color_result_sem = xSemaphoreCreateBinary();
    s_audio_queue = xQueueCreate(8, LINK_AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
    // Profundidad 6 (~600 ms a 16 kHz) — suficiente para absorber que el
    // celular manda los chunks de una locucion entera de un saque (ya
    // sintetizada) más rápido de lo que tarda en reproducirse. Si se llena,
    // xQueueSend bloquea en vez de descartar (ver LINK_MSG_TTS_AUDIO).
    s_tts_audio_queue = xQueueCreate(6, sizeof(tts_audio_chunk_t));
    if (!s_mutex || !s_ocr_result_sem || !s_color_result_sem || !s_audio_queue || !s_tts_audio_queue) {
        ESP_LOGE(TAG, "no se pudieron crear los primitivos de sincronizacion");
        return ESP_ERR_NO_MEM;
    }
    s_client_fd = -1;

    if (xTaskCreate(link_server_task, "link_server", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no se pudo crear link_server_task");
        return ESP_FAIL;
    }
    xTaskCreate(link_audio_sender_task, "link_audio_tx", 3072, NULL, 3, NULL);
    xTaskCreate(link_tts_playback_task, "link_tts_rx", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "link_init OK");
    return ESP_OK;
}
