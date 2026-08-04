#pragma once
/* ESP32-P4 platform configuration for eSpeak-NG 1.51.1
 * Replaces the autoconf-generated config.h for embedded builds.
 * This file must be on the compiler include path before the eSpeak-NG sources. */

#define PACKAGE         "espeak-ng"
#define PACKAGE_VERSION "1.51.1"
#define VERSION         "1.51.1"

/* Standard headers present in ESP-IDF */
#define HAVE_STDINT_H  1
#define HAVE_STDBOOL_H 1
#define HAVE_WCTYPE_H  1
#define HAVE_WCHAR_H   1

/* Synchronous synthesis only — no threads or FIFO queue needed */
#define USE_ASYNC  0

/* MBROLA diphone synthesis disabled (not available on embedded) */
#define USE_MBROLA 0

/* Output sample rate — must match AUDIO_SAMPLE_RATE defined in audio.h */
#define SAMPLE_RATE 16000

/* Path to runtime data directory — overridden at init via espeak_Initialize() */
#define PATH_ESPEAK_DATA "/sdcard/espeak-ng-data"

/* ESP32-P4 has no /proc filesystem */
#undef HAVE_PROC_SELF_EXE

/* No setlocale on ESP32 (not needed for synthesis) */
#define HAVE_SETLOCALE 0

/* Disable pcaudiolib — we provide our own audio output via audio_play_pcm() */
#define HAVE_PCAUDIOLIB 0
