// Copia este archivo a "config.h" (mismo directorio) y completa tus valores.
// config.h queda ignorado por git (contiene el token del dispositivo) — ver .gitignore.
#pragma once

// --- Identidad del dispositivo ---
// `python manage.py setup_planta` registra "esp32-vdf" e imprime su token
// (la misma línea VDF_DEVICE_TOKEN que va en el .env de la Raspberry).
#define DEVICE_SLUG   "esp32-vdf"
#define DEVICE_TOKEN  "paste-token-here"

// --- WiFi (obligatorio) ---
// Este ESP32 maneja un motor: no levanta portal cautivo. Si pierde el WiFi,
// detiene el variador y se reconecta solo.
#define WIFI_SSID     "Crux"
#define WIFI_PASSWORD ""

// --- Broker MQTT (la Raspberry) ---
#define MQTT_BROKER_HOST "172.20.10.7"
#define MQTT_BROKER_PORT 1883
#define MQTT_USERNAME    "ecs"                     // MQTT_USERNAME de tu .env
#define MQTT_PASSWORD    "change-me-mqtt-password" // MQTT_PASSWORD de tu .env
#define MQTT_TOPIC_PREFIX "ecs"                    // MQTT_TOPIC_PREFIX de tu .env

// --- Módulo TTL <-> RS-485 hacia el conector PU del D700 ---
#define RS485_RX_PIN  16   // RO del módulo
#define RS485_TX_PIN  17   // DI del módulo
// DE+RE unidos. Pon -1 si el módulo cambia de dirección solo ("auto direction").
#define RS485_DE_PIN  4

// --- Modbus RTU: debe coincidir con los parámetros del variador ---
#define VDF_STATION   1       // Pr.117
#define VDF_BAUD      19200   // Pr.118 = 192
#define VDF_SERIAL    SERIAL_8E1  // Pr.119 = 0 (8 bits, 1 stop) + Pr.120 = 2 (paridad par)
#define VDF_MAX_HZ    50.0f   // tope de seguridad para la orden de velocidad

// --- Tiempos (ms) ---
#define VDF_POLL_MS       250   // cada cuánto se habla con el variador (Pr.122 = 1 s lo vigila)
#define ORDER_TIMEOUT_MS  2000  // sin orden nueva de la Raspberry en este tiempo -> parar
#define PUBLISH_MS        1000  // cada cuánto se publica frecuencia, corriente y falla
#define STATUS_LED_PIN    2     // LED integrado: fijo = todo OK, parpadea = sin enlace
