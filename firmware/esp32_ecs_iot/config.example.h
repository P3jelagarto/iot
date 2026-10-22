// Copia este archivo a "config.h" (mismo directorio) y completa tus valores.
// config.h queda ignorado por git (contiene el token del dispositivo) — ver .gitignore.
#pragma once

// --- Identidad del dispositivo (la genera la plataforma al registrar el Device) ---
// Dashboard -> Dispositivos -> "+ Agregar sensor" -> te muestra el ID (slug) y el token.
// `python manage.py setup_planta` registra "esp32-estanque" e imprime su token.
#define DEVICE_SLUG   "esp32-estanque"        // debe coincidir EXACTO con el slug registrado
#define DEVICE_TOKEN  "paste-token-here"      // token único del dispositivo (regenerable desde el dashboard)
#define DEVICE_AP_TAG "esp32-estanque-ap"     // nombre del punto de acceso WiFiManager al aprovisionar

// --- WiFi (opcional): si se define, el sketch intenta conectar directo antes de
// levantar el portal cautivo de WiFiManager. Dejar vacío ("") para forzar siempre el portal
// (recomendado para hardware que se distribuye a clientes finales, ver README raíz).
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// --- Broker MQTT ---
// Desarrollo local (docker-compose): usa la IP de tu PC en la red local (no "localhost",
// el ESP32 es otro dispositivo en la red) y el puerto MOSQUITTO_HOST_PORT de tu .env.
// Producción (Railway): usa el host:puerto del TCP Proxy (Mosquitto -> Settings -> Networking -> TCP Proxy).
// Con la Raspberry Pi como servidor: la IP de la Raspberry en la red del laboratorio.
#define MQTT_BROKER_HOST "192.168.1.100"
#define MQTT_BROKER_PORT 1883
#define MQTT_USERNAME    "ecs"          // MQTT_USERNAME de tu .env
#define MQTT_PASSWORD    "change-me-mqtt-password" // MQTT_PASSWORD de tu .env
#define MQTT_TOPIC_PREFIX "ecs"         // MQTT_TOPIC_PREFIX de tu .env

// --- Sensor ultrasónico de nivel (HC-SR04 / JSN-SR04T) ---
// OJO: el pin ECHO de estos sensores entrega 5 V. Usa un divisor de voltaje
// (ej. 1 kΩ en serie + 2 kΩ a GND) antes de llegar al ESP32, que trabaja a 3.3 V.
#define SENSOR_VARIABLE      "nivel" // variable tal como aparecerá en el dashboard
#define TRIG_PIN             5
#define ECHO_PIN             18
#define SAMPLES_PER_READING  5       // se publica la mediana de estas lecturas
// Calibración: mide con una huincha la distancia del sensor al agua.
#define DISTANCIA_VACIO_CM   60.0f   // sensor -> fondo del estanque (estanque vacío = 0 %)
#define DISTANCIA_LLENO_CM   10.0f   // sensor -> agua con el estanque lleno (= 100 %)
#define NIVEL_ALARMA_ALTO    95.0f   // % sobre el cual se enciende el LED de alarma
#define ALARM_LED_PIN        2       // LED integrado en muchas placas ESP32 DevKit
#define PUBLISH_INTERVAL_MS  2000    // cada cuánto se publica una lectura

// --- Comando (plataforma -> dispositivo), opcional: gadget Botón/Slider ---
// Déjalo definido aunque no uses un relé real; si no se usa el pin, simplemente no tendrá efecto físico.
#define COMMAND_VARIABLE "relay"
#define COMMAND_PIN      4   // salida libre para un relé o LED extra

// --- NTP (para incluir "timestamp" en cada lectura; opcional, el servidor puede sellar la hora de recepción) ---
#define NTP_SERVER   "pool.ntp.org"
#define GMT_OFFSET_SEC 0
#define DST_OFFSET_SEC 0
