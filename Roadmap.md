# Cronograma de Desarrollo Incremental: Proyecto SENTIS

> **Estado general** — Hardware: Waveshare ESP32-P4-Module-DEV-KIT v1.3 · IDF v6.0.1

---

## ✅ Fase 1: El Sistema Nervioso (Salidas Hápticas y Base) — COMPLETA

*   **✅ Paso 1.1:** GPIO para los dos motores de vibración tipo moneda (izquierdo GPIO3, derecho GPIO6) via LEDC PWM.
*   **✅ Paso 1.2:** Patrones de vibración implementados: `PULSE_LEFT`, `PULSE_RIGHT`, `PULSE_BOTH`, `ALERT_BOTH` (intensidad máxima), `OFF`.

---

## ✅ Fase 2: La Voz de SENTIS (Almacenamiento y Audio) — COMPLETA

*   **✅ Paso 2.1:** MicroSD montada via SDMMC 4-bit en `/sdcard`. FAT VFS funcional.
*   **✅ Paso 2.2:** Parlante 2W funcionando via codec ES8311 (I2S0) + amplificador NS4150B. Reproducción de archivos `.wav` desde SD.

**Quirks resueltos (documentados para no repetirlos):**
- SD: el ESP32-P4 necesita habilitar el LDO interno #4 (`sd_pwr_ctrl_new_on_chip_ldo`) antes del SDMMC — sin esto, timeout constante.
- Audio: el I2S debe arrancar **antes** de configurar el codec (el ES8311 necesita MCLK activo para sincronizar su PLL).
- Audio: el ES8311 sale del reset con el DAC muteado por hardware (REG31 bits[6:5]=1). Hay que limpiarlo explícitamente.
- Monitor WiFi deshabilitado por defecto (comparte UART0 con el console; se activa via menuconfig).

**Formato WAV requerido:** 16000 Hz · 16-bit PCM · mono o estéreo  
**Conversión ffmpeg:** `ffmpeg -i audio.mp3 -ar 16000 -ac 1 -acodec pcm_s16le alerta.wav`  
**Volumen:** ajustar `AUDIO_VOLUME_PERCENT` en `components/audio/audio.h` (0–100 %). Referencia: 60–70% para no saturar.

---

## 🔄 Fase 3: El Escudo de Distancia (Integración del LiDAR) — PARCIALMENTE COMPLETA

*   **✅ Paso 3.1:** LiDAR SP10M01 comunicando via UART1 (460800 baud, protocolo reverse-engineered). Lecturas calibradas en mm.
*   **⏳ Paso 3.2 — PENDIENTE:** Fusión lógica completa. La háptica responde al LiDAR (funcional), pero **falta disparar `audio_play_wav()` cuando `dist < 50 cm`**. Requiere una tarea de audio independiente para no bloquear `proximity_task`.

**Siguiente acción concreta:**
1. Crear `audio_task` en FreeRTOS que reciba comandos via queue.
2. En `proximity_task`, cuando `pattern == HAPTIC_PATTERN_ALERT_BOTH`, encolar reproducción de `/sdcard/alerta.wav`.
3. Audio task consume la queue y llama `audio_play_wav()`.

---

## ⏳ Fase 4: La Escucha Activa (Micrófono I2S) — PENDIENTE

*   **Paso 4.1:** Configurar el micrófono INMP441 via I2S. Nota: I2S0 ya está usado por el codec de audio → usar **I2S1** para el micrófono.
*   **Paso 4.2:** Grabar muestra de voz directamente a la microSD (validación de calidad de audio).

---

## ✅ Fase 5: Abriendo los Ojos (Cámara MIPI CSI) — COMPLETA

*   **✅ Paso 5.1:** OV5647 (Raspberry Pi Camera v2) inicializada via MIPI CSI-2. Resolución RAW8 800×640 @50fps. Buffer en PSRAM.
*   **✅ Paso 5.2:** Análisis de frame en tiempo real: heurística de actividad de bordes que detecta la posición lateral del obstáculo (LEFT / CENTER / RIGHT). Integrado con `proximity_task`.

**Quirk documentado:** ESP32-P4 v1.x requiere LDO #3 @ 2500 mV para el PHY MIPI CSI, ajuste manual de `dmablk_size` del CSI bridge, e ISP en modo pass-through (RAW8 in/out).

---

## ⏳ Fase 6: El Cerebro Inteligente (Edge AI Local) — PENDIENTE

*   **Paso 6.1:** Detección de colores sobre el frame de la cámara → síntesis de voz por el parlante.
*   **Paso 6.2:** Reconocimiento de billetes argentinos (TinyML / Espressif Eye).
*   **Paso 6.3:** OCR — lectura de texto de carteles o documentos → síntesis de voz.

---

## ⏳ Fase 7: Navegación y Protocolo de Emergencia — PENDIENTE

*   **Paso 7.1:** GPS + asistencia por voz para guía de camino.
*   **Paso 7.2:** ESP32-C6 coprocesador (Wi-Fi/Bluetooth) — modo emergencia: SMS/coordenadas GPS ante caída.
