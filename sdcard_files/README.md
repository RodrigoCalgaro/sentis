# sdcard_files — Archivos que deben copiarse a la microSD

Esta carpeta replica la estructura de directorios que debe existir en la microSD
para que el firmware funcione correctamente.

**Para copiar:** montar la microSD en la PC y copiar el contenido de cada
subcarpeta a la raíz de la tarjeta.

---

## Estructura en la microSD

```
/sdcard/
├── alert.wav            ← sonido de alerta (ya existente — Fase 2)
│
├── sr_model/            ← modelos MultiNet7 para STT en inglés (Fase 4)
│   ├── mnet.esrm
│   └── ...
│
├── espeak-ng-data/      ← datos de voz eSpeak-NG para TTS en español (Fase 6A)
│   ├── phontab
│   ├── phonindex
│   ├── phondata
│   ├── intonations
│   ├── es_dict
│   ├── lang/
│   │   └── roa/
│   │       └── es       ← definición de idioma español (familia romance)
│   └── voices/
│       └── ...
│
└── models/
    └── p4/              ← modelos pp_ocr_v6 para lectura OCR (Fase 7)
        ├── pp_ocr_v6_det_s8.espdl        ← detector de texto (usado)
        ├── pp_ocr_v6_rec_s16.espdl       ← reconocedor de texto (usado)
        ├── pp_ocr_v6_rec_s8.espdl        ← variante más rápida/liviana (sin usar por defecto)
        └── pp_ocr_v6_rec_s16_w640.espdl  ← variante para líneas largas (sin usar por defecto)
```

El diccionario de caracteres NO es un archivo en la SD — viene compilado
dentro del componente `espressif/pp_ocr_v6` (`pp_ocr_v6_dict.hpp`).

---

## Cómo obtener los archivos de eSpeak-NG

Ejecutar el script desde la raíz del proyecto:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup_espeak.ps1
```

El script descarga eSpeak-NG 1.51.1 y:
1. Copia los fuentes C a `components/espeak-ng/src/`
2. Copia los datos de voz a `sdcard_files/espeak-ng-data/`

Después copiar `sdcard_files/espeak-ng-data/` a la microSD.

---

## Archivos sr_model

Los modelos MultiNet7 se obtienen al ejecutar la build de IDF con
`MODEL_IN_SDCARD=y`. Ver `docs/stt_sd_setup.md` para el procedimiento completo.

---

## Cómo obtener los archivos de OCR (pp_ocr_v6)

El paquete `espressif/pp_ocr_v6` (declarado en `components/ocr/idf_component.yml`)
ya trae los `.espdl` compilados para ESP32-P4 dentro de sí mismo — no hay
descarga externa. Primero compilar una vez para que el Component Manager lo
resuelva a `managed_components/espressif__pp_ocr_v6/`, y después ejecutar:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup_ocr.ps1
```

Esto copia `managed_components/espressif__pp_ocr_v6/models/p4/` a
`sdcard_files/models/p4/`. Después copiar `sdcard_files/models/` a la microSD.
