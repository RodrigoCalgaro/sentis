package com.sentis.companion

import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

// =============================================================================
// LinkClient — cliente del protocolo de components/link/link.h (firmware
// SENTIS), sobre el SoftAP que levanta components/wifi (192.168.4.1:3333).
// Reemplaza a tools/link_test_client.py para probar el protocolo desde el
// dispositivo real en vez de una PC (la PC no puede unirse a la red SENTIS
// sin perder su propia conexion a internet).
//
// Framing identico al firmware: magic(4 LE) + tipo(1) + pad(3) + tamano(4 LE)
// + payload, todo en little-endian (el ESP32-P4/RISC-V es little-endian
// nativo — el firmware NO hace conversion a network byte order).
// =============================================================================

private const val MAGIC = 0x314E4553  // "SEN1", igual que LINK_MAGIC en link.c
private const val HEADER_LEN = 12
private const val COMMAND_TEXT_MAX = 64

const val MSG_AUDIO = 1
const val MSG_OCR_REQUEST = 2
const val MSG_OCR_RESULT = 3
const val MSG_COMMAND = 4

class LinkClient(
    private val onLog: (String) -> Unit,
    private val onStatus: (String) -> Unit,
    private val onAudioChunk: (Int) -> Unit,
    private val onOcrRequest: (ByteArray) -> Unit,
) {
    @Volatile private var socket: Socket? = null
    @Volatile private var out: DataOutputStream? = null
    private val writeLock = Object()

    fun connect(host: String, port: Int) {
        thread(name = "link-client") {
            try {
                onStatus("Conectando a $host:$port...")
                val s = Socket(host, port)
                socket = s
                out = DataOutputStream(s.getOutputStream())
                onStatus("Conectado a $host:$port")
                onLog("Conectado.")
                readLoop(DataInputStream(s.getInputStream()))
            } catch (e: Exception) {
                onLog("Error de conexion: ${e.message}")
            } finally {
                disconnect()
            }
        }
    }

    fun disconnect() {
        try {
            socket?.close()
        } catch (_: Exception) {
            // ya estaba cerrado, no importa
        }
        socket = null
        out = null
        onStatus("Desconectado")
    }

    private fun readLoop(input: DataInputStream) {
        val header = ByteArray(HEADER_LEN)
        while (true) {
            try {
                input.readFully(header)
            } catch (e: Exception) {
                onLog("Conexion cerrada: ${e.message}")
                return
            }
            val buf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            val magic = buf.int
            val type = buf.get().toInt() and 0xFF
            buf.position(buf.position() + 3)  // pad
            val size = buf.int

            if (magic != MAGIC) {
                onLog("Magic invalido (0x${magic.toString(16)}), cierro conexion")
                return
            }

            val payload = if (size > 0) {
                val p = ByteArray(size)
                try {
                    input.readFully(p)
                } catch (e: Exception) {
                    onLog("Conexion cerrada leyendo payload: ${e.message}")
                    return
                }
                p
            } else {
                ByteArray(0)
            }

            when (type) {
                MSG_AUDIO -> onAudioChunk(payload.size)
                MSG_OCR_REQUEST -> onOcrRequest(payload)
                else -> onLog("Tipo de mensaje inesperado del ESP32: $type (${payload.size} bytes)")
            }
        }
    }

    // Los callers (botones de MainActivity) llaman a esto directo desde el
    // hilo de UI (onClickListener) — despachamos la escritura bloqueante a
    // un hilo aparte aca adentro, en vez de exigirle a cada caller que lo
    // haga, porque Android prohibe I/O de red en el hilo principal
    // (NetworkOnMainThreadException, que ademas no trae mensaje de texto:
    // "Error mandando (tipo=N): null" en el log es justamente esa excepcion,
    // no un problema del firmware ni de la conexion — confirmado en hardware
    // real 2026-09-21, la recepcion de audio funcionaba perfecto porque
    // readLoop() ya corria en background, pero start/stop reading y
    // responder OCR fallaban siempre por esto).
    private fun sendFramed(type: Int, payload: ByteArray) {
        thread(name = "link-send") {
            synchronized(writeLock) {
                val o = out
                if (o == null) {
                    onLog("No conectado, no se puede mandar (tipo=$type)")
                    return@thread
                }
                val header = ByteBuffer.allocate(HEADER_LEN).order(ByteOrder.LITTLE_ENDIAN)
                header.putInt(MAGIC)
                header.put(type.toByte())
                header.put(byteArrayOf(0, 0, 0))
                header.putInt(payload.size)
                try {
                    o.write(header.array())
                    o.write(payload)
                    o.flush()
                } catch (e: Exception) {
                    onLog("Error mandando (tipo=$type): ${e.message}")
                }
            }
        }
    }

    // Mismo layout que link_command_t en link.h: int32 command_id (LE) +
    // char text[64] (UTF-8, relleno con ceros).
    fun sendCommand(commandId: Int, text: String) {
        val textBytes = text.toByteArray(Charsets.UTF_8).copyOf(COMMAND_TEXT_MAX)
        val buf = ByteBuffer.allocate(4 + COMMAND_TEXT_MAX).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(commandId)
        buf.put(textBytes)
        sendFramed(MSG_COMMAND, buf.array())
        onLog("-> COMMAND id=$commandId texto=\"$text\"")
    }

    // Texto crudo UTF-8, sin estructura adicional (igual que send_ocr_result
    // en tools/link_test_client.py).
    fun sendOcrResult(text: String) {
        sendFramed(MSG_OCR_RESULT, text.toByteArray(Charsets.UTF_8))
        onLog("-> OCR_RESULT texto=\"$text\"")
    }
}
