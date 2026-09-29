package com.sentis.companion

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.annotation.RequiresApi

// requestNetwork(request, callback) de 2 argumentos NO garantiza que
// onUnavailable() se dispare en todos los casos — confirmado en hardware real
// 2026-09-28: si el selector de red del sistema se cancela solo (SENTIS
// apagado o fuera de rango), ni onAvailable ni onUnavailable llegan nunca.
// Sin una cota propia, el botón "Conectando..." de la UI quedaría
// inutilizable para siempre — inaceptable en una app para usuarios ciegos.
private const val NETWORK_REQUEST_TIMEOUT_MS = 15_000L

// =============================================================================
// SentisNetworkManager — pide conexión específica al SoftAP "SENTIS" sin
// cambiar la red default del teléfono (Android 10+, WifiNetworkSpecifier).
// LinkClient ata su Socket al Network que entrega acá, así el resto del
// teléfono (otras apps, tráfico en background) sigue usando datos móviles u
// otra WiFi como ruta default en vez de perder internet — decisión del
// usuario 2026-09-21. En versiones viejas (<29) esta clase no se usa: la app
// sigue dependiendo de que el usuario haya conectado el WiFi a mano, como
// hasta ahora (ver el chequeo de SDK_INT en MainActivity).
//
// removeCapability(NET_CAPABILITY_INTERNET) es necesario: por default todo
// NetworkRequest exige una red con internet validado, y el SoftAP de SENTIS
// nunca lo tiene — sin sacar esa capability el pedido nunca se satisface.
// =============================================================================

class SentisNetworkManager(
    context: Context,
    private val onLog: (String) -> Unit,
) {
    private val connectivityManager =
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private var callback: ConnectivityManager.NetworkCallback? = null
    private val timeoutHandler = Handler(Looper.getMainLooper())

    @RequiresApi(Build.VERSION_CODES.Q)
    fun requestSentisNetwork(ssid: String, password: String, onResult: (Network?) -> Unit) {
        release()

        val specifier = WifiNetworkSpecifier.Builder()
            .setSsid(ssid)
            .setWpa2Passphrase(password)
            .build()

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(specifier)
            .build()

        // Entrega el resultado una sola vez, venga de onAvailable,
        // onUnavailable, o de la cota propia de más abajo — evita un doble
        // onResult si dos de esas rutas se solapan.
        var resultDelivered = false
        fun deliverOnce(network: Network?, logMsg: String) {
            if (resultDelivered) return
            resultDelivered = true
            timeoutHandler.removeCallbacksAndMessages(null)
            onLog(logMsg)
            onResult(network)
        }

        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                deliverOnce(network, "Red SENTIS reservada (el resto del teléfono sigue con su conexión normal).")
            }

            override fun onUnavailable() {
                deliverOnce(null, "No se pudo reservar la red SENTIS vía WifiNetworkSpecifier.")
            }
        }
        callback = cb
        connectivityManager.requestNetwork(request, cb)

        timeoutHandler.postDelayed({
            try {
                connectivityManager.unregisterNetworkCallback(cb)
            } catch (_: IllegalArgumentException) {
                // ya estaba liberado
            }
            deliverOnce(null, "Tiempo de espera agotado reservando la red SENTIS.")
        }, NETWORK_REQUEST_TIMEOUT_MS)
    }

    // Libera el pedido de red — sin esto, Android sigue intentando mantener
    // la conexión a SENTIS en background aunque la app ya no la use.
    fun release() {
        timeoutHandler.removeCallbacksAndMessages(null)
        callback?.let {
            try {
                connectivityManager.unregisterNetworkCallback(it)
            } catch (_: IllegalArgumentException) {
                // ya estaba liberado
            }
        }
        callback = null
    }
}
