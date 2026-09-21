package com.sentis.companion

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import androidx.annotation.RequiresApi

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

        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                onLog("Red SENTIS reservada (el resto del teléfono sigue con su conexión normal).")
                onResult(network)
            }

            override fun onUnavailable() {
                onLog("No se pudo reservar la red SENTIS vía WifiNetworkSpecifier.")
                onResult(null)
            }
        }
        callback = cb
        connectivityManager.requestNetwork(request, cb)
    }

    // Libera el pedido de red — sin esto, Android sigue intentando mantener
    // la conexión a SENTIS en background aunque la app ya no la use.
    fun release() {
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
