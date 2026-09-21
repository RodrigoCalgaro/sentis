package com.sentis.companion

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView

// =============================================================================
// MainActivity — app minima de prueba para components/link (firmware
// SENTIS). Reemplaza a tools/link_test_client.py: conecta al SoftAP
// "SENTIS", muestra el audio sintetico y los pedidos de OCR que manda la
// tarea de AUTOTEST TEMPORAL de link.c, y permite mandar de vuelta un
// comando de voz o una respuesta de OCR a mano.
//
// No tiene Vosk ni ML Kit todavia — eso es un paso siguiente, una vez que
// el protocolo este validado end-to-end contra esta app real (en vez de la
// PC, que no puede unirse a la red SENTIS sin perder su propio internet).
// =============================================================================
class MainActivity : Activity() {

    private lateinit var statusText: TextView
    private lateinit var logText: TextView
    private lateinit var audioStatsText: TextView
    private lateinit var hostInput: EditText
    private lateinit var portInput: EditText
    private lateinit var ocrTextInput: EditText

    private var audioChunks = 0
    private var audioBytes = 0
    private var client: LinkClient? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        logText = findViewById(R.id.logText)
        audioStatsText = findViewById(R.id.audioStatsText)
        hostInput = findViewById(R.id.hostInput)
        portInput = findViewById(R.id.portInput)
        ocrTextInput = findViewById(R.id.ocrTextInput)

        client = LinkClient(
            onLog = { msg -> runOnUiThread { appendLog(msg) } },
            onStatus = { status -> runOnUiThread { statusText.text = status } },
            onAudioChunk = { size ->
                audioChunks++
                audioBytes += size
                runOnUiThread { audioStatsText.text = "$audioChunks chunks, $audioBytes bytes" }
            },
            onOcrRequest = { payload ->
                runOnUiThread {
                    appendLog(
                        "Pedido de OCR recibido (${payload.size} bytes) — " +
                            "escribi el texto y toca 'Responder OCR'"
                    )
                }
            },
        )

        findViewById<Button>(R.id.connectButton).setOnClickListener {
            val host = hostInput.text.toString().trim()
            val port = portInput.text.toString().toIntOrNull() ?: 3333
            client?.connect(host, port)
        }

        findViewById<Button>(R.id.startReadingButton).setOnClickListener {
            client?.sendCommand(6, "start reading")
        }
        findViewById<Button>(R.id.stopReadingButton).setOnClickListener {
            client?.sendCommand(7, "stop reading")
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

    override fun onDestroy() {
        client?.disconnect()
        super.onDestroy()
    }
}
