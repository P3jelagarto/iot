"""Simulador de la planta de llenado de estanque (sin hardware).

Simula los dos dispositivos que registra `python manage.py setup_planta`:

- esp32-estanque: publica `nivel` (%) del estanque.
- plc-bomba: el PLC S7-1200 + variador D700. Publica `bomba_marcha`,
  `modo_auto`, `setpoint_nivel`, `velocidad_ref`, `frecuencia` y `falla`, y
  responde a los comandos del dashboard en `<variable>/set` igual que lo
  haría el puente Modbus real (plc_bridge/).

La lógica de control es la misma que se documenta para el programa del PLC
(docs/PLC_VDF.md), así la demo se comporta como la planta real.

Con hardware real, apaga la parte que ya existe:
    SIMULATE_ESP32=false  -> el PLC simulado usa el nivel que publica el ESP32 real.
    SIMULATE_PLC=false    -> solo se simula el nivel, que sube según la frecuencia
                             que publica el PLC real (o fake_plc.py).
"""
import json
import os
import random
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

MQTT_BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "mosquitto")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
MQTT_TOPIC_PREFIX = os.environ.get("MQTT_TOPIC_PREFIX", "ecs")
PUBLISH_INTERVAL_SECONDS = float(os.environ.get("SIMULATOR_INTERVAL_SECONDS", "2"))
SIMULATE_ESP32 = os.environ.get("SIMULATE_ESP32", "true").lower() == "true"
SIMULATE_PLC = os.environ.get("SIMULATE_PLC", "true").lower() == "true"

ESP32_SLUG = "esp32-estanque"
PLC_SLUG = "plc-bomba"

TICK_SECONDS = 0.5
# Modelo del estanque (en % de nivel por segundo)
FILL_RATE_AT_50HZ = 2.0      # caudal de la bomba a frecuencia máxima
DRAIN_RATE = 0.6             # consumo/desagüe constante
VDF_RAMP_HZ_PER_S = 10.0     # rampa de aceleración del D700 (Pr.7/Pr.8)
# Lógica de control (igual a la del PLC)
AUTO_HYSTERESIS = 3.0        # arranca cuando el nivel baja de setpoint - histéresis
AUTO_KP = 4.0                # Hz por cada % de error en modo automático
AUTO_MIN_HZ = 20.0
MAX_HZ = 50.0


def parse_device_tokens(raw):
    """Parses SIMULATOR_DEVICE_TOKENS="slug1:token1,slug2:token2" into a dict."""
    tokens = {}
    for pair in raw.split(","):
        pair = pair.strip()
        if not pair or ":" not in pair:
            continue
        slug, token = pair.split(":", 1)
        tokens[slug.strip()] = token.strip()
    return tokens


DEVICE_TOKENS = parse_device_tokens(os.environ.get("SIMULATOR_DEVICE_TOKENS", ""))


class Plant:
    def __init__(self):
        self.nivel = 40.0
        self.bomba_marcha = 0
        self.modo_auto = 1
        self.setpoint_nivel = 70.0
        self.velocidad_ref = 35.0
        self.frecuencia = 0.0
        self.falla = 0

    def apply_command(self, variable, value):
        """Aplica un comando del dashboard. Devuelve True si la variable existe."""
        if variable == "bomba_marcha":
            # En automático el PLC decide la marcha; el comando manual se ignora.
            if not self.modo_auto:
                self.bomba_marcha = 1 if value else 0
        elif variable == "modo_auto":
            self.modo_auto = 1 if value else 0
        elif variable == "setpoint_nivel":
            self.setpoint_nivel = max(0.0, min(100.0, float(value)))
        elif variable == "velocidad_ref":
            self.velocidad_ref = max(0.0, min(MAX_HZ, float(value)))
        else:
            return False
        return True

    def control(self):
        """Lógica del PLC: decide marcha y frecuencia objetivo."""
        if self.modo_auto:
            if self.nivel >= self.setpoint_nivel:
                self.bomba_marcha = 0
            elif self.nivel < self.setpoint_nivel - AUTO_HYSTERESIS:
                self.bomba_marcha = 1
            error = self.setpoint_nivel - self.nivel
            target = max(AUTO_MIN_HZ, min(MAX_HZ, AUTO_KP * error))
        else:
            target = self.velocidad_ref
        return target if self.bomba_marcha and not self.falla else 0.0

    def step(self, dt, simulate_level, simulate_plc):
        if simulate_plc:
            target = self.control()
            max_delta = VDF_RAMP_HZ_PER_S * dt
            self.frecuencia += max(-max_delta, min(max_delta, target - self.frecuencia))
        if simulate_level:
            inflow = FILL_RATE_AT_50HZ * self.frecuencia / MAX_HZ
            self.nivel += (inflow - DRAIN_RATE) * dt + random.uniform(-0.05, 0.05)
            self.nivel = max(0.0, min(100.0, self.nivel))


def main():
    plant = Plant()
    client = mqtt.Client()
    if MQTT_USERNAME:
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    def publish(slug, variable, value):
        payload = {
            "value": round(float(value), 2),
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "token": DEVICE_TOKENS.get(slug, ""),
        }
        client.publish(f"{MQTT_TOPIC_PREFIX}/{slug}/{variable}", json.dumps(payload), qos=1)

    def publish_plc_state():
        for variable in ("bomba_marcha", "modo_auto", "setpoint_nivel", "velocidad_ref", "frecuencia", "falla"):
            publish(PLC_SLUG, variable, getattr(plant, variable))

    def on_connect(client, userdata, flags, rc):
        if SIMULATE_PLC:
            client.subscribe(f"{MQTT_TOPIC_PREFIX}/{PLC_SLUG}/+/set", qos=1)
            if not SIMULATE_ESP32:
                client.subscribe(f"{MQTT_TOPIC_PREFIX}/{ESP32_SLUG}/nivel", qos=1)
        elif SIMULATE_ESP32:
            # PLC real: el estanque simulado se llena según la frecuencia real del D700
            client.subscribe(f"{MQTT_TOPIC_PREFIX}/{PLC_SLUG}/frecuencia", qos=1)

    def on_message(client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return
        parts = msg.topic.split("/")
        if len(parts) == 3:
            if isinstance(data.get("value"), (int, float)):
                if parts[1] == ESP32_SLUG:
                    plant.nivel = float(data["value"])        # ESP32 real (SIMULATE_ESP32=false)
                elif parts[1] == PLC_SLUG:
                    plant.frecuencia = float(data["value"])   # PLC real (SIMULATE_PLC=false)
            return
        if len(parts) != 4 or data.get("token") != DEVICE_TOKENS.get(PLC_SLUG):
            print(f"Comando ignorado (topic o token inválido): {msg.topic}")
            return
        if plant.apply_command(parts[2], data.get("value", 0)):
            print(f"Comando {parts[2]} = {data.get('value')}")
            publish_plc_state()

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=60)
    client.loop_start()

    print(f"Simulador de planta publicando en {MQTT_BROKER_HOST}:{MQTT_BROKER_PORT} "
          f"(ESP32: {SIMULATE_ESP32}, PLC: {SIMULATE_PLC})")
    for slug, enabled in ((ESP32_SLUG, SIMULATE_ESP32), (PLC_SLUG, SIMULATE_PLC)):
        if enabled and slug not in DEVICE_TOKENS:
            print(f"WARNING: no token configured for '{slug}' in SIMULATOR_DEVICE_TOKENS; "
                  f"its readings will be rejected by the listener until you add one. "
                  f"Run `python manage.py setup_planta` to get it.")

    last_publish = 0.0
    try:
        while True:
            plant.step(TICK_SECONDS, simulate_level=SIMULATE_ESP32, simulate_plc=SIMULATE_PLC)
            now = time.monotonic()
            if now - last_publish >= PUBLISH_INTERVAL_SECONDS:
                last_publish = now
                if SIMULATE_ESP32:
                    publish(ESP32_SLUG, "nivel", plant.nivel)
                if SIMULATE_PLC:
                    publish_plc_state()
            time.sleep(TICK_SECONDS)
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
