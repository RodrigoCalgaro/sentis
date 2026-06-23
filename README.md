# SENTIS — Anteojos Asistivos con Inteligencia Artificial Local

SENTIS es un dispositivo de asistencia para personas con discapacidad visual, implementado sobre un ESP32-P4. Combina retroalimentación háptica, voz, cámara y LiDAR para ofrecer navegación y reconocimiento de entorno de forma completamente local, sin conexión a internet.

El plan de desarrollo completo está en [Roadmap.md](Roadmap.md).

---

## Estado actual del firmware

| Fase | Descripción | Estado |
|------|-------------|--------|
| Fase 1 — Háptica | Motores de vibración (izquierdo y derecho) con patrones PWM | ✅ Completa |
| Fase 2 — Audio | Codec ES8311 + amplificador NS4150B + microSD | ⬜ Pendiente |
| Fase 3 — LiDAR | Sensor SP10M01 → retroalimentación háptica por proximidad | ✅ Completa |
| Fase 4 — Micrófono | INMP441 via I2S | ⬜ Pendiente |
| Fase 5 — Cámara | OV5647 MIPI CSI-2 | ⬜ Pendiente |
| Fase 6 — Edge AI | Detección de colores, billetes, OCR local | ⬜ Pendiente |
| Fase 7 — GPS y emergencia | Navegación + ESP32-C6 coprocessor | ⬜ Pendiente |

El firmware integrado actualmente lee la distancia del LiDAR cada 50 ms y activa los motores de vibración según dos umbrales configurables:

- **Zona libre** (> 1500 mm): motores apagados.
- **Zona de precaución** (500–1500 mm): ambos motores pulsan suavemente (200 ms ON / 200 ms OFF).
- **Zona de peligro** (≤ 500 mm): ambos motores vibran de forma continua a máxima intensidad.

---

## Hardware

**Placa principal:** ESP32-P4 (dual-core RISC-V, 400 MHz, 32 MB PSRAM)

### Pines utilizados

| Periférico | GPIO | Detalle |
|------------|------|---------|
| Motor izquierdo | GPIO 3 | PWM via LEDC canal 0 |
| Motor derecho | GPIO 6 | PWM via LEDC canal 1 |
| LiDAR RX (cable verde) | GPIO 4 | UART1 RX — TX del sensor |
| LiDAR TX (cable amarillo) | GPIO 5 | UART1 TX — RX del sensor |

### Sensor LiDAR SP10M01

Sensor DToF de punto único, full-duplex UART a 460800 baud, alimentación 3.3 V. Transmite automáticamente tramas de 4 bytes a la frecuencia máxima del sensor, sin necesidad de comando de inicio. El protocolo fue obtenido por ingeniería inversa (ver [lidar.c](components/lidar/lidar.c)).

---

## Estructura del proyecto

```
sentis/
├── main/
│   ├── sentis.c              Punto de entrada. Define los umbrales de proximidad
│   │                         y lanza las tareas de háptica, LiDAR y proximidad.
│   └── CMakeLists.txt
│
├── components/
│   │
│   ├── board/
│   │   └── board_config.h    Mapa de pines y números de periférico para toda la placa.
│   │                         Único lugar donde se cambia la asignación de GPIOs.
│   │
│   ├── haptics/
│   │   ├── haptics.h         API pública: enum de patrones + haptic_init / haptic_set_pattern.
│   │   ├── haptics.c         Implementación: configura LEDC PWM, define los patrones
│   │   │                     de vibración y los ejecuta en una tarea FreeRTOS dedicada.
│   │   └── CMakeLists.txt
│   │
│   └── lidar/
│       ├── lidar.h           API pública: lidar_init / lidar_get_distance_mm.
│       ├── lidar.c           Implementación: configura UART1, decodifica el protocolo
│       │                     SP10M01 y expone la última medición en mm.
│       └── CMakeLists.txt
│
├── Roadmap.md                Plan de desarrollo incremental de las 7 fases.
└── README.md                 Este archivo.
```

### Descripción de cada archivo

#### `main/sentis.c`
Punto de entrada del firmware (`app_main`). Define dos constantes que controlan el comportamiento háptico:
- `PROXIMITY_WARN_MM` — distancia a la que comienza la vibración pulsante (por defecto 1500 mm).
- `PROXIMITY_ALERT_MM` — distancia a la que se activa la vibración continua (por defecto 500 mm).

Contiene la función `proximity_task` que corre en segundo plano, lee la distancia del LiDAR cada 50 ms y llama a `haptic_set_pattern` con el patrón correspondiente a la zona detectada.

#### `components/board/board_config.h`
Centraliza todos los pines y números de periférico del hardware. Todos los componentes importan este archivo en lugar de hardcodear números de GPIO. Si se cambia la placa o el ruteo de pines, este es el único archivo a modificar.

#### `components/haptics/haptics.h` y `haptics.c`
Controla los dos motores de vibración tipo moneda mediante PWM (periférico LEDC del ESP32). Expone cinco patrones: `OFF`, `PULSE_LEFT`, `PULSE_RIGHT`, `PULSE_BOTH` y `ALERT_BOTH`. La ejecución de los patrones ocurre en una tarea FreeRTOS interna, lo que permite que los retardos de los pulsos no bloqueen ningún otro componente del sistema.

#### `components/lidar/lidar.h` y `lidar.c`
Maneja la comunicación con el sensor LiDAR SP10M01 a través de UART1. Implementa la decodificación del protocolo propietario del sensor (ingeniería inversa documentada en el encabezado del archivo). Una tarea FreeRTOS interna recibe el stream continuo de tramas, verifica la integridad de cada una mediante checksum y actualiza la variable compartida `s_distance_mm`, accesible desde cualquier tarea via `lidar_get_distance_mm()`.

---

## Compilación y flasheo

Este proyecto usa ESP-IDF. Desde el directorio raíz del proyecto:

```bash
# Configurar el target (solo la primera vez)
idf.py set-target esp32p4

# Compilar
idf.py build

# Flashear y abrir monitor serie
idf.py flash monitor
```

El monitor serie mostrará las lecturas del LiDAR en tiempo real (`dist=NNN mm`) una vez que el sensor inicialice correctamente.
