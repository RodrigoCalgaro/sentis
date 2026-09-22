package com.sentis.companion

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.SeekBar
import android.widget.TextView

// SoftAP fijo de components/wifi (ver sdkconfig: CONFIG_WIFI_SSID/PASSWORD).
// Hardcodeado igual que el host/puerto ya precargados en activity_main.xml —
// esta es la app de prueba, no un cliente genérico.
private const val SENTIS_SSID = "SENTIS"
private const val SENTIS_PASSWORD = "sentisglasses"

// =============================================================================
// MainActivity — app companion de components/link (firmware SENTIS).
// Conecta al SoftAP "SENTIS", reconoce comandos de voz sobre el PCM que
// manda el ESP32 (Vosk, ver VoiceCommandRecognizer), responde los pedidos de
// OCR sobre el JPEG que manda el ESP32 (ML Kit, ver OcrTextRecognizer) y
// sintetiza el texto reconocido con el TTS de Android (ver TtsSpeaker) — el
// PCM resultante se manda de vuelta al ESP32 (LinkClient.sendTtsAudioChunk)
// para que suene por el parlante de SENTIS, no el del teléfono. El ESP32 ya
// no habla el resultado del OCR con su propio TTS (components/ocr/ocr.cpp),
// solo lo sigue usando para mensajes que no dependen del celular (ej.
// "Sentis Encendido" al arrancar). En Android 10+, "Conectar" reserva la red
// SENTIS por SSID (SentisNetworkManager) y ata el socket a ella, para no
// perder la conectividad normal del resto del teléfono mientras se usa la
// app — decisión del usuario 2026-09-21.
//
// Los botones de start/stop reading, detectar color y el campo de texto de
// OCR quedan como vía manual de prueba (Fase 3) — Vosk/ML Kit/ColorDetector
// son la vía automática, todas mandan por el mismo LinkClient así que no hay
// conflicto entre usarlas. La detección de color no usa ningún modelo de IA
// (ver ColorDetector): color dominante por análisis HSV, decisión del
// usuario 2026-09-22 para no depender de API keys/HTTP externo, y porque el
// teléfono de prueba (Moto G20) no soporta Gemini Nano (sin AICore).
// =============================================================================
class MainActivity : Activity() {

    private lateinit var statusText: TextView
    private lateinit var logText: TextView
    private lateinit var audioStatsText: TextView
    private lateinit var voiceStatusText: TextView
    private lateinit var ocrPreviewImage: ImageView
    private lateinit var hostInput: EditText
    private lateinit var portInput: EditText
    private lateinit var ocrTextInput: EditText
    private lateinit var volumeSeekBar: SeekBar
    private lateinit var warnSeekBar: SeekBar
    private lateinit var alertSeekBar: SeekBar
    private lateinit var volumeValueText: TextView
    private lateinit var warnValueText: TextView
    private lateinit var alertValueText: TextView

    private var audioChunks = 0
    private var audioBytes = 0
    private var client: LinkClient? = null
    private var voiceRecognizer: VoiceCommandRecognizer? = null
    private var ocrRecognizer: OcrTextRecognizer? = null
    private var colorDetector: ColorDetector? = null
    private var ttsSpeaker: TtsSpeaker? = null
    private var sentisNetworkManager: SentisNetworkManager? = null

    // Último texto de OCR efectivamente hablado. ocr_task manda un pedido por
    // frame mientras dura la lectura — con la página quieta, frames
    // consecutivos suelen reconocer el mismo texto, y sin este chequeo cada
    // uno dispara una locución nueva del TTS de Android: la voz termina
    // sonando como la misma frase repetida sin pausas reales, no como una
    // lectura limpia (reportado en hardware real 2026-09-21). Se resetea al
    // arrancar una lectura nueva para no silenciar una página repetida en
    // una sesión distinta.
    private var lastOcrText: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        logText = findViewById(R.id.logText)
        audioStatsText = findViewById(R.id.audioStatsText)
        voiceStatusText = findViewById(R.id.voiceStatusText)
        ocrPreviewImage = findViewById(R.id.ocrPreviewImage)
        hostInput = findViewById(R.id.hostInput)
        portInput = findViewById(R.id.portInput)
        ocrTextInput = findViewById(R.id.ocrTextInput)
        volumeSeekBar = findViewById(R.id.volumeSeekBar)
        warnSeekBar = findViewById(R.id.warnSeekBar)
        alertSeekBar = findViewById(R.id.alertSeekBar)
        volumeValueText = findViewById(R.id.volumeValueText)
        warnValueText = findViewById(R.id.warnValueText)
        alertValueText = findViewById(R.id.alertValueText)

        ocrRecognizer = OcrTextRecognizer(
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
        )
        colorDetector = ColorDetector(
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
        )
        ttsSpeaker = TtsSpeaker(
            context = this,
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
            sendAudioChunk = { samples -> client?.sendTtsAudioChunk(samples) },
        )
        sentisNetworkManager = SentisNetworkManager(
            context = this,
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
        )

        client = LinkClient(
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
            onStatus = { status -> runOnUiThread { statusText.text = status } },
            onAudioChunk = { pcm ->
                audioChunks++
                audioBytes += pcm.size
                runOnUiThread { audioStatsText.text = "$audioChunks chunks, $audioBytes bytes" }
                voiceRecognizer?.feedAudio(pcm)
            },
            onOcrRequest = { jpeg ->
                runOnUiThread {
                    appendLog("Pedido de OCR recibido (${jpeg.size} bytes), reconociendo con ML Kit...")
                }
                ocrRecognizer?.recognize(
                    jpeg,
                    onFrame = { bitmap -> runOnUiThread { ocrPreviewImage.setImageBitmap(bitmap) } },
                    onResult = { text ->
                        // Repetido: igual hay que responder (el ESP32 bloquea
                        // esperando MSG_OCR_RESULT), pero con texto vacío para
                        // que ocr.cpp no llame a tts_speak() de nuevo.
                        if (text.isNotBlank() && text == lastOcrText) {
                            client?.sendOcrResult("")
                            return@recognize
                        }
                        if (text.isNotBlank()) {
                            runOnUiThread { appendLog("ML Kit: \"$text\"") }
                            lastOcrText = text
                            ttsSpeaker?.speak(text)
                        }
                        client?.sendOcrResult(text)
                    },
                )
            },
            onColorRequest = { jpeg ->
                runOnUiThread {
                    appendLog("Pedido de color recibido (${jpeg.size} bytes), analizando...")
                }
                colorDetector?.detect(
                    jpeg,
                    onFrame = { bitmap -> runOnUiThread { ocrPreviewImage.setImageBitmap(bitmap) } },
                    onResult = { colorName ->
                        ttsSpeaker?.speak("El color principal de la imagen es el $colorName")
                        client?.sendColorResult(colorName)
                    },
                )
            },
            onSettingsState = { volumePct, warnMm, alertMm ->
                runOnUiThread {
                    volumeSeekBar.progress = volumePct
                    volumeValueText.text = "Volumen: $volumePct%"
                    warnSeekBar.progress = warnMm
                    warnValueText.text = "Umbral precaución: $warnMm mm"
                    alertSeekBar.progress = alertMm
                    alertValueText.text = "Umbral alerta: $alertMm mm"
                }
            },
        )

        // Sliders de ajustes — mandan SETTINGS_SET recien al soltar (no en
        // cada pixel de arrastre), para no saturar el socket. El firmware
        // responde con MSG_SETTINGS_STATE (ver onSettingsState arriba), que
        // deja el slider en el valor real aplicado si el pedido fue
        // clampeado o rechazado (ver components/settings/settings.c).
        setupSettingSeekBar(volumeSeekBar, volumeValueText, SETTING_VOLUME) { "Volumen: $it%" }
        setupSettingSeekBar(warnSeekBar, warnValueText, SETTING_PROXIMITY_WARN_MM) { "Umbral precaución: $it mm" }
        setupSettingSeekBar(alertSeekBar, alertValueText, SETTING_PROXIMITY_ALERT_MM) { "Umbral alerta: $it mm" }

        voiceRecognizer = VoiceCommandRecognizer(
            context = this,
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
            onModelReady = { runOnUiThread { voiceStatusText.text = "Vosk: listo" } },
            onCommandDetected = { commandId, phrase ->
                runOnUiThread { appendLog("Vosk: comando detectado [$commandId] \"$phrase\"") }
                sendCommand(commandId, phrase)
            },
        )
        voiceStatusText.text = "Vosk: cargando modelo..."
        voiceRecognizer?.loadModel()

        findViewById<Button>(R.id.connectButton).setOnClickListener {
            val host = hostInput.text.toString().trim()
            val port = portInput.text.toString().toIntOrNull() ?: 3333
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                appendLog("Reservando la red SENTIS (sin tocar la conectividad del resto del teléfono)...")
                sentisNetworkManager?.requestSentisNetwork(SENTIS_SSID, SENTIS_PASSWORD) { network ->
                    runOnUiThread {
                        if (network != null) {
                            client?.connect(host, port, network)
                        } else {
                            appendLog("Sigo con la red WiFi actual del teléfono.")
                            client?.connect(host, port)
                        }
                    }
                }
            } else {
                // Sin WifiNetworkSpecifier (API < 29): depende de que el
                // usuario ya haya conectado el WiFi a mano, como siempre.
                client?.connect(host, port)
            }
        }

        findViewById<Button>(R.id.startReadingButton).setOnClickListener {
            sendCommand(COMMAND_START_READING, "leer (manual)")
        }
        findViewById<Button>(R.id.stopReadingButton).setOnClickListener {
            sendCommand(COMMAND_STOP_READING, "parar (manual)")
        }
        findViewById<Button>(R.id.detectColorButton).setOnClickListener {
            sendCommand(COMMAND_DETECT_COLOR, "detectar color (manual)")
        }
        findViewById<Button>(R.id.ocrReplyButton).setOnClickListener {
            val text = ocrTextInput.text.toString()
            if (text.isNotBlank()) {
                client?.sendOcrResult(text)
            }
        }
    }

    private fun appendLog(msg: String) {
        logText.append("$msg\n")
    }

    // Actualiza el label en vivo mientras se arrastra, y manda el SETTINGS_SET
    // solo al soltar (onStopTrackingTouch) — igual criterio que sendCommand:
    // evitar floodear el socket con un mensaje por cada pixel de movimiento.
    private fun setupSettingSeekBar(
        seekBar: SeekBar,
        valueText: TextView,
        paramId: Int,
        label: (Int) -> String,
    ) {
        seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar, progress: Int, fromUser: Boolean) {
                valueText.text = label(progress)
            }
            override fun onStartTrackingTouch(seekBar: SeekBar) {}
            override fun onStopTrackingTouch(seekBar: SeekBar) {
                client?.sendSettingsSet(paramId, seekBar.progress)
            }
        })
    }

    // "leer" arranca una lectura nueva — resetea el dedup para no silenciar
    // una página que ya se leyó en una sesión anterior. "parar" corta
    // cualquier locución del TTS de Android en curso — el usuario pidió
    // silencio ya, no que termine la frase que estaba a mitad de camino.
    private fun sendCommand(commandId: Int, text: String) {
        when (commandId) {
            COMMAND_START_READING -> lastOcrText = ""
            COMMAND_STOP_READING -> ttsSpeaker?.stop()
        }
        client?.sendCommand(commandId, text)
    }

    override fun onDestroy() {
        client?.disconnect()
        voiceRecognizer?.close()
        colorDetector?.close()
        ttsSpeaker?.close()
        sentisNetworkManager?.release()
        super.onDestroy()
    }
}
