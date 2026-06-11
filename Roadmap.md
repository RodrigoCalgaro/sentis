# Cronograma de Desarrollo Incremental: Proyecto SENTIS

## Fase 1: El Sistema Nervioso (Salidas Hápticas y Base)

Antes de que los anteojos "vean" o "hablen", deben aprender a "tocar". Esta fase te servirá para familiarizarte con los tiempos de ejecución (loops) y los pines de la placa.

*   **Paso 1.1:** Configurar los pines GPIO para controlar los dos motores de vibración tipo moneda (izquierdo y derecho).
*   **Paso 1.2:** Crear patrones de vibración: intermitente izquierdo (peligro izquierda), intermitente derecho (peligro derecha) y continuo/intenso (freno de mano absoluto por obstáculo frontal).

## Fase 2: La Voz de SENTIS (Almacenamiento y Audio)

Un sistema de asistencia necesita comunicarse con claridad. Aquí daremos de alta el almacenamiento y los componentes de sonido de la unidad central.

*   **Paso 2.1:** Inicializar el lector de tarjetas microSD para poder leer y escribir archivos.
*   **Paso 2.2:** Configurar el parlante de 2W utilizando el convertidor de audio integrado para reproducir archivos .wav simples desde la microSD (por ejemplo, un pitido de alerta o audios pregrabados como "Obstáculo detectado").

## Fase 3: El Escudo de Distancia (Integración del LiDAR)

Aquí creamos la primera función de seguridad real combinando la Fase 1 y la Fase 2. El LiDAR será el encargado de medir distancias en tiempo real.

*   **Paso 3.1:** Comunicar el sensor LiDAR con la placa principal (normalmente mediante protocolo I2C o UART) y calibrar las lecturas en centímetros.
*   **Paso 3.2:** Fusión Lógica Temprana: Si la distancia del LiDAR es menor a 1.5 metros, activar una vibración suave. Si es menor a 50 cm, disparar la vibración intensa y reproducir la alerta por el parlante.

## Fase 4: La Escucha Activa (Micrófono I2S)

Para que el usuario active funciones por comandos de voz, necesitamos capturar audio limpio.

*   **Paso 4.1:** Configurar el micrófono INMP441 mediante el protocolo I2S (una interfaz digital de audio nativa muy eficiente en el ESP32).
*   **Paso 4.2:** Grabar una pequeña muestra de voz del micrófono directamente a la tarjeta microSD para comprobar que el audio se escucha nítido y sin ruidos parásitos.

## Fase 5: Abriendo los Ojos (Cámara MIPI CSI)

El ESP32-P4 destaca precisamente por su interfaz MIPI CSI de alta velocidad. Vamos a encender la cámara de 5MP.

*   **Paso 5.1:** Inicializar la cámara, configurar la resolución y el formato de buffer adecuado para no agotar la memoria.
*   **Paso 5.2:** Capturar una fotografía al presionar un botón de prueba (o simular un comando) y guardarla como .jpg en la microSD.

## Fase 6: El Cerebro Inteligente (Edge AI Local)

Esta es la fase más compleja y bonita, donde aprovechamos los 400 MHz del procesador dual-core y los 32 MB de PSRAM para procesar las imágenes de forma 100% local (sin internet). Irás de lo más fácil a lo más difícil:

*   **Paso 6.1:** Detección de Colores: Analizar los píxeles centrales de la foto para identificar el color predominante y decirlo por el parlante.
*   **Paso 6.2:** Reconocimiento de Billetes: Implementar un modelo ligero de clasificación (usando Espressif Eye o TinyML) entrenado con los billetes argentinos en circulación.
*   **Paso 6.3:** Lectura de Texto (OCR): Integrar una librería de reconocimiento óptico de caracteres para extraer texto de carteles o recetas y transformarlo en voz.

## Fase 7: Navegación y Protocolo de Emergencia

*   **Paso 7.1:** Integrar el módulo GPS y la lógica de asistencia por voz para guiar el camino.
*   **Paso 7.2:** Configurar el coprocesador ESP32-C6 (Wi-Fi/Bluetooth) para interactuar con la red en caso de activar el modo de emergencia (por ejemplo, enviar un SMS de alerta o coordenadas ante una caída estrepitosa).