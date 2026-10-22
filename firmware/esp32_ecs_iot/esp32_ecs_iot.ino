// Planta IoT - firmware del ESP32 del estanque (dispositivo "esp32-estanque")
//
// Mide el nivel del estanque con un sensor ultrasonico (HC-SR04 o JSN-SR04T,
// montado arriba del estanque mirando hacia el agua) y lo publica en % como
// la variable "nivel". La Raspberry Pi reenvia ese nivel al PLC S7-1200, que
// controla la bomba con el variador D700.
//
// Implementa el "Contrato MQTT" del backend (ver README.md del proyecto):
//   - Publica telemetria en   <prefix>/<slug>/<variable>          {value, timestamp, token}
//   - Escucha comandos en     <prefix>/<slug>/<variable>/set      {value, token}
//   - Al aplicar un comando, vuelve a publicar el nuevo estado en el topic de telemetria.
//   - El LED ALARM_LED_PIN se enciende si el nivel supera NIVEL_ALARMA_ALTO (alarma local).
//
// Librerias requeridas (Arduino IDE -> Sketch -> Include Library -> Manage Libraries):
//   - WiFiManager    by tzapu
//   - PubSubClient   by Nick O'Leary
//   - ArduinoJson    by Benoit Blanchon (v6.x)
//
// Antes de compilar: copia config.example.h a config.h y completa tus datos
// (slug, token, broker, credenciales MQTT). config.h NO se sube a git.

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

char telemetryTopic[128];
char commandTopic[128];

unsigned long lastPublishAt = 0;

void setupWiFi() {
  if (strlen(WIFI_SSID) > 0) {
    Serial.print("Conectando directo a WiFi ");
    Serial.println(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("\nWiFi conectado, IP: ");
      Serial.println(WiFi.localIP());
      return;
    }
    Serial.println("\nNo se pudo conectar con WIFI_SSID/WIFI_PASSWORD, cae al portal cautivo.");
  }

  WiFiManager wifiManager;
  // Levanta un portal cautivo (AP) si no hay credenciales guardadas o falla la conexion.
  // El usuario final configura su WiFi conectandose a este AP desde el celular/PC.
  if (!wifiManager.autoConnect(DEVICE_AP_TAG)) {
    Serial.println("No se pudo conectar a WiFi, reiniciando...");
    delay(3000);
    ESP.restart();
  }
  Serial.print("WiFi conectado, IP: ");
  Serial.println(WiFi.localIP());
}

String isoTimestampNow() {
  time_t now;
  time(&now);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return ""; // sin hora sincronizada aun: se omite, el servidor usara la hora de recepcion
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

void publishTelemetry(const char* variable, float value) {
  StaticJsonDocument<192> doc;
  doc["value"] = value;
  String ts = isoTimestampNow();
  if (ts.length() > 0) {
    doc["timestamp"] = ts;
  }
  doc["token"] = DEVICE_TOKEN;

  char payload[192];
  size_t len = serializeJson(doc, payload);

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/%s/%s", MQTT_TOPIC_PREFIX, DEVICE_SLUG, variable);
  mqttClient.publish(topic, (uint8_t*)payload, len, false);
  // Nota: PubSubClient solo soporta QoS 0 al publicar. El contrato pide QoS 1;
  // si tu instalacion lo requiere estricto, cambia a una libreria con soporte QoS 1
  // (ej. arduino-mqtt de 256dpi) o valida en el listener que toleres QoS 0 del hardware.
  Serial.print("-> ");
  Serial.print(topic);
  Serial.print(" ");
  Serial.println(payload);
}

// Mide la distancia al agua en cm (o -1 si no hubo eco).
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long echoUs = pulseIn(ECHO_PIN, HIGH, 30000);  // 30 ms ~ 5 m maximo
  if (echoUs == 0) {
    return -1.0f;
  }
  return echoUs * 0.0343f / 2.0f;  // velocidad del sonido: 0.0343 cm/us, ida y vuelta
}

// Mediana de varias lecturas para filtrar ecos falsos (olas, salpicaduras).
float readDistanceMedianCm() {
  float samples[SAMPLES_PER_READING];
  int count = 0;
  for (int i = 0; i < SAMPLES_PER_READING; i++) {
    float d = readDistanceCm();
    if (d > 0) {
      samples[count++] = d;
    }
    delay(60);  // el fabricante recomienda >= 60 ms entre disparos
  }
  if (count == 0) {
    return -1.0f;
  }
  for (int i = 1; i < count; i++) {  // insertion sort, son pocas muestras
    float key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }
  return samples[count / 2];
}

void readAndPublishSensor() {
  float distance = readDistanceMedianCm();
  if (distance < 0) {
    Serial.println("Sin eco del sensor ultrasonico, revisa el cableado (TRIG/ECHO/5V/GND).");
    return;
  }
  // Estanque vacio: el agua esta a DISTANCIA_VACIO_CM del sensor; lleno: a DISTANCIA_LLENO_CM.
  float nivel = (DISTANCIA_VACIO_CM - distance) * 100.0f / (DISTANCIA_VACIO_CM - DISTANCIA_LLENO_CM);
  nivel = constrain(nivel, 0.0f, 100.0f);

  Serial.print("Distancia ");
  Serial.print(distance, 1);
  Serial.print(" cm -> nivel ");
  Serial.print(nivel, 1);
  Serial.println(" %");

  digitalWrite(ALARM_LED_PIN, nivel >= NIVEL_ALARMA_ALTO ? HIGH : LOW);
  publishTelemetry(SENSOR_VARIABLE, nivel);
}

void handleCommand(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("Comando con JSON invalido, se ignora.");
    return;
  }

  const char* token = doc["token"] | "";
  if (strcmp(token, DEVICE_TOKEN) != 0) {
    Serial.println("Comando con token invalido, se ignora.");
    return;
  }

  float value = doc["value"] | 0.0f;
  Serial.print("Comando recibido en ");
  Serial.print(topic);
  Serial.print(" -> value=");
  Serial.println(value);

  // Aplica el comando (ejemplo: relay/LED). Ajusta segun tu actuador real.
  digitalWrite(COMMAND_PIN, value != 0 ? HIGH : LOW);

  // Confirma el nuevo estado real publicando en el topic de telemetria (sin /set).
  publishTelemetry(COMMAND_VARIABLE, value != 0 ? 1.0f : 0.0f);
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando a MQTT ");
    Serial.print(MQTT_BROKER_HOST);
    Serial.print(":");
    Serial.println(MQTT_BROKER_PORT);

    String clientId = String(DEVICE_SLUG) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.println("MQTT conectado.");
      mqttClient.subscribe(commandTopic, 1);
      Serial.print("Suscrito a ");
      Serial.println(commandTopic);
    } else {
      Serial.print("Fallo MQTT, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" reintentando en 5s...");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(COMMAND_PIN, OUTPUT);
  digitalWrite(COMMAND_PIN, LOW);
  pinMode(ALARM_LED_PIN, OUTPUT);
  digitalWrite(ALARM_LED_PIN, LOW);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  setupWiFi();

  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

  snprintf(telemetryTopic, sizeof(telemetryTopic), "%s/%s/%s", MQTT_TOPIC_PREFIX, DEVICE_SLUG, SENSOR_VARIABLE);
  snprintf(commandTopic, sizeof(commandTopic), "%s/%s/%s/set", MQTT_TOPIC_PREFIX, DEVICE_SLUG, COMMAND_VARIABLE);

  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setCallback(handleCommand);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  }
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  unsigned long nowMs = millis();
  if (nowMs - lastPublishAt >= PUBLISH_INTERVAL_MS) {
    lastPublishAt = nowMs;
    readAndPublishSensor();
  }
}
