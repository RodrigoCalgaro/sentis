Análisis de la funcionalidad
Diagnóstico del estado actual
El componente haptics ya está preparado para la distinción izquierda/derecha: HAPTIC_PATTERN_PULSE_LEFT y HAPTIC_PATTERN_PULSE_RIGHT existen en el enum pero nunca se usan, porque sentis.c solo selecciona PULSE_BOTH o ALERT_BOTH. El cuello de botella es precisamente que el LiDAR no puede ubicar lateralmente al obstáculo.

Rol de cada sensor en el sistema fusionado
Sensor	Responsabilidad	Característica
LiDAR	Cuándo alertar (distancia)	Rápido, confiable, funciona de noche
Cámara + IA	Dónde está el obstáculo (lado)	Costoso en CPU, requiere luz
La fusión es clave: la cámara solo procesa cuando el LiDAR confirma que hay un obstáculo dentro del rango de interés. Así se evita gastar CPU en inferencia cuando no hay nada relevante.

Tabla de decisión háptica resultante
LiDAR	Cámara	Motor activado
Fuera de rango	—	OFF
Precaución	LEFT	PULSE_LEFT
Precaución	RIGHT	PULSE_RIGHT
Precaución	CENTER	PULSE_BOTH
Peligro (≤500mm)	cualquiera	ALERT_BOTH (seguridad prioritaria)
Cruce de fases del roadmap
Esta funcionalidad combina Fase 5 (cámara OV5647) y Fase 6 (Edge AI). Es un adelanto controlado: se trabaja la cámara en baja resolución para inferencia, no para captura de fotos de alta calidad.

Planificación paso a paso
Tarea 1 — Configuración del entorno para cámara
Qué involucra:

Agregar esp_cam_sensor al idf_component.yml (repositorio de componentes extra de Espressif, diferente al driver antiguo esp_camera de los ESP32-S3)
Agregar esp_driver_cam como dependencia en el CMakeLists.txt del proyecto
Configurar sdkconfig: habilitar PSRAM (los framebuffers de video son grandes, ~150 KB en QVGA RGB565), habilitar la interfaz MIPI CSI
Verificar que el proyecto compila sin periférico activo aún
Por qué es una tarea separada: Los cambios en sdkconfig y en el sistema de componentes pueden romper el build; es mejor aislarlos y verificarlos antes de escribir código.

Estimación: 1–2 días

Tarea 2 — Componente vision: inicialización de la cámara
Qué involucra:

Crear components/vision/vision.h con la API pública mínima: vision_init() y vision_get_obstacle_side()
Definir el enum obstacle_side_t { SIDE_NONE, SIDE_LEFT, SIDE_CENTER, SIDE_RIGHT }
Implementar vision_init(): probe I2C de la OV5647 (dirección 0x36, bus compartido con el codec ES8311), configurar el pipeline CSI-DMA en baja resolución (QVGA 320×240 o incluso QQVGA 160×120)
Loguear el primer frame recibido para confirmar que el sensor responde
Consideración de hardware: La I2C es compartida entre el codec y la cámara (BOARD_I2C_SDA_GPIO 7, BOARD_I2C_SCL_GPIO 8). Habrá que asegurarse de que no haya acceso concurrente desde distintas tareas; probablemente se necesite un mutex de bus.

Estimación: 2–3 días

Tarea 3 — Captura de frames y gestión de buffers
Qué involucra:

Implementar vision_capture_frame() que retorna un puntero al buffer DMA del frame más reciente
Implementar vision_release_frame() para devolver el buffer al driver (evitar memory leaks)
Definir el formato: escala de grises (PIXFORMAT_GRAYSCALE) es preferible a RGB565 porque reduce el buffer a la mitad (37.5 KB en QVGA) y simplifica el procesamiento posterior
Gestión de lifecycle: el frame debe quedar "bloqueado" durante el procesamiento para evitar que el DMA lo sobreescriba
Estimación: 1 día

Tarea 4 — Detección de posición: algoritmo heurístico por zonas
Qué involucra:

Dividir el frame en tres franjas verticales de igual ancho (izquierda, centro, derecha)
Para cada franja, calcular la actividad de bordes: diferencia entre píxeles adyacentes (aproximación simple de gradiente) o suma de varianza local
La franja con mayor actividad indica dónde está el obstáculo
Agregar un umbral de actividad mínima: si ninguna zona supera el umbral, retornar SIDE_NONE
Por qué heurística primero: Es determinista, fácil de depurar, y produce resultados inmediatos. Sirve como baseline para comparar con el modelo de IA en la siguiente tarea. Para un proyecto académico es importante demostrar que se entendió el problema antes de resolverlo con ML.

Estimación: 2–3 días

Tarea 5 — Integración con modelo de IA (ESP-DL)
Qué involucra:

Integrar ESP-DL v2.x (librería de inferencia de Espressif optimizada para las extensiones vectoriales RISC-V del ESP32-P4, que es la alternativa a TF-Lite Micro para este chip)
Evaluar si existe un modelo pre-entrenado de detección de personas/obstáculos en ESP-WHO (framework de visión de Espressif) que pueda reutilizarse
Si no: entrenar un modelo liviano de 3 clases (izquierda/centro/derecha) en PC con un dataset simple, cuantizarlo a int8, y exportarlo al formato que consume ESP-DL
Reemplazar o complementar la heurística con la inferencia del modelo, manteniendo la heurística como fallback
Consideración de latencia: La inferencia en ESP32-P4 a 400MHz sin NPU puede tomar entre 50ms y 300ms dependiendo del modelo. Si supera el PROXIMITY_POLL_MS (50ms actual), habrá que desacoplar la captura de la fusión en tareas separadas con un buffer de último resultado.

Estimación: 3–5 días

Tarea 6 — Fusión LiDAR + cámara en sentis.c
Qué involucra:

Modificar proximity_task (o reemplazarla con dos tareas: proximity_task y vision_task) para consultar vision_get_obstacle_side() solo cuando el LiDAR reporta un obstáculo en rango
Implementar la tabla de decisión háptica completa
Manejar el caso de SIDE_NONE de la cámara con una fallback a PULSE_BOTH (si la IA no detecta posición, asumir frontal)
Estimación: 1–2 días

Tarea 7 — Calibración y testing
Qué involucra:

Pruebas con distintas condiciones de iluminación (la cámara es el componente más sensible al entorno)
Ajuste de los umbrales de la heurística o del modelo según resultados reales
Medición de la latencia end-to-end: desde que el LiDAR detecta hasta que el motor vibra
Documentar limitaciones conocidas (luz baja, obstáculos muy cercanos que llenan el frame completo, etc.)
Estimación: 2–3 días

Resumen de estimaciones
Tarea	Descripción	Estimación
1	Entorno y dependencias (cámara)	1–2 días
2	Componente vision + init cámara	2–3 días
3	Captura de frames y buffers	1 día
4	Detección heurística por zonas	2–3 días
5	Integración con ESP-DL (modelo IA)	3–5 días
6	Fusión LiDAR + cámara en sentis.c	1–2 días
7	Calibración y testing	2–3 días
Total		12–19 días
Decisiones que conviene tomar antes de empezar
¿Se implementa primero la heurística sola (Tareas 1–4, 6–7) y luego se agrega el modelo (Tarea 5)? Recomendado: sí. Así el dispositivo funciona desde antes y la IA es una mejora incremental.

¿Se usa un modelo pre-entrenado de ESP-WHO o se entrena uno propio? Si hay tiempo, entrenar uno propio con 3 clases tiene más valor académico. Si el plazo es ajustado, reutilizar person-detection de ESP-WHO como proxy es viable.

Latencia aceptable: Si el modelo tarda más de 50ms, la tarea de visión debe correr independiente de proximity_task. ¿Se acepta esa complejidad arquitectural adicional?

¿Quieres que avancemos con esta secuencia o ajustar alguna tarea antes de escribir código?


-------------------------------------------

Respondiendo a las decisiones:
1. Si, podemos relegar la Tarea 5 como mejora incremental y validar la integración solamente con el análisis heurísitico
2. También trabajemoslo de forma incremental, teniendo en cuenta que no implementaremos la fase 5 esta decisión puede postergarse
3. Resolvamos los problemas de latencia en caso de presentarse, eventualmente manejemos fallbacks en que los sensores hapticos se activen de la forma en que funciona actualmente (en base a lo que captura el Lidar)

---------------------------------------------

Perfecto. Scope definido: Tareas 1–4, 6–7 con fallback al comportamiento actual del LiDAR si la cámara no resuelve la posición. Antes de escribir una línea de código, déjame ver los archivos de build y configuración para entender el entorno exacto.
Perfecto. Scope final: heurística de zonas con fallback al comportamiento LiDAR cuando la cámara no resuelve posición. Voy a implementar todo en secuencia.