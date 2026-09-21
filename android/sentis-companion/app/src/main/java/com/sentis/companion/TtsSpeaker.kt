package com.sentis.companion

import android.content.Context
import android.os.Bundle
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

// =============================================================================
// TtsSpeaker — sintetiza el texto de OCR con el TTS nativo de Android (mejor
// cadencia que eSpeak-NG del ESP32, ver components/ocr/ocr.cpp) y manda el
// PCM resultante de vuelta al ESP32 vía sendAudioChunk (LinkClient,
// MSG_TTS_AUDIO) para que se escuche por el parlante de SENTIS — no el del
// teléfono (decisión del usuario 2026-09-21, corrigiendo el primer intento
// de esta clase que sí sonaba local). El componente tts del ESP32 sigue
// activo para todo lo que no depende del celular (ej. "Sentis Encendido").
//
// synthesizeToFile() sintetiza la locución COMPLETA a un WAV antes de poder
// mandar nada — no hay streaming incremental real. Simplificación
// deliberada: para una oración de OCR la demora es chica, y evita la
// complejidad de engancharse a onAudioAvailable() (no todos los motores TTS
// lo soportan de la misma forma).
// =============================================================================

private const val TARGET_SAMPLE_RATE = 16000

// Debe coincidir con LINK_TTS_AUDIO_MAX_SAMPLES en components/link/link.c —
// si se manda un chunk más grande, el ESP32 lo trunca y descarta el resto.
private const val CHUNK_SAMPLES = 1600

class TtsSpeaker(
    context: Context,
    private val onLog: (String) -> Unit,
    private val sendAudioChunk: (ShortArray) -> Unit,
) {
    private val executor = Executors.newSingleThreadExecutor { r -> Thread(r, "tts-speaker") }
    private val cacheDir = context.cacheDir
    @Volatile private var ready = false
    private val tts: TextToSpeech = TextToSpeech(context) { status -> onInit(status) }

    private fun onInit(status: Int) {
        if (status != TextToSpeech.SUCCESS) {
            onLog("Error inicializando TTS de Android (status=$status)")
            return
        }
        val result = tts.setLanguage(Locale("es", "AR"))
        if (result == TextToSpeech.LANG_MISSING_DATA || result == TextToSpeech.LANG_NOT_SUPPORTED) {
            onLog("TTS de Android: sin voz en español instalada, el texto no se va a escuchar")
            return
        }
        ready = true
        onLog("TTS de Android listo.")
    }

    fun speak(text: String) {
        if (!ready || text.isBlank()) return
        executor.execute { synthesizeAndStream(text) }
    }

    private fun synthesizeAndStream(text: String) {
        val file = File(cacheDir, "tts_${UUID.randomUUID()}.wav")
        val utteranceId = "tts-${System.nanoTime()}"
        val latch = CountDownLatch(1)
        // No hace falta @Volatile: CountDownLatch.countDown()/await() ya
        // establecen happens-before, así que el valor escrito antes de
        // countDown() se ve correctamente después de que await() retorna.
        var success = false

        tts.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
            override fun onStart(id: String?) {}
            override fun onDone(id: String?) {
                success = true
                latch.countDown()
            }
            @Deprecated("Deprecated in Java", ReplaceWith(""))
            override fun onError(id: String?) {
                latch.countDown()
            }
            override fun onError(id: String?, errorCode: Int) {
                latch.countDown()
            }
        })

        if (tts.synthesizeToFile(text, Bundle(), file, utteranceId) != TextToSpeech.SUCCESS) {
            onLog("TTS: no se pudo iniciar la síntesis")
            return
        }

        val finished = latch.await(15, TimeUnit.SECONDS)
        if (!finished || !success) {
            onLog("TTS: la síntesis no terminó a tiempo")
            file.delete()
            return
        }

        try {
            streamWavFile(file)
        } catch (e: Exception) {
            onLog("TTS: error leyendo/mandando el WAV sintetizado: ${e.message}")
        } finally {
            file.delete()
        }
    }

    private fun streamWavFile(file: File) {
        val bytes = file.readBytes()
        if (bytes.size < 44 || String(bytes, 0, 4, Charsets.US_ASCII) != "RIFF") {
            onLog("TTS: WAV con header inesperado, se descarta")
            return
        }
        val sourceRate = readLeInt(bytes, 24)
        val channels = readLeShort(bytes, 22)
        val bitsPerSample = readLeShort(bytes, 34)
        if (bitsPerSample != 16) {
            onLog("TTS: WAV de $bitsPerSample bits (se esperaba 16), se descarta")
            return
        }

        val pcm = bytes.copyOfRange(44, bytes.size)
        val samples = ShortArray(pcm.size / 2)
        val buf = ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN)
        for (i in samples.indices) samples[i] = buf.short

        val mono = if (channels == 2) downmixStereo(samples) else samples
        // El motor de TTS de Android sintetiza a la frecuencia que declare
        // (típicamente 22050 o 24000 Hz, no fija) — audio_play_pcm() en el
        // ESP32 espera AUDIO_SAMPLE_RATE (16 kHz), así que hay que
        // resamplear salvo que ya coincida.
        val resampled = if (sourceRate == TARGET_SAMPLE_RATE) {
            mono
        } else {
            resampleLinear(mono, sourceRate, TARGET_SAMPLE_RATE)
        }

        var offset = 0
        while (offset < resampled.size) {
            val end = minOf(offset + CHUNK_SAMPLES, resampled.size)
            sendAudioChunk(resampled.copyOfRange(offset, end))
            offset = end
        }
    }

    private fun downmixStereo(interleaved: ShortArray): ShortArray {
        val mono = ShortArray(interleaved.size / 2)
        for (i in mono.indices) {
            val l = interleaved[i * 2].toInt()
            val r = interleaved[i * 2 + 1].toInt()
            mono[i] = ((l + r) / 2).toShort()
        }
        return mono
    }

    // Resampling lineal — mismo criterio que el resampler Q16 nearest-neighbor
    // que ya usa components/tts/tts.c del lado del ESP32 para este mismo
    // problema (eSpeak a AUDIO_SAMPLE_RATE), solo que interpolado en vez de
    // nearest-neighbor porque acá sobra CPU.
    private fun resampleLinear(input: ShortArray, fromRate: Int, toRate: Int): ShortArray {
        if (input.isEmpty() || fromRate == toRate) return input
        val outLen = (input.size.toLong() * toRate / fromRate).toInt()
        val out = ShortArray(outLen)
        val ratio = fromRate.toDouble() / toRate.toDouble()
        for (i in out.indices) {
            val srcPos = i * ratio
            val srcIndex = srcPos.toInt()
            val frac = srcPos - srcIndex
            val a = input[srcIndex]
            val b = if (srcIndex + 1 < input.size) input[srcIndex + 1] else a
            out[i] = (a + (b - a) * frac).toInt().toShort()
        }
        return out
    }

    private fun readLeInt(b: ByteArray, offset: Int): Int =
        (b[offset].toInt() and 0xFF) or
            ((b[offset + 1].toInt() and 0xFF) shl 8) or
            ((b[offset + 2].toInt() and 0xFF) shl 16) or
            ((b[offset + 3].toInt() and 0xFF) shl 24)

    private fun readLeShort(b: ByteArray, offset: Int): Int =
        (b[offset].toInt() and 0xFF) or ((b[offset + 1].toInt() and 0xFF) shl 8)

    // Corta la síntesis/locución en curso — para "parar reading". No alcanza
    // a vaciar lo que el ESP32 ya tiene encolado para reproducir (ver
    // s_tts_audio_queue en link.c); mismo límite que ya tenía tts_speak() en
    // el ESP32 (no se cancela a mitad de frase), documentado ahí.
    fun stop() {
        tts.stop()
    }

    fun close() {
        tts.stop()
        tts.shutdown()
        executor.shutdown()
    }
}
