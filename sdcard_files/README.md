# sdcard_files — Archivos que deben copiarse a la microSD

Esta carpeta replica la estructura de directorios que debe existir en la microSD
para que el firmware funcione correctamente.

**Para copiar:** montar la microSD en la PC y copiar el contenido de cada
subcarpeta a la raíz de la tarjeta.

---

## Estructura en la microSD

```
/sdcard/
├── alert.wav            ← sonido de alerta (Fase 2)
│
└── espeak-ng-data/      ← datos de voz eSpeak-NG para TTS en español (Fase 6A)
    ├── phontab
    ├── phonindex
    ├── phondata
    ├── intonations
    ├── es_dict
    ├── lang/
    │   └── roa/
    │       └── es       ← definición de idioma español (familia romance)
    └── voices/
        └── ...
```

Fase 2 (ver sentis-stability-integration-plan.md): el reconocimiento de voz
(antes `sr_model/`, MultiNet7 desde la SD) y el OCR (antes `models/p4/`,
pp_ocr_v6 desde la SD) se retiraron del ESP32 — corren en la app Android
companion (Vosk + ML Kit), que no lee nada de esta tarjeta.

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
