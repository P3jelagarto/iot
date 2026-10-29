// Planta IoT - firmware del ESP32 del variador (dispositivo "esp32-vdf")
//
// Reemplaza el cableado PLC <-> D700 (STF y borne 2) por WiFi:
//
//   PLC --Modbus TCP-- Raspberry (plc_bridge) --MQTT/WiFi-- este ESP32
//       --UART TTL-- módulo RS-485 --conector PU-- Mitsubishi D700
//
// El PLC sigue decidiendo marcha y velocidad. plc_bridge reenvía esa orden a
//   <prefix>/esp32-vdf/orden/set   {"marcha": 0|1, "hz": 0..50, "token": ...}
// cada 0,5 s, y este ESP32 se la escribe al variador por Modbus RTU.
// Publica (contrato MQTT del backend, {value, token}):
//   frecuencia (Hz real), corriente (A), en_marcha, falla_vdf, enlace_vdf
//
// Seguridad: si no llega una orden válida en ORDER_TIMEOUT_MS (WiFi, MQTT o
// Raspberry caídos), manda parar. Si este ESP32 se cuelga o se desconecta, el
// variador para solo por Pr.122 (verificación de comunicación).
//
// Librerias requeridas (Arduino IDE -> Manage Libraries):
//   - PubSubClient   by Nick O'Leary
//   - ArduinoJson    by Benoit Blanchon (v6.x)
//   - ModbusMaster   by Doc Walker
//
// Antes de compilar: copia config.example.h a config.h y completa tus datos.

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ModbusMaster.h>

#include "config.h"

// Registros Modbus del D700 (dirección = número de registro - 40001)
const uint16_t REG_CONTROL    = 8;    // 40009: escritura = comando de marcha, lectura = estado
const uint16_t REG_FREQ_RAM   = 13;   // 40014: frecuencia de consigna en RAM, 0,01 Hz
const uint16_t REG_MONITOR    = 200;  // 40201: frecuencia de salida, 40202: corriente (0,01)
const uint16_t CMD_STOP       = 0x0000;
const uint16_t CMD_FORWARD    = 0x0002;  // bit 1 = STF
const uint16_t STATUS_RUN     = 0x0001;  // bit 0 = en marcha
const uint16_t STATUS_FAULT   = 0x0080;  // bit 7 = alarma (contacto ABC)

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
ModbusMaster vdf;

char orderTopic[128];

// Última orden recibida de la Raspberry
bool orderMarcha = false;
float orderHz = 0.0f;
unsigned long lastOrderAt = 0;
bool hasOrder = false;

// Último estado leído del variador
bool linkOk = false;
float outHz = 0.0f;
float outAmps = 0.0f;
bool running = false;
bool fault = false;

unsigned long lastPollAt = 0;
unsigned long lastPublishAt = 0;
unsigned long lastMqttAttemptAt = 0;

void preTransmission() {
  if (RS485_DE_PIN >= 0) digitalWrite(RS485_DE_PIN, HIGH);
}

void postTransmission() {
  if (RS485_DE_PIN >= 0) digitalWrite(RS485_DE_PIN, LOW);
}

void publishTelemetry(const char* variable, float value) {
  StaticJsonDocument<160> doc;
  doc["value"] = value;
  doc["token"] = DEVICE_TOKEN;
  char payload[160];
  size_t len = serializeJson(doc, payload);
  char topic[128];
  snprintf(topic, sizeof(topic), "%s/%s/%s", MQTT_TOPIC_PREFIX, DEVICE_SLUG, variable);
  mqttClient.publish(topic, (uint8_t*)payload, len, false);
}

void handleOrder(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("Orden con JSON invalido, se ignora.");
    return;
  }
  const char* token = doc["token"] | "";
  if (strcmp(token, DEVICE_TOKEN) != 0) {
    Serial.println("Orden con token invalido, se ignora.");
    return;
  }
  orderMarcha = (doc["marcha"] | 0) != 0;
  orderHz = constrain((float)(doc["hz"] | 0.0f), 0.0f, VDF_MAX_HZ);
  lastOrderAt = millis();
  hasOrder = true;
}

// No bloquea: el variador se sigue atendiendo aunque no haya WiFi ni MQTT.
void keepMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  if (millis() - lastMqttAttemptAt < 5000) return;
  lastMqttAttemptAt = millis();
  String clientId = String(DEVICE_SLUG) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    mqttClient.subscribe(orderTopic, 0);
    Serial.print("MQTT conectado, escuchando ");
    Serial.println(orderTopic);
  } else {
    Serial.print("Fallo MQTT, rc=");
    Serial.println(mqttClient.state());
  }
}

void pollVdf() {
  bool orderFresh = hasOrder && millis() - lastOrderAt < ORDER_TIMEOUT_MS;
  bool marcha = orderFresh && orderMarcha;
  float hz = marcha ? orderHz : 0.0f;
  if (!orderFresh && running) {
    Serial.println("Sin orden de la Raspberry: parando el variador.");
  }

  // Primero la velocidad y después la marcha, para que arranque ya con la consigna.
  uint8_t r1 = vdf.writeSingleRegister(REG_FREQ_RAM, (uint16_t)lroundf(hz * 100.0f));
  if (r1 != vdf.ku8MBSuccess) {
    // Cada intento sin respuesta tarda ~2 s (timeout de ModbusMaster): no insistir en este ciclo.
    if (linkOk) Serial.println("Variador NO responde: revisa cable A/B, Pr.117-120 y Pr.549.");
    linkOk = false;
    return;
  }
  uint8_t r2 = vdf.writeSingleRegister(REG_CONTROL, marcha ? CMD_FORWARD : CMD_STOP);
  uint8_t r3 = vdf.readHoldingRegisters(REG_MONITOR, 2);
  if (r3 == vdf.ku8MBSuccess) {
    outHz = vdf.getResponseBuffer(0) / 100.0f;
    outAmps = vdf.getResponseBuffer(1) / 100.0f;
  }
  uint8_t r4 = vdf.readHoldingRegisters(REG_CONTROL, 1);
  if (r4 == vdf.ku8MBSuccess) {
    uint16_t status = vdf.getResponseBuffer(0);
    running = status & STATUS_RUN;
    fault = status & STATUS_FAULT;
  }

  bool ok = r2 == vdf.ku8MBSuccess && r3 == vdf.ku8MBSuccess && r4 == vdf.ku8MBSuccess;
  if (ok != linkOk) {
    Serial.println(ok ? "Variador responde por RS-485." : "Variador NO responde: revisa cable A/B, Pr.117-120 y Pr.549.");
  }
  linkOk = ok;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(STATUS_LED_PIN, OUTPUT);
  if (RS485_DE_PIN >= 0) {
    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);
  }
  Serial2.begin(VDF_BAUD, VDF_SERIAL, RS485_RX_PIN, RS485_TX_PIN);
  vdf.begin(VDF_STATION, Serial2);
  vdf.preTransmission(preTransmission);
  vdf.postTransmission(postTransmission);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  snprintf(orderTopic, sizeof(orderTopic), "%s/%s/orden/set", MQTT_TOPIC_PREFIX, DEVICE_SLUG);
  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setCallback(handleOrder);
}

void loop() {
  keepMqtt();
  mqttClient.loop();

  unsigned long nowMs = millis();
  if (nowMs - lastPollAt >= VDF_POLL_MS) {
    lastPollAt = nowMs;
    pollVdf();
  }

  if (nowMs - lastPublishAt >= PUBLISH_MS && mqttClient.connected()) {
    lastPublishAt = nowMs;
    publishTelemetry("enlace_vdf", linkOk ? 1 : 0);
    publishTelemetry("frecuencia", outHz);
    publishTelemetry("corriente", outAmps);
    publishTelemetry("en_marcha", running ? 1 : 0);
    publishTelemetry("falla_vdf", fault ? 1 : 0);
  }

  bool allOk = linkOk && mqttClient.connected() && hasOrder && nowMs - lastOrderAt < ORDER_TIMEOUT_MS;
  digitalWrite(STATUS_LED_PIN, allOk || (nowMs / 250) % 2 ? HIGH : LOW);
}
