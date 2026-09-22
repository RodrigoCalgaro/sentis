package com.sentis.companion

import android.content.Context
import org.json.JSONObject
import org.vosk.Model
import org.vosk.Recognizer
import org.vosk.android.StorageService
import java.util.concurrent.Executors

// =============================================================================
// VoiceCommandRecognizer — Vosk (offline, modelo español chico en
// assets/model-es-small/) sobre el PCM mono 16kHz/16-bit que ya manda el
// ESP32 por MSG_AUDIO (ver components/link/link.h). No usa el micrófono del
// teléfono ni org.vosk.android.SpeechService (que graba con AudioRecord) —
// el audio ya llega hecho por LinkClient, así que se alimenta a mano con
// feedAudio().
//
// Misma tabla de comandos que interpreta on_link_command() en
// main/sentis.c:64 — si se agrega un comando nuevo hay que tocar los dos
// lados. Los IDs 1-5 de la vieja tabla en inglés (stt.c, retirada en la
// Fase 2) no se migran: nunca tuvieron un caso en el switch del firmware.
// =============================================================================

const val COMMAND_START_READING = 6
const val COMMAND_STOP_READING = 7
const val COMMAND_DETECT_COLOR = 8

// Registro voseo rioplatense, varias formas de decir cada comando para
// tolerar cómo lo pronuncie cada usuario — Vosk devuelve texto libre, no
// hay gramática cerrada que restrinja lo que puede reconocer.
private val START_READING_PHRASES = listOf(
    "leer", "lee esto", "leé esto", "empezar a leer", "lectura",
)
private val STOP_READING_PHRASES = listOf(
    "parar", "para", "dejá de leer", "deja de leer", "parar de leer", "basta",
)
private val DETECT_COLOR_PHRASES = listOf(
    "detectar color", "detecta color", "que color es", "qué color es",
    "de que color es esto", "de qué color es esto", "decime el color",
    "decime que color es",
)

private const val MODEL_ASSET_DIR = "model-es-small"
private const val MODEL_SAMPLE_RATE = 16000.0f

class VoiceCommandRecognizer(
    private val context: Context,
    private val onLog: (String) -> Unit,
    private val onModelReady: () -> Unit,
    private val onCommandDetected: (commandId: Int, phrase: String) -> Unit,
) {
    // Un solo hilo: Vosk no es thread-safe por instancia de Recognizer, y
    // acceptWaveForm es CPU-bound — nunca debe correr en el hilo que lee el
    // socket (LinkClient.readLoop) ni en el de UI.
    private val executor = Executors.newSingleThreadExecutor { r -> Thread(r, "voice-recognizer") }

    @Volatile private var model: Model? = null
    @Volatile private var recognizer: Recognizer? = null

    // Descarga (StorageService.sync) el modelo desde assets/ a almacenamiento
    // de la app y crea el Model de Vosk. Llamar una vez, desde onCreate.
    fun loadModel() {
        onLog("Cargando modelo de voz (Vosk, español)...")
        StorageService.unpack(
            context, MODEL_ASSET_DIR, "model",
            { model -> onModelLoaded(model) },
            { exception -> onLog("Error cargando modelo Vosk: ${exception.message}") },
        )
    }

    private fun onModelLoaded(loadedModel: Model) {
        executor.execute {
            try {
                model = loadedModel
                recognizer = Recognizer(loadedModel, MODEL_SAMPLE_RATE)
                onLog("Modelo de voz listo.")
                onModelReady()
            } catch (e: Exception) {
                onLog("Error creando Recognizer de Vosk: ${e.message}")
            }
        }
    }

    // Llamado desde LinkClient.readLoop cada vez que llega un chunk
    // MSG_AUDIO. Se descarta en silencio si el modelo todavía no cargó (los
    // primeros chunks tras conectar, mientras StorageService.unpack corre).
    fun feedAudio(pcm: ByteArray) {
        val r = recognizer ?: return
        executor.execute {
            try {
                if (r.acceptWaveForm(pcm, pcm.size)) {
                    handleFinalResult(r.result)
                }
            } catch (e: Exception) {
                onLog("Error en Vosk acceptWaveForm: ${e.message}")
            }
        }
    }

    private fun handleFinalResult(resultJson: String) {
        val text = try {
            JSONObject(resultJson).optString("text", "").trim().lowercase()
        } catch (e: Exception) {
            return
        }
        if (text.isEmpty()) return

        // Match contra el texto completo del resultado final, no
        // contains()/substring — con contains(), una frase corta como
        // "para" (parte de STOP_READING_PHRASES, pensando en "pará") matchea
        // dentro de cualquier oración que contenga esa palabra como
        // preposición ("director PARA la mozo..."). Confirmado en hardware
        // real 2026-09-21: Vosk transcribe ruido ambiente como oraciones
        // largas sin sentido, y contains() disparaba "parar" sin que nadie
        // dijera el comando. Con igualdad exacta, una oración larga de ruido
        // nunca coincide con una frase corta de comando.
        // Sin match no hay nada que loguear — la mayoria de los resultados
        // finales durante una sesion son ruido ambiente sin comando, y
        // logear cada uno ensucia el log sin aportar nada (pedido 2026-09-21).
        when {
            START_READING_PHRASES.any { text == it } ->
                onCommandDetected(COMMAND_START_READING, text)
            STOP_READING_PHRASES.any { text == it } ->
                onCommandDetected(COMMAND_STOP_READING, text)
            DETECT_COLOR_PHRASES.any { text == it } ->
                onCommandDetected(COMMAND_DETECT_COLOR, text)
        }
    }

    fun close() {
        executor.execute {
            recognizer?.close()
            model?.close()
        }
        executor.shutdown()
    }
}
