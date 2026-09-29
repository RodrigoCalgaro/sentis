package com.sentis.companion

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.SeekBar
import android.widget.TextView

// =============================================================================
// MainActivity (flavor "internal") — UI de debug/testing de la app companion
// de components/link (firmware SENTIS). Toda la lógica de sesión vive en
// SentisController (compartida con el flavor "client"); esta Activity solo
// junta widgets y los conecta a los callbacks/métodos del controller.
//
// Expone además la vía manual de prueba (Fase 3): botones de start/stop
// reading, detectar color, sliders de settings y el campo de texto de OCR —
// Vosk/ML Kit/ColorDetector siguen siendo la vía automática, todas mandan por
// el mismo SentisController así que no hay conflicto entre usarlas.
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

    private lateinit var controller: SentisController

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

        controller = SentisController(
            context = this,
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
            onStatus = { status -> runOnUiThread { statusText.text = status } },
            onAudioStats = { chunks, bytes ->
                runOnUiThread { audioStatsText.text = "$chunks chunks, $bytes bytes" }
            },
            onVoiceStatus = { status -> runOnUiThread { voiceStatusText.text = status } },
            onFramePreview = { bitmap -> runOnUiThread { ocrPreviewImage.setImageBitmap(bitmap) } },
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

        voiceStatusText.text = "Vosk: cargando modelo..."

        findViewById<Button>(R.id.connectButton).setOnClickListener {
            val host = hostInput.text.toString().trim()
            val port = portInput.text.toString().toIntOrNull() ?: 3333
            controller.connect(host, port)
        }

        // Corta la conexión a pedido del usuario, sin esperar a cerrar la
        // app (antes solo pasaba en onDestroy).
        findViewById<Button>(R.id.disconnectButton).setOnClickListener {
            controller.disconnect()
        }

        findViewById<Button>(R.id.startReadingButton).setOnClickListener {
            controller.sendCommand(COMMAND_START_READING, "leer (manual)")
        }
        findViewById<Button>(R.id.stopReadingButton).setOnClickListener {
            controller.sendCommand(COMMAND_STOP_READING, "parar (manual)")
        }
        findViewById<Button>(R.id.detectColorButton).setOnClickListener {
            controller.sendCommand(COMMAND_DETECT_COLOR, "detectar color (manual)")
        }
        findViewById<Button>(R.id.startAlertsButton).setOnClickListener {
            controller.sendCommand(COMMAND_START_ALERTS, "iniciar alertas (manual)")
        }
        findViewById<Button>(R.id.stopAlertsButton).setOnClickListener {
            controller.sendCommand(COMMAND_STOP_ALERTS, "detener alertas (manual)")
        }
        findViewById<Button>(R.id.previewOnButton).setOnClickListener {
            controller.sendCommand(COMMAND_PREVIEW_ON, "preview on (manual)")
        }
        findViewById<Button>(R.id.previewOffButton).setOnClickListener {
            controller.sendCommand(COMMAND_PREVIEW_OFF, "preview off (manual)")
        }
        findViewById<Button>(R.id.ocrReplyButton).setOnClickListener {
            controller.sendOcrManualReply(ocrTextInput.text.toString())
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
                controller.sendSettingsSet(paramId, seekBar.progress)
            }
        })
    }

    override fun onDestroy() {
        controller.destroy()
        super.onDestroy()
    }
}
