package com.sentis.companion

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.latin.TextRecognizerOptions
import java.util.concurrent.Executors

// =============================================================================
// OcrTextRecognizer — ML Kit Text Recognition v2 (variante bundled, modelo
// Latin embebido en el APK) sobre el JPEG que manda el ESP32 en cada
// MSG_OCR_REQUEST (ver components/link/link.h: link_request_ocr_text
// bloquea del lado del firmware hasta que llega la respuesta).
//
// El decode de JPEG corre en un executor propio, no en el hilo que lee el
// socket (LinkClient.readLoop) — si el decode+OCR se hiciera ahí, el
// firmware no podría mandar el siguiente mensaje (audio incluido) hasta que
// termine, porque readLoop no vuelve a leer del socket hasta que la
// callback onOcrRequest retorna.
// =============================================================================

class OcrTextRecognizer(
    private val onLog: (String) -> Unit,
) {
    private val executor = Executors.newSingleThreadExecutor { r -> Thread(r, "ocr-recognizer") }
    private val recognizer = TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS)

    // onFrame entrega el bitmap ya decodificado antes de correr ML Kit, para
    // que la UI pueda mostrar el frame que la cámara del ESP32 capturó —
    // util para diagnosticar lecturas parciales ("Ho", "Horl" en vez de
    // "Hola": encuadre/enfoque, no un bug del pipeline).
    fun recognize(jpeg: ByteArray, onFrame: (Bitmap) -> Unit, onResult: (String) -> Unit) {
        executor.execute {
            val raw = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size)
            if (raw == null) {
                onLog("OCR: no se pudo decodificar el JPEG (${jpeg.size} bytes)")
                return@execute
            }
            // El firmware manda el frame girado 90° y espejado respecto a la
            // vista del usuario — mirror_rgb565_inplace en components/ocr/
            // ocr.cpp solo corrige el artefacto de binning del sensor (un
            // espejado vertical), no el montaje físico de la cámara. Rotación
            // + espejo confirmados contra una foto real (portada de libro,
            // texto legible y ML Kit exacto) en hardware real 2026-09-21 con
            // la preview de este mismo archivo.
            val bitmap = Bitmap.createBitmap(
                raw, 0, 0, raw.width, raw.height,
                Matrix().apply {
                    postRotate(90f)
                    postScale(-1f, 1f)
                },
                true,
            )
            onFrame(bitmap)
            val image = InputImage.fromBitmap(bitmap, 0)
            recognizer.process(image)
                .addOnSuccessListener { result -> onResult(result.text) }
                .addOnFailureListener { e -> onLog("Error de ML Kit: ${e.message}") }
        }
    }
}
