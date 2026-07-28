# STT — Configuración de modelos en microSD

> **Contexto:** Los modelos de reconocimiento de voz (ESP-SR MultiNet7) se cargan
> desde la microSD en lugar de una partición flash. Esto elimina la restricción de
> tamaño y permite cambiar o actualizar modelos sin reflaschear el firmware.

---

## Requisitos de hardware

- Waveshare ESP32-P4-Module-DEV-KIT v1.3
- microSD formateada en FAT32 (cualquier tamaño)
- Lector de SD en el PC para la copia inicial

---

## Paso 1 — Copiar los modelos a la SD (una sola vez)

Sacá la microSD del kit e insertala en tu PC.

Desde la raíz del proyecto, ejecutar en PowerShell (reemplazar `E:` con la letra real de la SD):

```powershell
New-Item -ItemType Directory -Force E:\sr_model
Copy-Item -Recurse components\espressif__esp-sr\model\multinet_model\mn7_en  E:\sr_model\
Copy-Item -Recurse components\espressif__esp-sr\model\multinet_model\fst     E:\sr_model\
```

### Estructura esperada en la SD

```
sr_model/
├── mn7_en/                ← modelo MultiNet7 inglés (~2.7 MB)
│   ├── _MODEL_INFO_
│   ├── mn7_data
│   ├── mn7_index
│   └── vocab
└── fst/                   ← vocabulario FST requerido por MultiNet6/7 (~12 KB)
    ├── commands_cn.txt
    └── commands_en.txt
```

---

## Paso 2 — Build y flash del firmware

> Solo necesario la primera vez o cuando haya cambios de código.

```powershell
Remove-Item sdkconfig          # fuerza regeneración desde sdkconfig.defaults
idf.py flash
```

La tabla de particiones **no incluye** la partición `model` (se eliminó). Los modelos
se cargan en runtime desde `/sdcard/sr_model/` vía `esp_srmodel_init`.

---

## Paso 3 — Verificar en el monitor serie

```powershell
idf.py monitor
```

Boot exitoso con STT activo:

```
I (xxxx) MODEL_LOADER: load models from sdcard: /sdcard/sr_model
I (xxxx) stt: modelo MultiNet: mn7_en
                Quantized MultiNet7:rnnt_ctc_2.0, name:mn7_en, ...
I (xxxx) stt: listo — 5 comandos registrados
I (xxxx) mic: capture task started — 480 samples/chunk @ 16000 Hz mono
```

Cuando se detecta un comando:

```
I (xxxxx) stt: COMANDO: [1] "stop"
I (xxxxx) stt: COMANDO: [2] "turn left"
```

---

## Comandos configurados

Definidos en `components/stt/stt.c` en la tabla `s_commands[]`.

| ID | Frase reconocida | Uso previsto |
|----|-----------------|--------------|
| 1  | stop            | Detener acción actual |
| 2  | turn left       | Girar izquierda |
| 3  | turn right      | Girar derecha |
| 4  | alert on        | Activar alerta de emergencia |
| 5  | help            | Modo emergencia / auxilio |

Para agregar o cambiar comandos: editar `s_commands[]` en `stt.c` y reflaschear.
No es necesario volver a copiar archivos a la SD.

---

## Solución de problemas

### `E stt: esp_srmodel_init failed`
- Verificar que la microSD está insertada antes de encender el kit.
- Verificar que el directorio `/sr_model/mn7_en/` existe en la SD.
- La SD debe estar formateada en FAT32 y montada en `/sdcard`.

### `E stt: no se encontro modelo MultiNet en la particion`
- El directorio `sr_model/mn7_en/` existe pero no contiene `_MODEL_INFO_`.
- Repetir el Paso 1 para copiar los archivos correctamente.

### El firmware detecta comandos pero con score bajo (< 0.20)
- Hablar cerca del micrófono (10–20 cm), con voz clara.
- Verificar ganancia ADC del ES8311: registros `0x14` y `0x17` en `components/audio/audio.c`.
- Scores entre 0.15 y 0.50 son normales para mn7_en con micrófono onboard.

---

## Actualizar o cambiar el modelo (sin reflaschear)

El modelo se lee de la SD en cada boot. Para cambiar a un modelo diferente:

1. Copiar el nuevo directorio de modelo a `sr_model/` en la SD.
2. Reiniciar el kit — el nuevo modelo se carga automáticamente.

Modelos disponibles en el componente (para ESP32-P4):

| Modelo | Idioma | Tamaño | Kconfig |
|--------|--------|--------|---------|
| `mn7_en` | Inglés | 2.7 MB | `SR_MN_EN_MULTINET7_QUANT` ✓ usado |
| `mn7_cn` | Chino  | ~3 MB  | `SR_MN_CN_MULTINET7_QUANT` |

> **Español:** No existe un modelo MultiNet7 en español para ESP32-P4 en el
> registry de Espressif (julio 2026). La Fase 6 del Roadmap contempla entrenamiento
> de modelos custom con TinyML/Espressif Eye.

---

## Archivos relevantes del proyecto

| Archivo | Rol |
|---------|-----|
| `components/stt/stt.c` | Inicialización ESP-SR, tabla de comandos, callback |
| `components/mic/mic.c` | Captura de audio ES8311 ADC → chunks mono 480 samples |
| `components/audio/audio.c` | I2S0 full-duplex, ganancia ADC (regs 0x14/0x17) |
| `sdkconfig.defaults` | `CONFIG_MODEL_IN_SDCARD=y`, `SR_MN_EN_MULTINET7_QUANT=y` |
| `partitions.csv` | Sin partición `model`; solo nvs + phy + factory (3 MB) |
