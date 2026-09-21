plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.sentis.companion"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.sentis.companion"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1-link-test"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    // El modelo de Vosk (assets/model-es-small/, ~58 MB) no debe ir por el
    // pipeline de compresion de aapt: son archivos binarios propios de Kaldi,
    // no recursos comprimibles, y aapt2 puede fallar o inflar el APK al
    // intentar comprimirlos igual que un asset comun.
    androidResources {
        noCompress += listOf("")
    }
}

dependencies {
    // Vosk — reconocimiento de voz offline (Fase 3), alimentado a mano con
    // el PCM 16kHz/16-bit que ya manda el ESP32 por LinkClient (no usa el
    // microfono del telefono, asi que no se usa vosk-android's SpeechService).
    implementation("com.alphacephei:vosk-android:0.3.75")

    // ML Kit Text Recognition v2, variante bundled: el modelo Latin queda
    // dentro del APK (~4 MB) en vez de descargarse via Play Services, para
    // que el OCR funcione offline desde el primer arranque en el telefono
    // de prueba (decision del usuario 2026-09-21).
    implementation("com.google.mlkit:text-recognition:16.0.1")
}
