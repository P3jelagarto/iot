# Puesta en marcha: conectar el PLC y el variador

Guía para leer y cumplir en orden. Empieza donde ya tienes funcionando:

- ✅ La página (plataforma) corriendo en la Raspberry Pi.
- ✅ El ESP32 con el sensor ultrasónico, mostrando el nivel en el dashboard.
- ✅ El programa del PLC armado en TIA Portal ([PLC_LADDER.md](PLC_LADDER.md)).

Cada paso termina con una **✔ Verificación**. No pases al siguiente si la
verificación no se cumple: al final del documento hay una tabla de
problemas típicos.

> ⚠️ **Seguridad.** El variador trabaja con 220 V y guarda energía en sus
> condensadores varios minutos después de apagarlo. Los cables de **potencia**
> (alimentación L1/N o R/S/T y motor U/V/W) los conecta o revisa el profesor o
> el encargado del laboratorio. Tú solo trabajas en las **borneras de
> control** (señales de 24 V y 0-10 V), y siempre con el variador
> **desenergizado**. Además, una bomba **no debe girar en seco**: antes de
> hacerla andar, confirma que tiene agua.

---

## Paso 0. Materiales

- [ ] PLC S7-1200 (CPU 1214C DC/DC/DC) con la signal board **SB 1232 AQ** puesta.
- [ ] Variador Mitsubishi FR-D700 con el motor de la bomba ya conectado (potencia lista).
- [ ] **Relé de 24 VDC** con su base (cualquier relé de interfaz con 1 contacto NA).
- [ ] Fuente de 24 VDC (o la salida de 24 V del propio PLC, bornes L+ / M del sensor supply).
- [ ] Cable de control (0.5-1 mm²), terminales y destornillador de paleta chico.
- [ ] Cable de red y, si hace falta, un switch.
- [ ] Potenciómetro de 1 kΩ (para probar el variador solo, paso 3).
- [ ] Multímetro.
- [ ] El PC con TIA Portal y el proyecto del PLC.

---

## Paso 1. Plan de direcciones IP

Todo lo que habla Modbus (PLC, Raspberry y tu PC con TIA) va en la **misma
red cableada**. El ESP32 sigue por WiFi.

| Equipo | Interfaz | IP | Máscara |
|---|---|---|---|
| PLC S7-1200 | PROFINET | `192.168.0.1` | `255.255.255.0` |
| Raspberry Pi | Ethernet (`eth0`) | `192.168.0.10` | `255.255.255.0` |
| PC con TIA Portal | Ethernet | `192.168.0.50` | `255.255.255.0` |
| Raspberry Pi | WiFi (`wlan0`) | la que le dé la red del lab | |
| ESP32 | WiFi | la que le dé la red del lab | |

Conecta PLC, Raspberry y PC al mismo switch (o, si solo son PLC y
Raspberry, un cable directo entre ambos también sirve).

✔ **Verificación:** tienes anotadas las IP y los tres equipos están en el switch.

---

## Paso 2. IP fija en la Raspberry (Ethernet)

En la Raspberry (Raspberry Pi OS Bookworm), abre una terminal:

```
nmcli con show
```

Busca la conexión cableada (suele llamarse `Wired connection 1`) y dale la IP
fija **sin puerta de enlace**, así el internet sigue saliendo por el WiFi:

```
sudo nmcli con mod "Wired connection 1" ipv4.method manual ipv4.addresses 192.168.0.10/24 ipv4.gateway ""
sudo nmcli con up "Wired connection 1"
```

✔ **Verificación:** `ip addr show eth0` muestra `192.168.0.10/24`, y la
página sigue abriendo en `http://<ip-wifi-de-la-raspberry>:8000/monitoring/`.

> **Si cambiaste la plataforma del PC a la Raspberry:** el `setup.sh` de la
> Raspberry generó contraseñas y tokens **nuevos**. Actualiza en
> `firmware/esp32_ecs_iot/config.h` el `MQTT_BROKER_HOST` (IP **WiFi** de la
> Raspberry), `MQTT_PASSWORD` y `DEVICE_TOKEN` con los del `.env` y la salida
> de `setup_planta` de la Raspberry, y vuelve a subir el sketch al ESP32.

---

## Paso 3. Probar el variador solo (sin PLC)

Así sabes que el D700 y el motor funcionan antes de meter el PLC.

### 3.1 Parámetros

Con el variador **energizado y detenido** (lo enciende el profesor),
configura con el panel (dial y tecla SET) estos parámetros:

| Parámetro | Valor | Qué hace |
|---|---|---|
| Pr.160 | 0 | Muestra todos los parámetros |
| Pr.79 | 2 | Marcha y velocidad desde las borneras |
| Pr.1 | 50 | Frecuencia máxima 50 Hz |
| Pr.7 | 5 | Aceleración en 5 s |
| Pr.8 | 5 | Desaceleración en 5 s |
| Pr.9 | corriente de placa del motor (A) | Protección térmica |
| Pr.73 | 0 | Borne 2 acepta 0-10 V |
| Pr.125 | 50 | 10 V en el borne 2 = 50 Hz |
| Pr.158 | 1 | Borne AM muestra la frecuencia |
| Pr.55 | 50 | 10 V en AM = 50 Hz |

### 3.2 Prueba con potenciómetro

1. **Desenergiza** el variador y espera a que se apague el display.
2. Potenciómetro: extremos a los bornes **10** y **5**, cursor (pata del medio) al borne **2**.
3. Un cable entre **STF** y **SD** (esto es "marcha").
4. Con agua en la bomba, energiza (profesor) y gira el potenciómetro despacio.

✔ **Verificación:** el motor parte, su velocidad sigue al potenciómetro y el
display muestra la frecuencia. Con el multímetro entre **AM** y **5** mides
~0-10 V según la frecuencia (por ejemplo ~5 V a 25 Hz).

5. **Desenergiza** y saca el puente STF-SD y el potenciómetro.

---

## Paso 4. Cableado PLC ↔ variador

Todo con el variador y el PLC **desenergizados**.

| # | Señal | Desde | Hacia | Para qué |
|---|---|---|---|---|
| 1 | Bobina del relé | PLC `Q0.0` | Relé A1 | El PLC activa el relé |
| 2 | Retorno de la bobina | Relé A2 | PLC `M` (0 V) | |
| 3 | Contacto del relé | Relé COM | D700 **STF** | Marcha |
| 4 | Contacto del relé | Relé NA | D700 **SD** | (cierra STF-SD) |
| 5 | Referencia de velocidad | SB 1232 salida `0` (+) | D700 **2** | 0-10 V = 0-50 Hz |
| 6 | Común de la referencia | SB 1232 `M` (−) | D700 **5** | |
| 7 | Frecuencia real | D700 **AM** | CPU entrada analógica `0` | El PLC lee la velocidad |
| 8 | Común de la medición | D700 **5** | CPU `2M` (común analógico) | |
| 9 | Alimentación del contacto de falla | PLC `L+` (24 V) | D700 **A** | |
| 10 | Señal de falla | D700 **C** | PLC `I0.0` | Se cierra cuando hay falla |
| 11 | Común de entradas digitales | PLC `1M` | PLC `M` (0 V) | Para que `I0.0` funcione |

Esquema rápido:

```
PLC S7-1200                         Relé 24 VDC              D700
  Q0.0 ──────────────────────────── A1
  M   ───────────────────────────── A2
                                    COM ──────────────────── STF
                                    NA  ──────────────────── SD
  SB 1232  salida 0 (+) ──────────────────────────────────── 2
  SB 1232  M        (−) ──────────────────────────────────── 5
  AI 0                ◄───────────────────────────────────── AM
  2M                  ◄───────────────────────────────────── 5
  L+ (24 V) ─────────────────────────────────────────────── A
  I0.0                ◄───────────────────────────────────── C
  1M ── M
```

Los nombres exactos de los bornes del PLC están impresos al lado de cada
tornillo: guíate por ellos.

✔ **Verificación (sin energía, con el multímetro en continuidad):**
- Entre STF y SD **no** hay continuidad (el relé está en reposo).
- Cada cable de la tabla llega a donde dice.
- No hay ningún cable de 24 V tocando los bornes 2, 5 o AM del variador.

---

## Paso 5. IP del PLC y carga del programa

1. En TIA: **Configuración de dispositivos** → clic en el puerto PROFINET →
   **Propiedades → Direcciones Ethernet** → IP `192.168.0.1`, máscara
   `255.255.255.0`. Compila.
2. En tu PC, pon la IP fija `192.168.0.50 / 255.255.255.0` en la tarjeta de red.
3. Energiza el PLC (el variador puede seguir apagado).
4. **Cargar en dispositivo** (PN/IE, tu tarjeta de red, Iniciar búsqueda,
   Cargar). Si pregunta, **reinicializa** los DB y al final **Arrancar todo**.

✔ **Verificación:** LED **RUN** verde. En TIA, **Online** y observando
`Control`: después de ~5 s, `comOk = FALSE` y `falla = TRUE`. Es lo
correcto: todavía no hay Raspberry hablando con el PLC.

---

## Paso 6. Conectar la Raspberry con el PLC

1. Desde la Raspberry, prueba la red: `ping 192.168.0.1` (Ctrl+C para cortar).
2. Edita el `.env` de la plataforma (`nano .env` en la carpeta del repo):
   ```
   PLC_HOST=192.168.0.1
   SIMULATE_PLC=false
   SIMULATE_ESP32=false
   ```
   (`PLC_DEVICE_TOKEN` ya lo dejó puesto `setup.sh`; no lo cambies.)
3. Aplica los cambios y levanta el puente:
   ```
   docker compose up -d simulator
   docker compose --profile plc up -d --build
   docker compose logs -f plc_bridge
   ```

✔ **Verificación:**
- El log muestra `MQTT conectado` y **no** repite `PLC no disponible`.
- En TIA, `"MB_DATA".reg[5]` cambia todo el rato, `comOk = TRUE` y `falla = FALSE`.
- En el dashboard, **Bomba (PLC S7-1200 + VDF D700)** aparece **en línea**,
  con Setpoint 70 %, Velocidad manual 35 Hz y Modo automático encendido.

---

## Paso 7. Prueba en manual (desde el dashboard)

Variador energizado por el profesor, con agua en la bomba.

1. **Modo automático** → apágalo (queda "Apagado").
2. **Velocidad manual** → bájala a **10 Hz**.
3. **Marcha bomba** → enciéndela.

✔ **Verificación:**
- Se escucha el clic del relé y el motor parte suave (rampa de 5 s).
- **Frecuencia VDF** en el dashboard sube a ~10 Hz.
- En TIA, `"Ref_VDF"` ≈ 5530 y `"MB_DATA".reg[14]` ≈ 100.

4. Sube la velocidad a **25 Hz**: el motor acelera y el dashboard muestra ~25 Hz.
5. Apaga **Marcha bomba**: el motor frena y la frecuencia vuelve a 0.

---

## Paso 8. Prueba en automático (el sistema completo)

1. Deja el estanque con poca agua (por debajo del setpoint).
2. **Setpoint de nivel** → por ejemplo **50 %**.
3. **Modo automático** → enciéndelo.

✔ **Verificación:**
- La bomba parte sola y el nivel del ESP32 empieza a subir en el gráfico.
- La velocidad baja a medida que el nivel se acerca al setpoint.
- Al llegar a 50 % la bomba se detiene sola.
- Si sacas agua y el nivel baja de 47 %, vuelve a partir.

---

## Paso 9. Prueba de seguridad (watchdog)

Con la bomba andando, **desconecta el cable de red de la Raspberry**.

✔ **Verificación:** en menos de 5 s el PLC detiene la bomba (`comOk = FALSE`,
`falla = TRUE` en TIA). Vuelve a conectar el cable: en unos segundos todo se
recupera y puedes volver a dar marcha.

---

## Paso 10. Listo para la demo

- [ ] Entrar al dashboard como `cliente` / `123456` desde otro equipo de la red: se ve todo en vivo, sin controles.
- [ ] Entrar como `admin` para mostrar el control: cambiar setpoint, modo y velocidad.
- [ ] Mostrar el repo en GitHub y el programa del PLC.
- [ ] Archivar el proyecto de TIA (**Proyecto → Archivar**, genera un `.zap`) y subirlo a una carpeta `plc/` del repo.

---

## Problemas típicos

| Síntoma | Causa probable | Qué revisar |
|---|---|---|
| `ping 192.168.0.1` no responde | IP o cable | IP del PLC en TIA (paso 5), IP fija de la Raspberry (paso 2), luces del switch |
| Log del puente: `PLC no disponible` | Sin conexión al puerto 502 | Que `MB_SERVER` esté en OB1 y el PLC en RUN; `LocalPort = 502` en `MB_CONN`; `PLC_HOST` en `.env` |
| Log del puente: `Error leyendo estado` | El PLC responde pero rechaza la lectura | `MB_DATA` **sin** acceso optimizado; `MB_HOLD_REG` apunta a `"MB_DATA".reg`; `reg` tiene 20 elementos |
| `comOk` siempre FALSE | El watchdog no cambia | Que el puente esté corriendo (`docker compose ps`); en TIA, que `reg[5]` cambie |
| Bomba "en línea" pero los botones no hacen nada | Token o modo | En automático la marcha la decide el PLC (apaga Modo automático para usar Marcha bomba); revisa `PLC_DEVICE_TOKEN` en `.env` |
| El relé hace clic pero el motor no parte | Variador | Pr.79 = 2; STF-SD bien cableado; display del D700 con alarma (E.xxx) |
| El motor parte pero siempre a 0 Hz o muy lento | Referencia analógica | Pr.73 = 0 y Pr.125 = 50; medir 0-10 V entre 2 y 5; dirección `%QW80` de la SB 1232 |
| Frecuencia VDF en el dashboard siempre 0 | Medición AM | Pr.158 = 1, Pr.55 = 50; AI0 en 0-10 V; medir voltaje entre AM y 5 |
| LED Falla encendido | Falla del D700 o sin comunicación | Display del variador (código de alarma); cable A-C; `comOk` |
| El nivel no se mueve en automático | ESP32 | El ESP32 publicando (`SIMULATE_ESP32=false` y el ESP32 conectado al broker de la Raspberry) |
