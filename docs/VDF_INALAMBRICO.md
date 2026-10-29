# Variador inalámbrico: ESP32 + RS-485 en vez de cables al PLC

Con esto se sacan los 5 cables entre el PLC y el D700 (STF, SD, PC, 2 y 5).
El PLC sigue decidiendo cuándo y a qué velocidad anda la bomba, pero la orden
llega al variador por WiFi:

```
PLC ──Modbus TCP (cable)── Raspberry ──MQTT (WiFi)── ESP32 "esp32-vdf"
                                                          │ UART TTL 3,3 V
                                                  módulo TTL ↔ RS-485
                                                          │ A/B
                                               conector PU del D700 ── bomba
```

Lo marcado con ⚠ no está probado en el tablero y hay que confirmarlo con el
manual del FR-D700 antes de conectar.

---

## 1. Qué comprar

| Qué | Cuál | Nota |
|---|---|---|
| Módulo TTL ↔ RS-485 | **De 3,3 V**: "MAX3485" o "TTL to RS485 auto direction" (los de chip SP3485 o MAX13487) | Evita el MAX485 azul común: es de 5 V y su salida RO le mete 5 V al ESP32 |
| Cable de red | Un patch cord RJ45 cualquiera, que vas a cortar en un extremo | O un conector RJ45 hembra con bornes |
| Resistencia | 120 Ω (opcional) | Muchos módulos ya la traen soldada |
| Fuente | Cargador USB de 5 V para el ESP32 | No saques 5 V del conector PU |

Si el módulo trae pines **DE** y **RE**, es "manual": únelos y van a GPIO4.
Si solo trae **TXD/RXD** (o dice "auto direction"), deja `RS485_DE_PIN -1`
en `config.h`.

## 2. Conexión

Todo con el variador **desenergizado** (espera a que se apague el display).

| ESP32 | Módulo RS-485 | Conector PU del D700 ⚠ |
|---|---|---|
| GPIO17 (TX2) | DI (o TXD) | |
| GPIO16 (RX2) | RO (o RXD) | |
| GPIO4 | DE + RE unidos (solo módulo manual) | |
| 3V3 | VCC | |
| GND | GND | pin 1 (SG) |
| | A (+) | pines **3 y 5** unidos (RDA + SDA) |
| | B (−) | pines **4 y 6** unidos (RDB + SDB) |
| | | pines 2 y 8 (5 V): **no conectar** |

Los pines del RJ45 se cuentan con la traba hacia abajo y los contactos
mirándote, de izquierda a derecha (1 a 8). En un cable T568B: 1
blanco-naranja, 3 blanco-verde, 4 azul, 5 blanco-azul, 6 verde.

Si después el variador no responde, lo primero es **invertir A y B**: los
fabricantes no se ponen de acuerdo en cuál es cuál.

Después **saca los 5 cables** de la bornera intermedia (STF, SD, PC, 2 y 5).
⚠ La parada de emergencia del tablero tiene que seguir cortando la potencia
del variador; eso no pasa por WiFi.

## 3. Parámetros del variador

Con el variador energizado y detenido, en modo PU (Pr.79 = 1), y
**Pr.160 = 0** para ver todos los parámetros:

| Parámetro | Valor | Para qué |
|---|---|---|
| Pr.549 | 1 | Protocolo Modbus RTU |
| Pr.117 | 1 | Dirección (estación) |
| Pr.118 | 192 | 19200 bps |
| Pr.119 | 0 | 8 bits, 1 bit de parada |
| Pr.120 | 2 | Paridad par |
| Pr.122 | 1 | Si pasa 1 s sin mensajes, para con alarma (E.PUE) |
| Pr.338 | 0 | Marcha desde la comunicación |
| Pr.339 | 0 | Velocidad desde la comunicación |
| Pr.340 | 1 | Al encender, entra directo en modo red (NET) |
| Pr.79 | 0 | Permite el modo red |

Después de cambiarlos, **apaga y enciende el variador**: los parámetros de
comunicación se leen al arrancar. El display tiene que mostrar **NET**.

⚠ Con Pr.122 = 1, el variador da alarma si el ESP32 no está hablándole. Si
quieres probar el variador solo, pon Pr.122 = 9999 mientras tanto.

## 4. El ESP32

1. Librerías (Arduino IDE → Manage Libraries): **PubSubClient**,
   **ArduinoJson** (v6) y **ModbusMaster** (Doc Walker).
2. Copia `firmware/esp32_vdf/config.example.h` a `config.h` y completa:
   - `DEVICE_TOKEN`: lo imprime `setup_planta` (paso 5).
   - `WIFI_SSID` / `WIFI_PASSWORD`: la misma red que la Raspberry ("Crux").
   - `MQTT_BROKER_HOST`: la IP de la Raspberry en esa red (`hostname -I`).
   - `MQTT_USERNAME` / `MQTT_PASSWORD`: los del `.env` de la Raspberry.
   - `RS485_DE_PIN`: 4 o -1 según el módulo.
3. Flashea y abre el monitor serie a 115200. Con el variador encendido debe
   decir **"Variador responde por RS-485."**

El LED azul queda **fijo** cuando todo está bien (variador, WiFi y órdenes
de la Raspberry), y **parpadea** cuando falta algo.

Seguridad del firmware: si no llega una orden de la Raspberry en 2 s
(se cayó el WiFi, MQTT o la Raspberry), manda parar al variador. Si el ESP32
mismo se apaga, el variador para solo por Pr.122.

## 5. La Raspberry

```
cd ~/iot
git pull
docker compose exec web python manage.py setup_planta
```

Copia la línea `VDF_DEVICE_TOKEN=...` que imprime. En el `.env`:

```
VDF_INALAMBRICO=true
VDF_DEVICE_TOKEN=<el token>
```

Y reinicia el puente:

```
docker compose --profile plc up -d --build plc_bridge
docker compose logs -f plc_bridge
```

El log debe decir `Variador inalámbrico: órdenes a ecs/esp32-vdf/orden/set`.

En el dashboard aparece un dispositivo nuevo, **Variador (ESP32 + D700)**,
con la corriente del motor y las luces de enlace y falla. La "Frecuencia
VDF" de la bomba ahora es la **frecuencia real** del variador.

## 6. El PLC

Carga `plc/PLC03_bomba_inalambrico.scl` en vez de `PLC03_bomba.scl`
(External source files → Add new external file → clic derecho →
Generate blocks from source). Cambios respecto del programa con cables:

- Ya no escribe `%Q0.0` ni `%QW96`.
- Publica su orden en `MB_DATA.reg[16]` (marcha) y `reg[17]` (Hz × 10). La
  Raspberry la lee y se la manda al ESP32.
- Recibe en `reg[6]` la frecuencia real y en `reg[7]` el estado del
  variador (bit 0 enlace, bit 1 falla, bit 2 en marcha).
- Hay **falla** (y la bomba no arranca) si se pierde la Raspberry, si se
  pierde el enlace con el variador o si el D700 tiene una alarma. El enlace
  tiene que estar bien 2 s seguidos antes de volver a dar marcha.
- `reg[14]` ahora es la frecuencia real del variador.

El mapa de registros 0 a 5 y 10 a 15 no cambió, así que el dashboard
funciona igual.

## 7. Prueba

1. Variador en **NET**, ESP32 con el LED fijo, `plc_bridge` corriendo.
2. Dashboard: modo manual, velocidad 10 Hz, marcha → la bomba arranca y
   "Frecuencia VDF" sube a ~10 Hz (la real, no la ordenada).
3. Sube a 30 Hz → el variador sigue en ~1 s.
4. **Prueba de seguridad:** con la bomba andando, desenchufa el ESP32 → el
   variador para en ~1 s (alarma E.PUE en el display) y el dashboard muestra
   falla. Vuelve a enchufarlo, resetea el variador (STOP/RESET) y la falla
   se va sola.
5. Con la bomba andando, apaga el WiFi del hotspot → el ESP32 manda parar
   en ~2 s.

## Problemas comunes

| Síntoma | Revisar |
|---|---|
| Monitor serie: "Variador NO responde" | Invertir A y B; Pr.549 = 1; Pr.117 a Pr.120; apagar y encender el variador; GND al pin 1 |
| Responde, pero no arranca | Display en **NET** (Pr.79 = 0, Pr.340 = 1); Pr.338 = 0; ¿hay alarma? (STOP/RESET) |
| Arranca, pero no cambia la velocidad | Pr.339 = 0 |
| Alarma E.PUE apenas enciende | Normal si el ESP32 está apagado; con el ESP32 andando, revisar Pr.122 y el cable |
| LED del ESP32 parpadea y el monitor dice "Fallo MQTT" | IP de la Raspberry, usuario y clave MQTT, misma red WiFi |
| Dashboard con falla y el LED del ESP32 fijo | `VDF_INALAMBRICO=true` y `VDF_DEVICE_TOKEN` en el `.env`; log de `plc_bridge` |

## Probar sin hardware

```
cd plc_bridge
FAKE_VDF_INALAMBRICO=true python fake_plc.py
PLC_HOST=127.0.0.1 PLC_PORT=5020 VDF_INALAMBRICO=true VDF_DEVICE_TOKEN=x python bridge.py
```

Sin el ESP32 el PLC de prueba marca falla, que es lo correcto.
