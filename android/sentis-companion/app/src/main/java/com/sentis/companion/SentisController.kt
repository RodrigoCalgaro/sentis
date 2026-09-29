package com.sentis.companion

import android.content.Context
import android.graphics.Bitmap
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

// SoftAP fijo de components/wifi (ver sdkconfig: CONFIG_WIFI_SSID/PASSWORD).
private const val SENTIS_SSID = "SENTIS"
private const val SENTIS_PASSWORD = "sentisglasses"

// Cota para onModelsReady: si Vosk y/o el TTS de Android no terminan de
// inicializar en este tiempo, se avisa igual — un consumidor (ej. la pantalla
// de splash del flavor client) no puede depender indefinidamente de un
// callback de background que quizás nunca llegue (falla de storage, telefono
// sin motor TTS instalado, etc.).
private const val MODELS_READY_TIMEOUT_MS = 15_000L

// =============================================================================
// SentisController — toda la lógica de sesión con el firmware SENTIS
// (conectar, comandos de voz/manuales, OCR, color, TTS, settings), sin
// depender de ninguna UI concreta. Extraído de MainActivity para que tanto la
// UI de debug (flavor "internal") como la UI de cliente final (flavor
// "client") lo compartan sin duplicar wiring — ver
// android/sentis-companion/app/build.gradle.kts para los flavors.
//
// Igual que LinkClient y el resto de los helpers de este paquete, no hace
// runOnUiThread por su cuenta: todos los callbacks (onLog, onStatus, etc.)
// pueden llegar desde hilos de background (socket, executor de Vosk/ML Kit,
// NetworkCallback), y es responsabilidad de quien los pasa (la Activity)
// saltar al hilo de UI si va a tocar vistas.
// =============================================================================
class SentisController(
    private val context: Context,
    private val onLog: (String) -> Unit,
    private val onStatus: (String) -> Unit,
    private val onAudioStats: (chunks: Int, bytes: Int) -> Unit = { _, _ -> },
    private val onVoiceStatus: (String) -> Unit = {},
    private val onFramePreview: (Bitmap) -> Unit = {},
    private val onSettingsState: (volumePct: Int, warnMm: Int, alertMm: Int) -> Unit = { _, _, _ -> },
    private val onConnectionEvent: (ConnectionEvent) -> Unit = {},
    private val onModelsReady: () -> Unit = {},
) {
    private var audioChunks = 0
    private var audioBytes = 0

    // Último texto de OCR efectivamente hablado — ver dedup en el
    // onOcrRequest de más abajo (mismo comentario que tenía MainActivity).
    private var lastOcrText: String = ""

    private val modelsReadyLock = Object()
    @Volatile private var voiceReady = false
    @Volatile private var ttsReady = false
    @Volatile private var modelsReadyFired = false
    private val modelsReadyHandler = Handler(Looper.getMainLooper())

    private val previewExecutor = Executors.newSingleThreadExecutor { r -> Thread(r, "preview-decoder") }
    private val previewDecoding = AtomicBoolean(false)

    private lateinit var client: LinkClient
    private val ocrRecognizer = OcrTextRecognizer(onLog = onLog)
    private val colorDetector = ColorDetector(onLog = onLog)
    private val ttsSpeaker = TtsSpeaker(
        context = context,
        onLog = onLog,
        sendAudioChunk = { samples -> client.sendTtsAudioChunk(samples) },
        onReady = {
            ttsReady = true
            maybeSignalModelsReady()
        },
    )
    private val sentisNetworkManager = SentisNetworkManager(context = context, onLog = onLog)
    private val voiceRecognizer = VoiceCommandRecognizer(
        context = context,
        onLog = onLog,
        onModelReady = {
            onVoiceStatus("Vosk: listo")
            voiceReady = true
            maybeSignalModelsReady()
        },
        onCommandDetected = { commandId, phrase ->
            onLog("Vosk: comando detectado [$commandId] \"$phrase\"")
            sendCommand(commandId, phrase)
        },
    )

    init {
        client = LinkClient(
            onLog = onLog,
            onStatus = onStatus,
            onAudioChunk = { pcm ->
                audioChunks++
                audioBytes += pcm.size
                onAudioStats(audioChunks, audioBytes)
                voiceRecognizer.feedAudio(pcm)
            },
            onOcrRequest = { jpeg ->
                onLog("Pedido de OCR recibido (${jpeg.size} bytes), reconociendo con ML Kit...")
                ocrRecognizer.recognize(
                    jpeg,
                    onFrame = { bitmap -> onFramePreview(bitmap) },
                    onResult = { text ->
                        // Repetido: igual hay que responder (el ESP32 bloquea
                        // esperando MSG_OCR_RESULT), pero con texto vacío para
                        // que ocr.cpp no llame a tts_speak() de nuevo.
                        if (text.isNotBlank() && text == lastOcrText) {
                            client.sendOcrResult("")
                            return@recognize
                        }
                        if (text.isNotBlank()) {
                            onLog("ML Kit: \"$text\"")
                            lastOcrText = text
                            ttsSpeaker.speak(text)
                        }
                        client.sendOcrResult(text)
                    },
                )
            },
            onColorRequest = { jpeg ->
                onLog("Pedido de color recibido (${jpeg.size} bytes), analizando...")
                colorDetector.detect(
                    jpeg,
                    onFrame = { bitmap -> onFramePreview(bitmap) },
                    onResult = { colorName ->
                        ttsSpeaker.speak("El color principal de la imagen es el $colorName")
                        client.sendColorResult(colorName)
                    },
                )
            },
            onPreviewFrame = { jpeg ->
                // Descartar si todavía se está decodificando el anterior: el
                // hilo del socket no puede esperar (también trae el audio del
                // micrófono para Vosk), y en una preview un frame viejo no
                // sirve de nada.
                if (previewDecoding.compareAndSet(false, true)) {
                    previewExecutor.execute {
                        try {
                            decodeSentisFrame(jpeg)?.let { onFramePreview(it) }
                        } finally {
                            previewDecoding.set(false)
                        }
                    }
                }
            },
            onSettingsState = onSettingsState,
            onConnectionEvent = onConnectionEvent,
        )

        onVoiceStatus("Vosk: cargando modelo...")
        voiceRecognizer.loadModel()

        modelsReadyHandler.postDelayed({
            synchronized(modelsReadyLock) {
                if (!modelsReadyFired) {
                    modelsReadyFired = true
                    onLog(
                        "Tiempo de espera agotado cargando modelos " +
                            "(vosk=$voiceReady, tts=$ttsReady) — continuo igual.",
                    )
                    onModelsReady()
                }
            }
        }, MODELS_READY_TIMEOUT_MS)
    }

    private fun maybeSignalModelsReady() {
        synchronized(modelsReadyLock) {
            if (!modelsReadyFired && voiceReady && ttsReady) {
                modelsReadyFired = true
                modelsReadyHandler.removeCallbacksAndMessages(null)
                onModelsReady()
            }
        }
    }

    // En Android 10+, reserva la red SENTIS por SSID (SentisNetworkManager) y
    // ata el socket a ella, para no perder la conectividad normal del resto
    // del teléfono mientras se usa la app. En versiones viejas depende de que
    // el usuario ya haya conectado el WiFi a mano, como siempre.
    fun connect(host: String, port: Int) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            onLog("Reservando la red SENTIS (sin tocar la conectividad del resto del teléfono)...")
            sentisNetworkManager.requestSentisNetwork(SENTIS_SSID, SENTIS_PASSWORD) { network ->
                if (network != null) {
                    client.connect(host, port, network)
                } else {
                    onLog("Sigo con la red WiFi actual del teléfono.")
                    client.connect(host, port)
                }
            }
        } else {
            client.connect(host, port)
        }
    }

    // Libera también la reserva de red de SentisNetworkManager — si no,
    // Android sigue intentando mantener la conexión a SENTIS en background
    // aunque el LinkClient ya no la esté usando.
    fun disconnect() {
        client.disconnect()
        sentisNetworkManager.release()
    }

    // "leer" arranca una lectura nueva — resetea el dedup para no silenciar
    // una página que ya se leyó en una sesión anterior. "parar" corta
    // cualquier locución del TTS de Android en curso.
    fun sendCommand(commandId: Int, text: String) {
        when (commandId) {
            COMMAND_START_READING -> lastOcrText = ""
            COMMAND_STOP_READING -> ttsSpeaker.stop()
            // Confirmación hablada: con las alertas apagadas el usuario tiene
            // que saber que el bastón no lo va a avisar de obstáculos.
            COMMAND_START_ALERTS -> ttsSpeaker.speak("Alertas activadas")
            COMMAND_STOP_ALERTS -> ttsSpeaker.speak("Alertas desactivadas")
        }
        client.sendCommand(commandId, text)
    }

    fun sendSettingsSet(paramId: Int, value: Int) {
        client.sendSettingsSet(paramId, value)
    }

    fun sendOcrManualReply(text: String) {
        if (text.isNotBlank()) {
            client.sendOcrResult(text)
        }
    }

    fun destroy() {
        modelsReadyHandler.removeCallbacksAndMessages(null)
        client.disconnect()
        previewExecutor.shutdown()
        voiceRecognizer.close()
        colorDetector.close()
        ttsSpeaker.close()
        sentisNetworkManager.release()
    }
}
