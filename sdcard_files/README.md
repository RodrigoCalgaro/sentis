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
└── espeak-ng-data/      ← datos de voz eSpeak-NG para TTS en español (Fase 6A)
    ├── phontab
    ├── phonindex
    ├── phondata
    ├── intonations
    ├── es_dict
    ├── lang/
    │   └── es           ← definición de idioma español
    └── voices/
        ├── !v/          ← variantes de voz
        └── other/
            └── es       ← definición de la voz "es"
```

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
