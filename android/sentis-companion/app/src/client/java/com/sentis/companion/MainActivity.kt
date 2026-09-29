package com.sentis.companion

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.TextView
import kotlin.math.roundToInt

private const val SENTIS_HOST = "192.168.4.1"
private const val SENTIS_PORT = 3333
private const val TAG = "SentisClient"

// El parlante de SENTIS solo funciona bien entre 50% y 80% de volumen real
// (fuera de ese rango, distorsiona o casi no se escucha) — en vez de exponer
// ese 50-80% crudo, la UI lo normaliza a una escala de 1 a 10 (pedido del
// usuario 2026-09-28), más simple de ajustar a los saltos con los botones -/+.
private const val VOLUME_STEPS = 10
private const val VOLUME_MIN_PERCENT = 50
private const val VOLUME_MAX_PERCENT = 80
private const val DEFAULT_VOLUME_STEP = 7 // ~70%, mismo default que tenía el slider crudo

private fun volumeStepToPercent(step: Int): Int {
    val fraction = (step - 1).toFloat() / (VOLUME_STEPS - 1)
    return (VOLUME_MIN_PERCENT + fraction * (VOLUME_MAX_PERCENT - VOLUME_MIN_PERCENT)).roundToInt()
}

private fun percentToVolumeStep(percent: Int): Int {
    val clamped = percent.coerceIn(VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT)
    val fraction = (clamped - VOLUME_MIN_PERCENT).toFloat() / (VOLUME_MAX_PERCENT - VOLUME_MIN_PERCENT)
    return (1 + fraction * (VOLUME_STEPS - 1)).roundToInt()
}

// Umbrales de vibración por proximidad — rangos recomendados de uso (más
// angostos que el clamp real del firmware, components/settings/settings.h,
// que sigue aceptando 300-4000mm/100-2000mm para no romper al flavor
// internal) y saltos de 5cm por toque, pedido del usuario 2026-09-28.
private const val DISTANCE_STEP_CM = 5
private const val WARN_MIN_CM = 150
private const val WARN_MAX_CM = 250
private const val WARN_DEFAULT_CM = 200
private const val ALERT_MIN_CM = 50
private const val ALERT_MAX_CM = 150
private const val ALERT_DEFAULT_CM = 100

// =============================================================================
// MainActivity (flavor "client") — UI para el usuario final: splash mientras
// cargan Vosk/TTS, pantalla de conexión sin exponer IP/puerto, y ajustes
// (volumen, umbrales de proximidad) una vez conectado. Sin controles de
// debug (log, contadores, disparo manual de comandos, preview de OCR) — esos
// quedan exclusivos del flavor "internal"; acá el uso real es por voz sobre
// los propios anteojos, el teléfono es solo para emparejar y ajustar.
//
// Una sola Activity que cambia de pantalla con setContentView() (sin
// Fragments/Navigation, no se usan en este proyecto) — SentisController se
// crea una única vez acá y sobrevive a los cambios de pantalla, así el
// socket/Vosk/TTS no se reinician al navegar.
//
// Accesibilidad: producto para personas no videntes, requisito no
// negociable. Cada cambio de pantalla y cada cambio de estado (conectando,
// error, desconexión) se anuncia explícitamente con announceForAccessibility
// — un usuario ciego no recibe ninguna señal de un cambio visual/de color
// por sí solo.
// =============================================================================
class MainActivity : Activity() {

    private enum class Screen { SPLASH, CONNECT, SETTINGS }

    private lateinit var controller: SentisController
    private var currentScreen: Screen = Screen.SPLASH

    // Nulleables: se recrean en cada showSettings() (la vista anterior queda
    // destruida al volver a hacer setContentView), y onSettingsState puede
    // llegar en cualquier momento — el guard de currentScreen en
    // applySettingsState() evita tocar vistas de una pantalla que ya no está.
    private var volumeValueText: TextView? = null
    private var volumeDownButton: Button? = null
    private var volumeUpButton: Button? = null
    private var warnValueText: TextView? = null
    private var warnDownButton: Button? = null
    private var warnUpButton: Button? = null
    private var alertValueText: TextView? = null
    private var alertDownButton: Button? = null
    private var alertUpButton: Button? = null

    // Los tres ajustes son steppers de solo dos botones (sin SeekBar) — el
    // valor actual de cada uno se guarda acá.
    private var volumeStep: Int = DEFAULT_VOLUME_STEP
    private var warnCm: Int = WARN_DEFAULT_CM
    private var alertCm: Int = ALERT_DEFAULT_CM

    override fun onCreate(savedInstanceState: Bundle?) {
        setTheme(R.style.SentisClientTheme)
        super.onCreate(savedInstanceState)

        controller = SentisController(
            context = this,
            onLog = { msg -> Log.d(TAG, msg) },
            onStatus = {},
            onSettingsState = { volumePct, warnMm, alertMm ->
                runOnUiThread { applySettingsState(volumePct, warnMm, alertMm) }
            },
            onConnectionEvent = { event ->
                runOnUiThread { handleConnectionEvent(event) }
            },
            onModelsReady = {
                runOnUiThread { if (currentScreen == Screen.SPLASH) showConnect() }
            },
        )

        showSplash()
    }

    private fun showSplash() {
        currentScreen = Screen.SPLASH
        setContentView(R.layout.screen_splash)
        val root = findViewById<View>(R.id.splashRoot)
        root.post { root.announceForAccessibility(getString(R.string.splash_announcement)) }
    }

    private fun showConnect(announcement: String? = null) {
        currentScreen = Screen.CONNECT
        setContentView(R.layout.screen_connect)

        val connectButton = findViewById<Button>(R.id.connectButton)
        connectButton.setOnClickListener {
            connectButton.isEnabled = false
            connectButton.text = getString(R.string.connect_connecting)
            connectButton.announceForAccessibility(getString(R.string.connect_connecting_announcement))
            controller.connect(SENTIS_HOST, SENTIS_PORT)
        }

        val toAnnounce = announcement ?: getString(R.string.connect_screen_announcement)
        connectButton.post { connectButton.announceForAccessibility(toAnnounce) }
    }

    private fun showSettings() {
        currentScreen = Screen.SETTINGS
        setContentView(R.layout.screen_settings)

        volumeValueText = findViewById(R.id.volumeValueText)
        volumeDownButton = findViewById(R.id.volumeDownButton)
        volumeUpButton = findViewById(R.id.volumeUpButton)
        warnValueText = findViewById(R.id.warnValueText)
        warnDownButton = findViewById(R.id.warnDownButton)
        warnUpButton = findViewById(R.id.warnUpButton)
        alertValueText = findViewById(R.id.alertValueText)
        alertDownButton = findViewById(R.id.alertDownButton)
        alertUpButton = findViewById(R.id.alertUpButton)

        setupVolumeStepper()
        setupDistanceStepper(
            downButton = warnDownButton, upButton = warnUpButton, valueText = warnValueText,
            paramId = SETTING_PROXIMITY_WARN_MM,
            minCm = WARN_MIN_CM, maxCm = WARN_MAX_CM,
            label = getString(R.string.setting_warn_label),
            getCm = { warnCm }, setCm = { warnCm = it },
        )
        setupDistanceStepper(
            downButton = alertDownButton, upButton = alertUpButton, valueText = alertValueText,
            paramId = SETTING_PROXIMITY_ALERT_MM,
            minCm = ALERT_MIN_CM, maxCm = ALERT_MAX_CM,
            label = getString(R.string.setting_alert_label),
            getCm = { alertCm }, setCm = { alertCm = it },
        )

        val disconnectButton = findViewById<Button>(R.id.disconnectButton)
        disconnectButton.setOnClickListener {
            // No navegamos acá directo: LinkClient emite
            // ConnectionEvent.Disconnected(userInitiated=true), que vuelve
            // por handleConnectionEvent() — mismo camino que un corte
            // inesperado, así hay un solo lugar que decide "volver a Conectar".
            controller.disconnect()
        }

        val root = findViewById<View>(R.id.settingsRoot)
        root.post { root.announceForAccessibility(getString(R.string.settings_screen_announcement)) }
    }

    private fun applySettingsState(volumePct: Int, warnMm: Int, alertMm: Int) {
        if (currentScreen != Screen.SETTINGS) return
        volumeStep = percentToVolumeStep(volumePct)
        updateVolumeDisplay()
        warnCm = (warnMm / 10).coerceIn(WARN_MIN_CM, WARN_MAX_CM)
        updateDistanceDisplay(warnValueText, warnDownButton, warnUpButton, warnCm, WARN_MIN_CM, WARN_MAX_CM, getString(R.string.setting_warn_label))
        alertCm = (alertMm / 10).coerceIn(ALERT_MIN_CM, ALERT_MAX_CM)
        updateDistanceDisplay(alertValueText, alertDownButton, alertUpButton, alertCm, ALERT_MIN_CM, ALERT_MAX_CM, getString(R.string.setting_alert_label))
    }

    private fun handleConnectionEvent(event: ConnectionEvent) {
        when (event) {
            is ConnectionEvent.Connected -> {
                if (currentScreen == Screen.CONNECT) showSettings()
            }
            is ConnectionEvent.ConnectFailed -> {
                if (currentScreen == Screen.CONNECT) {
                    val connectButton = findViewById<Button>(R.id.connectButton)
                    connectButton.isEnabled = true
                    connectButton.text = getString(R.string.connect_button)
                    connectButton.announceForAccessibility(getString(R.string.connect_failed_announcement))
                }
            }
            is ConnectionEvent.Disconnected -> {
                if (currentScreen == Screen.SETTINGS) {
                    val message = getString(
                        if (event.userInitiated) R.string.disconnected_user_announcement
                        else R.string.disconnected_unexpected_announcement,
                    )
                    showConnect(announcement = message)
                }
            }
        }
    }

    // Stepper de volumen (1-10, mapeado internamente a 50-80% real): cada tap
    // manda el SETTINGS_SET de una, no hay "soltar" como en un slider — cada
    // tap ya es una decisión discreta y completa del usuario.
    private fun setupVolumeStepper() {
        updateVolumeDisplay()
        volumeDownButton?.setOnClickListener { changeVolumeStep(-1) }
        volumeUpButton?.setOnClickListener { changeVolumeStep(1) }
    }

    private fun changeVolumeStep(delta: Int) {
        val newStep = (volumeStep + delta).coerceIn(1, VOLUME_STEPS)
        if (newStep == volumeStep) return
        volumeStep = newStep
        updateVolumeDisplay()
        controller.sendSettingsSet(SETTING_VOLUME, volumeStepToPercent(volumeStep))
    }

    // El TextView del valor tiene accessibilityLiveRegion="polite" (ver
    // screen_settings.xml) — actualizar su texto/contentDescription alcanza
    // para que TalkBack lo anuncie solo, sin necesidad de foco ni de un
    // announceForAccessibility manual.
    private fun updateVolumeDisplay() {
        volumeValueText?.text = volumeStep.toString()
        volumeValueText?.contentDescription = getString(R.string.volume_value_description, volumeStep, VOLUME_STEPS)
        volumeDownButton?.isEnabled = volumeStep > 1
        volumeUpButton?.isEnabled = volumeStep < VOLUME_STEPS
    }

    // Stepper de umbral de proximidad (precaución/alerta), en cm, saltos de
    // DISTANCE_STEP_CM — mismo criterio que el stepper de volumen: cada tap
    // ya es una decisión completa, se manda el SETTINGS_SET de una (en mm,
    // que es lo que espera el protocolo). getCm/setCm leen y escriben el
    // campo (warnCm o alertCm) que guarda el valor actual de este stepper.
    private fun setupDistanceStepper(
        downButton: Button?,
        upButton: Button?,
        valueText: TextView?,
        paramId: Int,
        minCm: Int,
        maxCm: Int,
        label: String,
        getCm: () -> Int,
        setCm: (Int) -> Unit,
    ) {
        updateDistanceDisplay(valueText, downButton, upButton, getCm(), minCm, maxCm, label)
        downButton?.setOnClickListener {
            changeDistanceCm(-DISTANCE_STEP_CM, minCm, maxCm, getCm, setCm) { newCm ->
                updateDistanceDisplay(valueText, downButton, upButton, newCm, minCm, maxCm, label)
                controller.sendSettingsSet(paramId, newCm * 10)
            }
        }
        upButton?.setOnClickListener {
            changeDistanceCm(DISTANCE_STEP_CM, minCm, maxCm, getCm, setCm) { newCm ->
                updateDistanceDisplay(valueText, downButton, upButton, newCm, minCm, maxCm, label)
                controller.sendSettingsSet(paramId, newCm * 10)
            }
        }
    }

    private fun changeDistanceCm(
        delta: Int,
        minCm: Int,
        maxCm: Int,
        getCm: () -> Int,
        setCm: (Int) -> Unit,
        onChanged: (Int) -> Unit,
    ) {
        val newCm = (getCm() + delta).coerceIn(minCm, maxCm)
        if (newCm == getCm()) return
        setCm(newCm)
        onChanged(newCm)
    }

    // El TextView del valor tiene accessibilityLiveRegion="polite" (ver
    // screen_settings.xml) — actualizar su texto/contentDescription alcanza
    // para que TalkBack lo anuncie solo, igual que el stepper de volumen.
    private fun updateDistanceDisplay(
        valueText: TextView?,
        downButton: Button?,
        upButton: Button?,
        cm: Int,
        minCm: Int,
        maxCm: Int,
        label: String,
    ) {
        valueText?.text = getString(R.string.settings_value_cm, cm)
        valueText?.contentDescription =
            getString(R.string.stepper_value_description, label, getString(R.string.settings_value_cm, cm))
        downButton?.isEnabled = cm > minCm
        upButton?.isEnabled = cm < maxCm
    }

    override fun onDestroy() {
        controller.destroy()
        super.onDestroy()
    }
}
