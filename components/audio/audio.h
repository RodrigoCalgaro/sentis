#pragma once

#include "esp_err.h"

// =============================================================================
// Formato de audio fijo — WAV files must match exactly:
//   Sample rate  : AUDIO_SAMPLE_RATE Hz
//   Bit depth    : 16-bit PCM signed
//   Channels     : mono or stereo (mono se duplica automáticamente)
//   Container    : RIFF WAV
//
// Para convertir desde otro formato con ffmpeg:
//   ffmpeg -i input.mp3 -ar 16000 -ac 1 -acodec pcm_s16le alert.wav
// =============================================================================
#define AUDIO_SAMPLE_RATE  16000

// -----------------------------------------------------------------------------
// Volumen de reproducción — rango 0 a 100 (porcentaje).
//
// Ajustar este valor si el parlante satura o se escucha muy bajo.
// Conversión interna al registro ES8311 REG32:
//   REG32 = (vol% × 256 / 100) − 1  (fórmula del driver oficial)
//   0%   → 0x00  (silencio)
//   50%  → 0x7F  (-6 dBFS aprox)
//   70%  → 0xB2  (punto de partida recomendado)
//   80%  → 0xCB  (default BSP Waveshare ESP32-P4)
//   100% → 0xFF  (0 dBFS — puede saturar parlantes pequeños)
//
// Referencia Waveshare BSP: CODEC_DEFAULT_VOLUME = 60
// -----------------------------------------------------------------------------
#define AUDIO_VOLUME_PERCENT  70

// Initialize I2S0, configure the ES8311 codec via I2C, and enable NS4150B amplifier.
// Requires bus_i2c_init() to have been called first (called internally if not).
esp_err_t audio_init(void);

// Play a WAV file from the VFS filesystem (e.g. "/sdcard/alert.wav").
// Blocks until playback is complete. Returns ESP_ERR_NOT_FOUND if the file
// does not exist, or ESP_ERR_NOT_SUPPORTED if the format is incompatible.
// Requires storage_init() and audio_init() to have succeeded.
esp_err_t audio_play_wav(const char *path);

// Set playback volume at runtime. vol_percent: 0 (silence) to 100 (max).
// Uses the same formula as the official espressif/es8311 component:
//   REG32 = (vol% × 256 / 100) − 1
// Effective immediately — no need to restart audio.
esp_err_t audio_set_volume(int vol_percent);

// Generate a 440 Hz square wave for 400 ms to verify codec → speaker chain.
void audio_test_tone(void);

// Immediately silence the amplifier output (PA enable pin, not codec volume).
void audio_mute(bool mute);
