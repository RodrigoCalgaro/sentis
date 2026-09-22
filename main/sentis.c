#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "haptics.h"
#include "wifi.h"
#include "cp_ota.h"
#include "link.h"
#include "lidar.h"
#include "vision.h"
#include "monitor.h"
#include "audio.h"
#include "mic.h"
#include "tts.h"
#include "ocr.h"
#include "settings.h"

// -----------------------------------------------------------------------------
// Umbrales de proximidad (distancia a partir de la cual empieza la vibración
// suave / la alerta a máxima intensidad) y volumen del parlante: ya no son
// constantes fijas, son ajustables en runtime desde la app Android companion
// y persisten en NVS entre reinicios — ver components/settings. proximity_task
// los lee en cada iteración con settings_get_proximity_warn_mm()/alert_mm();
// los defaults (1500 mm / 500 mm / 70%) y el invariante alert_mm < warn_mm
// están documentados en settings.h.
// -----------------------------------------------------------------------------

// Intervalo entre evaluaciones de proximidad en milisegundos.
// 50 ms → 20 evaluaciones por segundo, suficiente para obstáculos semi-dinámicos.
// La cámara corre en segundo plano a 15 fps (~67 ms por frame), por lo que en
// cada evaluación se lee el resultado del último frame analizado sin bloquear.
#define PROXIMITY_POLL_MS   50

// -----------------------------------------------------------------------------
// on_link_command — callback invocada por el componente link cuando llega un
// comando de voz reconocido desde la app Android companion (Vosk, del lado
// del celular — el ESP32 nunca escucha por su propio micrófono para esto,
// ver ocr_reading_start/stop más abajo y components/link/link.h).
//
// Reemplaza al viejo on_stt_result (ESP-SR/MultiNet on-device, retirado en
// la Fase 2 — ver sentis-stability-integration-plan.md). Misma tabla de IDs
// de comando que usaba stt.c, para no romper la app Android de prueba
// (android/sentis-companion/LinkClient.kt ya manda 6/7).
//
// Se llama desde la tarea de recepción de link — no bloquear aquí.
// -----------------------------------------------------------------------------
static void on_link_command(const link_command_t *cmd)
{
    ESP_LOGI("link", "COMANDO (app): [%d] \"%s\"", cmd->command_id, cmd->text);

    // Publicar al viewer gráfico (no-op si CONFIG_MONITOR_ENABLED=n).
    monitor_set_stt_text(cmd->text);

    switch (cmd->command_id) {
        case 6:  // "start reading"
            ocr_reading_start();
            break;
        case 7:  // "stop reading"
            ocr_reading_stop();
            break;
        case 8:  // "detectar color"
            ocr_detect_color();
            break;
        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// on_link_settings_set / on_link_settings_get — callbacks pasadas a link_init()
// para LINK_MSG_SETTINGS_SET/STATE (ver components/link/link.h). Solo hacen de
// puente hacia components/settings, que es quien valida, clampea, persiste en
// NVS y aplica el cambio (audio_set_volume + tono de prueba para el volumen).
// Se llaman desde la tarea de recepción de link — no bloquear aquí (los
// setters de settings son rápidos: memoria + NVS, sin I/O de red).
// -----------------------------------------------------------------------------
static void on_link_settings_set(link_setting_id_t param_id, int32_t value)
{
    switch (param_id) {
        case LINK_SETTING_VOLUME:
            settings_set_volume_percent((int)value);
            break;
        case LINK_SETTING_PROXIMITY_WARN_MM:
            settings_set_proximity_warn_mm((int)value);
            break;
        case LINK_SETTING_PROXIMITY_ALERT_MM:
            settings_set_proximity_alert_mm((int)value);
            break;
        default:
            ESP_LOGW("link", "settings SET: param_id desconocido (%d)", (int)param_id);
            break;
    }
}

static link_settings_state_t on_link_settings_get(void)
{
    link_settings_state_t state = {
        .volume_pct = settings_get_volume_percent(),
        .proximity_warn_mm = settings_get_proximity_warn_mm(),
        .proximity_alert_mm = settings_get_proximity_alert_mm(),
    };
    return state;
}

// -----------------------------------------------------------------------------
// proximity_task — lee distancia del LiDAR, consulta posición de la cámara y
// selecciona el patrón háptico mediante la tabla de fusión.
//
// Tabla de decisión:
//
//   dist == 0 ó dist > WARN     → sin obstáculo relevante  → OFF
//
//   ALERT < dist ≤ WARN         → zona de precaución
//     cámara lista + LEFT       → PULSE_LEFT   (motor izquierdo pulsa)
//     cámara lista + RIGHT      → PULSE_RIGHT  (motor derecho pulsa)
//     cámara lista + CENTER     → PULSE_BOTH   (ambos pulsan, obstáculo frontal)
//     cámara lista + NONE       → PULSE_BOTH   (sin posición resuelta: fallback)
//     cámara no lista aún       → PULSE_BOTH   (fallback hasta primer frame)
//
//   dist ≤ ALERT                → zona de peligro
//     siempre                   → ALERT_BOTH   (ambos vibran, máxima prioridad)
//     (posición ignorada: a <50 cm la reacción es crítica y no hay margen)
//
//   ocr_is_reading() == true    → lectura OCR en curso: OFF sin importar la
//     medición del LiDAR. El objeto a leer se acerca a la cámara a propósito,
//     así que la vibración continua de "obstáculo cercano" no aporta nada y
//     solo distrae al usuario mientras escucha la lectura. Esta condición
//     tiene prioridad absoluta sobre la tabla de fusión de abajo y se
//     restaura la lógica normal recién cuando el comando "stop reading" baja
//     la bandera (ver ocr_reading_stop() en ocr.cpp).
//
// La función haptic_set_pattern es segura para llamar desde esta tarea porque
// la escritura sobre s_pattern es atómica (ver haptics.c).
// La lectura de vision_get_obstacle_side() también es atómica (volatile uint8_t).
// -----------------------------------------------------------------------------
static void proximity_task(void *arg)
{
    while (1) {
        haptic_pattern_t pattern;

        if (ocr_is_reading()) {
            // Lectura OCR en curso: prioridad absoluta, ignorar LiDAR/visión.
            pattern = HAPTIC_PATTERN_OFF;
            haptic_set_pattern(pattern);
            vTaskDelay(pdMS_TO_TICKS(PROXIMITY_POLL_MS));
            continue;
        }

        uint16_t dist = lidar_get_distance_mm();
        uint16_t warn_mm = settings_get_proximity_warn_mm();
        uint16_t alert_mm = settings_get_proximity_alert_mm();

        if (dist == 0 || dist > warn_mm) {
            // Sin datos todavía, o el obstáculo está fuera del rango de interés.
            pattern = HAPTIC_PATTERN_OFF;

        } else if (dist > alert_mm) {
            // Zona de precaución: el obstáculo se acerca pero hay margen.
            // Usar la posición lateral de la cámara para activar solo el motor
            // del lado correspondiente.
            if (!vision_is_ready()) {
                // La cámara aún no procesó su primer frame. Comportamiento de
                // arranque seguro: activar ambos motores hasta tener datos.
                pattern = HAPTIC_PATTERN_PULSE_BOTH;
            } else {
                switch (vision_get_obstacle_side()) {
                    case OBSTACLE_SIDE_LEFT:
                        pattern = HAPTIC_PATTERN_PULSE_LEFT;
                        break;
                    case OBSTACLE_SIDE_RIGHT:
                        pattern = HAPTIC_PATTERN_PULSE_RIGHT;
                        break;
                    case OBSTACLE_SIDE_CENTER:
                    case OBSTACLE_SIDE_NONE:
                    default:
                        // NONE: la heurística no resolvió una posición clara
                        // (sin bordes suficientes o escena muy uniforme).
                        // Fallback a ambos motores para no dejar al usuario
                        // sin retroalimentación.
                        pattern = HAPTIC_PATTERN_PULSE_BOTH;
                        break;
                }
            }

        } else {
            // Zona de peligro: obstáculo muy cercano, detención inmediata.
            // Seguridad primero: ambos motores a máxima intensidad,
            // independientemente de la posición lateral.
            pattern = HAPTIC_PATTERN_ALERT_BOTH;
        }

        haptic_set_pattern(pattern);

        // Ceder CPU al resto del sistema durante el intervalo de muestreo.
        vTaskDelay(pdMS_TO_TICKS(PROXIMITY_POLL_MS));
    }
}

// -----------------------------------------------------------------------------
// app_main — punto de entrada del firmware SENTIS.
//
// Orden de inicialización:
//    1. haptic_init   — LEDC PWM, sin dependencias externas
//    2. lidar_init    — UART1, sin dependencias externas
//    3. wifi_init     — C6 (esp_hosted/SDIO) + SoftAP para la app companion
//    4. cp_ota_check_and_update — actualiza el firmware del C6 si hace falta
//    5. settings_init — carga volumen/umbrales de proximidad desde NVS
//    6. link_init     — servidor TCP hacia la app companion (Fase 2)
//    7. audio_init    — ES8311 + I2S0 full-duplex + NS4150B (Fase 2 + Fase 4)
//                       Abre TX (playback) y RX (micrófono) en el mismo I2S0.
//                       Aplica el volumen cargado por settings_init().
//    8. tts_init      — monta partición de flash "voice_data" y carga voz
//                       eSpeak-NG (Fase 6A / Fase 2). Reproduce "Sentis
//                       Encendido" como confirmación de arranque.
//    9. mic_init      — tarea de captura: ES8311 ADC → chunks mono → link_send_audio()
//   10. vision_init   — I2C + MIPI CSI-2 (Fase 5)
//   11. ocr_init      — captura+JPEG, pedido de lectura vía app companion (Fase 2)
//   12. monitor_init  — transmisión de frames para desarrollo (Fase 5)
//   13. proximity_task — fusiona LiDAR + visión + háptica, usando los umbrales
//                       de settings_init() (ajustables en runtime vía app)
//
// Fase 2 (ver sentis-stability-integration-plan.md): el reconocimiento de voz
// (antes ESP-SR/MultiNet7, inglés-only) y el OCR (antes pp_ocr_v6, crasheaba
// tras ~46s) se retiraron por completo del ESP32 — ahora corren en la app
// Android companion (Vosk + ML Kit), conectada por el SoftAP del C6. La SD
// ya no se monta: ni TTS (datos en flash) ni ningún otro componente activo
// la necesitan — ver components/storage/ si hace falta reactivarla.
// -----------------------------------------------------------------------------
void app_main(void)
{
    haptic_init();

    // Autotest hápticos: pulsa cada motor por separado para confirmar al
    // arrancar que ambos responden (mismo espíritu que audio_play_wav más
    // abajo para el speaker). Usa los patrones existentes en vez de tocar
    // el PWM directo para no pisar la lectura de s_pattern de haptic_task.
    haptic_set_pattern(HAPTIC_PATTERN_PULSE_LEFT);
    vTaskDelay(pdMS_TO_TICKS(400));
    haptic_set_pattern(HAPTIC_PATTERN_PULSE_RIGHT);
    vTaskDelay(pdMS_TO_TICKS(400));
    haptic_set_pattern(HAPTIC_PATTERN_OFF);

    lidar_init();

    // ---- SoftAP para la app companion ----
    // wifi_init() levanta el ESP32-C6 (esp_hosted, SDIO) + SoftAP. Se llama
    // ANTES de storage_init(): hay un bug conocido de ESP-IDF (issue #16233)
    // donde SDMMC (SD) y esp_hosted (también SDIO) se pisan si se inicializan
    // en el orden contrario en el mismo controlador. No fatal: si el C6 no
    // responde, se loguea el error y el resto del sistema sigue igual que
    // hoy (sin WiFi, sin cambios de comportamiento).
    esp_err_t wifi_ret = wifi_init();

    // ---- Actualizar firmware del co-procesador C6 si hace falta ----
    // El C6 de esta placa vino de fábrica con firmware genérico ("major
    // version mismatch — OTA coprocessor from host" en el log, versión
    // reportada 0.0.0) — sospechoso de varios problemas encontrados en
    // hardware real (2026-09-18: WiFi intermitente, cámara sin frames).
    // Requiere que wifi_init() haya levantado el link con el C6. No fatal:
    // si la partición "slave_fw" está vacía (placa sin provisionar) o el
    // OTA falla, se loguea y el sistema sigue con lo que ya tenga el C6.
    if (wifi_ret == ESP_OK) {
        cp_ota_check_and_update();
    }

    // ---- Ajustes persistidos (volumen, umbrales de proximidad) ----
    // settings_init() abre NVS y carga los tres valores (o los defaults si es
    // el primer boot). wifi_init() ya corrió nvs_flash_init() más arriba,
    // sea cual sea su resultado (ver components/wifi/wifi.c: init_nvs() es lo
    // primero que hace) — no fatal si NVS no está disponible, se sigue con
    // los defaults en memoria sin persistencia.
    settings_init();

    // ---- Fase 2: protocolo hacia la app companion ----
    // link_init() levanta el servidor TCP (puerto components/link/link.h,
    // LINK_TCP_PORT) sobre el SoftAP. on_link_command() reemplaza al viejo
    // on_stt_result — los comandos de voz ahora se reconocen del lado del
    // celular (Vosk), nunca on-device. on_link_settings_set/get exponen
    // volumen y umbrales de proximidad ajustables desde la app (ver
    // components/settings). No fatal si falla.
    //
    // El bug histórico "la cámara nunca entrega frames con el C6 activo" ya
    // se resolvió (ver sentis-stability-integration-plan.md, Fase 1 — el CP
    // corría en modo SW_AGGR, ahora en STREAM) — ya no hace falta mantener
    // esto deshabilitado para aislar esa causa.
    link_init(on_link_command, on_link_settings_set, on_link_settings_get);

    // ---- Fase 2: audio ----
    // La SD ya no se monta acá: el sonido de alerta de arranque (alert.wav)
    // se sacó por completo (dejó de tener sentido una vez que "Sentis
    // Encendido" por TTS ya confirma que el audio funciona), y los datos de
    // voz de TTS se migraron a flash (ver más abajo) — storage_init() no
    // tiene ya ningún consumidor en el firmware activo. Queda disponible en
    // components/storage/ para si hace falta SD a futuro (ver el conflicto
    // conocido SDMMC-vs-esp_hosted documentado ahí).
    audio_init();     // ES8311 + I2S0 full-duplex (TX playback + RX mic)

    // Aplicar el volumen persistido (o el default) recién ahora — antes de
    // esto audio_set_volume() no tiene codec inicializado para escribirle.
    // Silencioso a propósito (sin tono de prueba): eso queda reservado para
    // cambios en runtime pedidos desde la app (ver settings_set_volume_percent).
    audio_set_volume(settings_get_volume_percent());

    // ---- Fase 6A / Fase 2: TTS en español (eSpeak-NG desde flash) ----
    // tts_init monta la partición "voice_data" (imagen FAT de solo lectura,
    // ver components/tts/voice_data/ y partitions.csv) y carga los datos de
    // voz desde ahí. No fatal: si algo falla, se loguea el error y el
    // sistema sigue operando sin TTS.
    if (tts_init() == ESP_OK) {
        tts_speak("Sentis Encendido");
    }

    // ---- Fase 2: micrófono → app companion ----
    // mic_init arranca la tarea de captura: I2S0 RX (ES8311 ADC) → downmix a
    // mono → link_send_audio() manda cada chunk al celular por TCP. Ya no hay
    // reconocimiento de voz on-device (ESP-SR/MultiNet se retiró — el celular
    // corre Vosk sobre este mismo stream de PCM). link_send_audio() no
    // bloquea y no falla si todavía no hay celular conectado (descarta el
    // chunk), así que no hace falta esperar a que link_init() haya
    // encontrado un cliente antes de arrancar la captura.
    mic_init(link_send_audio);

    // ---- Fase 5: cámara ----
    vision_init();

    // ---- Fase 2: lectura de texto vía app companion ----
    // ocr_init arma los buffers de captura+JPEG y deja la tarea de lectura
    // lista pero inactiva hasta el comando "start reading" (ver
    // on_link_command). Ya no depende de la SD (el reconocimiento corre en
    // el celular con ML Kit) — se llama sin condición.
    ocr_init();

    // Monitor visual solo para desarrollo (menuconfig → SENTIS Monitor).
    // Deshabilitar (CONFIG_MONITOR_ENABLED=n) antes de un build de producción.
    monitor_init();

    // DIAGNÓSTICO TEMPORAL — margen real de PSRAM una vez que todos los
    // componentes ya reservaron su memoria (buffers de vision/ocr, TTS,
    // audio). Se usa para decidir si alcanza para subir la resolución de
    // captura de la cámara (800x640 → 800x1280 RAW8, o RAW10 1280x960/
    // 1920x1080) sin quedarse sin memoria en tiempo de ejecución. Quitar una
    // vez tomada la decisión.
    ESP_LOGI("sentis", "PSRAM libre tras init: %u bytes (bloque contiguo mas grande: %u bytes)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    xTaskCreate(proximity_task, "proximity", 2048, NULL, 5, NULL);
}
