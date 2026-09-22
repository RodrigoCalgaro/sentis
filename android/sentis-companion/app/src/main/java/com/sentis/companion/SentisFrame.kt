package com.sentis.companion

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix

// =============================================================================
// SentisFrame — decodifica un JPEG mandado por el ESP32 (MSG_OCR_REQUEST,
// MSG_COLOR_REQUEST) y lo orienta como lo ve el usuario. Extraído de
// OcrTextRecognizer (que era el único consumidor hasta que se sumó
// ColorDetector) para no duplicar esta corrección en cada analizador nuevo.
// =============================================================================

// El firmware manda el frame girado 90° y espejado respecto a la vista del
// usuario — mirror_rgb565_inplace en components/ocr/ocr.cpp solo corrige el
// artefacto de binning del sensor (un espejado vertical), no el montaje
// físico de la cámara. Rotación + espejo confirmados contra una foto real
// (portada de libro, texto legible y ML Kit exacto) en hardware real
// 2026-09-21 con la preview de OcrTextRecognizer.
fun decodeSentisFrame(jpeg: ByteArray): Bitmap? {
    val raw = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size) ?: return null
    return Bitmap.createBitmap(
        raw, 0, 0, raw.width, raw.height,
        Matrix().apply {
            postRotate(90f)
            postScale(-1f, 1f)
        },
        true,
    )
}
