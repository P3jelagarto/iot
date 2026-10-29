import json
import logging
import os
import secrets
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from pymodbus.client import ModbusTcpClient

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("plc_bridge")

MQTT_BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "mosquitto")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
MQTT_TOPIC_PREFIX = os.environ.get("MQTT_TOPIC_PREFIX", "ecs")

PLC_HOST = os.environ.get("PLC_HOST", "192.168.0.1")
PLC_PORT = int(os.environ.get("PLC_PORT", "502"))
PLC_UNIT_ID = int(os.environ.get("PLC_UNIT_ID", "1"))
PLC_POLL_SECONDS = float(os.environ.get("PLC_POLL_SECONDS", "0.5"))
PLC_PUBLISH_SECONDS = float(os.environ.get("PLC_PUBLISH_SECONDS", "2"))

PLC_SLUG = os.environ.get("PLC_DEVICE_SLUG", "plc-bomba")
PLC_TOKEN = os.environ.get("PLC_DEVICE_TOKEN", "")
ESP32_SLUG = os.environ.get("ESP32_DEVICE_SLUG", "esp32-estanque")
# Opcional: si se define, solo se reenvía al PLC el nivel firmado con este token.
ESP32_TOKEN = os.environ.get("ESP32_DEVICE_TOKEN", "")
# Variador inalámbrico (docs/VDF_INALAMBRICO.md): el PLC ya no tiene cables al
# D700; el puente reenvía su orden al ESP32 del variador y le devuelve al PLC
# la frecuencia real y la falla. Apagado, el puente funciona como antes.
VDF_WIRELESS = os.environ.get("VDF_INALAMBRICO", "false").lower() in ("1", "true", "yes")
VDF_SLUG = os.environ.get("VDF_DEVICE_SLUG", "esp32-vdf")
VDF_TOKEN = os.environ.get("VDF_DEVICE_TOKEN", "")
VDF_STALE_SECONDS = float(os.environ.get("VDF_STALE_SECONDS", "3"))

# --- Mapa de registros (holding registers, dirección 0 = 40001) ---
# Escritura Raspberry -> PLC
REG_CMD_MARCHA = 0
REG_CMD_MODO_AUTO = 1
REG_CMD_SETPOINT = 2      # % x10
REG_CMD_VELOCIDAD = 3     # Hz x10
REG_NIVEL = 4             # % x10 (desde el ESP32)
REG_WATCHDOG = 5
REG_VDF_FRECUENCIA = 6    # Hz x10, frecuencia real leída por el ESP32 del variador
REG_VDF_ESTADO = 7        # bit 0 enlace OK, bit 1 falla del D700, bit 2 en marcha
# Lectura PLC -> Raspberry (bloque contiguo desde REG_STATE_START)
REG_STATE_START = 10
STATE_VARIABLES = [
    # (variable MQTT, escala)
    ("bomba_marcha", 1),
    ("modo_auto", 1),
    ("setpoint_nivel", 10),
    ("velocidad_ref", 10),
    ("frecuencia", 10),
    ("falla", 1),
]

# variable MQTT del comando -> (registro, escala, mínimo, máximo)
COMMANDS = {
    "bomba_marcha": (REG_CMD_MARCHA, 1, 0, 1),
    "modo_auto": (REG_CMD_MODO_AUTO, 1, 0, 1),
    "setpoint_nivel": (REG_CMD_SETPOINT, 10, 0, 100),
    "velocidad_ref": (REG_CMD_VELOCIDAD, 10, 0, 50),
}

# Lectura PLC -> Raspberry: orden que el PLC le da al variador
REG_ORDEN_START = 16      # 16 marcha (0/1), 17 velocidad Hz x10


class Bridge:
    def __init__(self):
        self.plc = ModbusTcpClient(PLC_HOST, port=PLC_PORT, timeout=2)
        self.plc_lock = threading.Lock()
        self.mqtt = mqtt.Client()
        if MQTT_USERNAME:
            self.mqtt.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        self.mqtt.on_connect = self.on_connect
        self.mqtt.on_message = self.on_message
        self.nivel = None
        self.watchdog = 0
        self.force_publish = False
        self.vdf = {}               # última telemetría del ESP32 del variador
        self.vdf_seen_at = None     # time.monotonic() del último mensaje válido

    # --- MQTT ---
    def on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            log.error("Falla de conexión MQTT rc=%s", rc)
            return
        log.info("MQTT conectado a %s:%s", MQTT_BROKER_HOST, MQTT_BROKER_PORT)
        client.subscribe(f"{MQTT_TOPIC_PREFIX}/{PLC_SLUG}/+/set", qos=1)
        client.subscribe(f"{MQTT_TOPIC_PREFIX}/{ESP32_SLUG}/nivel", qos=1)
        if VDF_WIRELESS:
            client.subscribe(f"{MQTT_TOPIC_PREFIX}/{VDF_SLUG}/+", qos=0)

    def on_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return
        parts = msg.topic.split("/")
        value = data.get("value")
        if not isinstance(value, (int, float)):
            return

        if len(parts) == 3 and parts[1] == ESP32_SLUG:
            if ESP32_TOKEN and not secrets.compare_digest(str(data.get("token", "")), ESP32_TOKEN):
                log.warning("Nivel con token inválido, se ignora")
                return
            self.nivel = float(value)
            return

        if len(parts) == 3 and parts[1] == VDF_SLUG:
            if not secrets.compare_digest(str(data.get("token", "")), VDF_TOKEN):
                log.warning("Telemetría del variador con token inválido, se ignora")
                return
            self.vdf[parts[2]] = float(value)
            self.vdf_seen_at = time.monotonic()
            return

        if len(parts) == 4 and parts[3] == "set":
            if not secrets.compare_digest(str(data.get("token", "")), PLC_TOKEN):
                log.warning("Comando con token inválido en %s, se ignora", msg.topic)
                return
            command = COMMANDS.get(parts[2])
            if command is None:
                log.warning("Comando desconocido: %s", parts[2])
                return
            register, scale, low, high = command
            raw = int(round(max(low, min(high, float(value))) * scale))
            if self.write_register(register, raw):
                log.info("Comando %s = %s -> HR%d = %d", parts[2], value, register, raw)
                self.force_publish = True

    def publish(self, variable, value):
        payload = {
            "value": round(float(value), 2),
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "token": PLC_TOKEN,
        }
        self.mqtt.publish(f"{MQTT_TOPIC_PREFIX}/{PLC_SLUG}/{variable}", json.dumps(payload), qos=1)

    def forward_vdf(self):
        """Reenvía al ESP32 la orden del PLC y le devuelve al PLC el estado del variador."""
        orden = self.read_registers(REG_ORDEN_START, 2)
        if orden is not None:
            marcha, hz_x10 = orden
            payload = {"marcha": 1 if marcha else 0, "hz": max(0, min(500, hz_x10)) / 10, "token": VDF_TOKEN}
            self.mqtt.publish(f"{MQTT_TOPIC_PREFIX}/{VDF_SLUG}/orden/set", json.dumps(payload), qos=0)

        fresh = self.vdf_seen_at is not None and time.monotonic() - self.vdf_seen_at < VDF_STALE_SECONDS
        enlace = fresh and self.vdf.get("enlace_vdf", 0) == 1
        estado = (
            (1 if enlace else 0)
            | (2 if fresh and self.vdf.get("falla_vdf", 0) else 0)
            | (4 if fresh and self.vdf.get("en_marcha", 0) else 0)
        )
        frecuencia = int(round(self.vdf.get("frecuencia", 0) * 10)) if enlace else 0
        self.write_registers(REG_VDF_FRECUENCIA, [frecuencia, estado])

    # --- Modbus ---
    def write_register(self, register, value):
        return self.write_registers(register, [value])

    def write_registers(self, register, values):
        with self.plc_lock:
            try:
                if not self.plc.connected:
                    self.plc.connect()
                values = [v & 0xFFFF for v in values]
                if len(values) == 1:
                    result = self.plc.write_register(register, values[0], slave=PLC_UNIT_ID)
                else:
                    result = self.plc.write_registers(register, values, slave=PLC_UNIT_ID)
                if result.isError():
                    log.error("Error escribiendo HR%d: %s", register, result)
                    return False
                return True
            except Exception as exc:
                log.error("PLC no disponible al escribir HR%d: %s", register, exc)
                self.plc.close()
                return False

    def read_registers(self, register, count):
        with self.plc_lock:
            try:
                if not self.plc.connected:
                    self.plc.connect()
                result = self.plc.read_holding_registers(register, count=count, slave=PLC_UNIT_ID)
                if result.isError():
                    log.error("Error leyendo HR%d: %s", register, result)
                    return None
                return result.registers
            except Exception as exc:
                log.error("PLC no disponible en %s:%s: %s", PLC_HOST, PLC_PORT, exc)
                self.plc.close()
                return None

    def read_state(self):
        registers = self.read_registers(REG_STATE_START, len(STATE_VARIABLES))
        if registers is None:
            return None
        return {variable: raw / scale for (variable, scale), raw in zip(STATE_VARIABLES, registers)}

    def run(self):
        if not PLC_TOKEN:
            log.warning("PLC_DEVICE_TOKEN vacío: el listener rechazará las lecturas. "
                        "Corre `python manage.py setup_planta` para obtenerlo.")
        self.mqtt.reconnect_delay_set(min_delay=1, max_delay=30)
        self.mqtt.connect_async(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=60)
        self.mqtt.loop_start()
        log.info("Puente PLC %s:%s (unit %s) <-> MQTT", PLC_HOST, PLC_PORT, PLC_UNIT_ID)
        if VDF_WIRELESS:
            if not VDF_TOKEN:
                log.warning("VDF_INALAMBRICO activo pero VDF_DEVICE_TOKEN vacío: el ESP32 del "
                            "variador ignorará las órdenes. Corre `python manage.py setup_planta`.")
            log.info("Variador inalámbrico: órdenes a %s/%s/orden/set", MQTT_TOPIC_PREFIX, VDF_SLUG)

        last_publish = 0.0
        while True:
            self.watchdog = (self.watchdog + 1) % 32768
            self.write_register(REG_WATCHDOG, self.watchdog)
            if self.nivel is not None:
                self.write_register(REG_NIVEL, int(round(self.nivel * 10)))
            if VDF_WIRELESS:
                self.forward_vdf()

            now = time.monotonic()
            if self.force_publish or now - last_publish >= PLC_PUBLISH_SECONDS:
                state = self.read_state()
                if state is not None:
                    for variable, value in state.items():
                        self.publish(variable, value)
                    last_publish = now
                    self.force_publish = False
            time.sleep(PLC_POLL_SECONDS)


if __name__ == "__main__":
    Bridge().run()
