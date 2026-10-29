# Firmware ESP32

Hay dos sketches:

- `esp32_ecs_iot/`: el ESP32 del estanque (este README).
- `esp32_vdf/`: el ESP32 conectado al variador D700 por RS-485, que reemplaza
  los cables entre el PLC y el variador. Ver
  [docs/VDF_INALAMBRICO.md](../docs/VDF_INALAMBRICO.md).

Sketch para el ESP32 del estanque (`esp32-estanque`): mide el nivel con un
sensor ultrasónico HC-SR04 y lo publica
en % como la variable ¨nivel¨.

## Conexión del sensor

| Sensor | ESP32 |
|--------|-------|
| VCC    | 5V (VIN) |
| GND    | GND |
| TRIG   | GPIO 5 |
| ECHO   | GPIO 18 **a través de un divisor** (1 kΩ en serie, 2 kΩ a GND): ECHO sale a 5 V y el ESP32 trabaja a 3.3 V |

Monta el sensor arriba del estanque, apuntando hacia el agua, y mide con una
huincha la distancia al fondo (`DISTANCIA_VACIO_CM`) y al agua con el estanque
lleno (`DISTANCIA_LLENO_CM`).

Librerías (Sketch → Include Library → Manage Libraries):
**WiFiManager** (tzapu)
**PubSubClient** (Nick O'Leary)
**ArduinoJson** (Benoit Blanchon, v6.x)

## 1. Registra el dispositivo en la plataforma

Antes de tocar el firmware, el `Device` debe existir en el backend (las
lecturas de un dispositivo no registrado, o con token inválido, se
descartan y quedan en cuarentena como `UnregisteredReading`):

Lo más rápido es `docker compose exec web python manage.py setup_planta`:
registra `esp32-estanque` e imprime su token listo para `config.h`. También
puedes verlo en **Dispositivos → esp32-estanque** (página de
aprovisionamiento).

## 2. Configura el sketch

```
copy config.example.h esp32_ecs_iot\config.h
```

Edita `esp32_ecs_iot/config.h`:

- `DEVICE_SLUG` / `DEVICE_TOKEN` — los del paso 1.
- `MQTT_BROKER_HOST` / `MQTT_BROKER_PORT`:
  - **Local (docker-compose):** la IP de la PC que corre Docker en tu red
    local (el ESP32 es otro dispositivo en la LAN, así que `localhost` no
    sirve) y el puerto `MOSQUITTO_HOST_PORT` de tu `.env`.
  - **Railway (producción):** el host:puerto del **TCP Proxy** del servicio
    `mosquitto` (Settings → Networking → TCP Proxy en Railway).
- `MQTT_USERNAME` / `MQTT_PASSWORD` — igual que `MQTT_USERNAME`/`MQTT_PASSWORD`
  de tu `.env`.
- `TRIG_PIN`, `ECHO_PIN`, `DISTANCIA_VACIO_CM`, `DISTANCIA_LLENO_CM` — pines
  y calibración del sensor ultrasónico (ver "Conexión del sensor").

`config.h` está en `.gitignore` (contiene el token del dispositivo) — no se
sube al repo.

## 3. Flashea y prueba

1. Selecciona tu placa ESP32 en Arduino IDE y el puerto correspondiente.
2. Sube `esp32_ecs_iot.ino`.
3. Abre el **Monitor Serie** (115200 baud). Al primer arranque (o si no
   recuerda ninguna red), el ESP32 levanta un punto de acceso WiFi llamado
   como `DEVICE_AP_TAG` — conéctate a él desde tu celular/PC y sigue el
   portal cautivo para darle el SSID/contraseña de tu WiFi real (las
   credenciales de red **no** se programan en el código ni las pide la
   plataforma, se ingresan directo en el hardware, como indica el README raíz).
4. Tras conectar, el sketch se conecta al broker MQTT y empieza a publicar
   en `ecs/<slug>/<variable>` cada `PUBLISH_INTERVAL_MS`. Deberías ver la
   gráfica llenarse en `/monitoring/` en unos segundos.
5. Si agregaste un gadget **Botón** o **Slider** a la variable `relay`
   (`COMMAND_VARIABLE`) desde `/monitoring/sensors/<id>/widget/`, al
   accionarlo en el dashboard el ESP32 debería recibir el comando y prender/
   apagar el pin `COMMAND_PIN` (LED integrado por defecto), y el dashboard
   debe reflejar el estado confirmado.

## Diagnóstico rápido

- **No aparecen lecturas:** revisa el Monitor Serie del ESP32 (¿conectó
  WiFi? ¿conectó MQTT, o reintenta con `rc=...`?) y el Admin de Django
  (`/admin/monitoring/unregisteredreading/`) — si el mensaje llegó pero con
  slug o token incorrecto, aparecerá ahí con el motivo.
- **`rc=5` o falla de conexión MQTT:** usuario/contraseña del broker
  incorrectos (`MQTT_USERNAME`/`MQTT_PASSWORD` en `config.h` vs `.env`).
- **El comando no llega:** confirma que el topic suscrito
  (`ecs/<slug>/relay/set`, visible en el log de arranque) coincide
  exactamente con el que usa la plataforma para ese sensor.
- Puedes espiar el tráfico MQTT sin hardware con `mosquitto_sub` desde otra
  terminal (con el cliente `mosquitto` instalado o el contenedor
  `eclipse-mosquitto:2`):
  ```
  mosquitto_sub -h <broker-host> -p <puerto> -u ecs -P <MQTT_PASSWORD> -t "ecs/#" -v
  ```
