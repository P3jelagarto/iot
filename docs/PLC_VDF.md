# PLC S7-1200 + variador Mitsubishi D700 + Raspberry Pi

Guía para conectar el hardware de la planta de llenado de estanque a la
plataforma.

```
                 WiFi (MQTT)                         Ethernet (Modbus TCP)
ESP32 + ultrasónico ─────────► Raspberry Pi ◄──────────────────────────► PLC S7-1200
   (nivel %)                (Docker: Django,                              │  Q0.0 ──► relé ──► STF  ┐
                             Mosquitto, Postgres,                         │  AQ    ──────────► 2/5  │ Mitsubishi
                             plc_bridge)                                  │  AI0   ◄────────── AM/5 │ FR-D700 ──► motor bomba
                                  ▲                                       │  I0.0  ◄────────── A/C  ┘
                                  │ HTTP
                             Navegador (dashboard)
```

- El **ESP32** mide el nivel y lo publica por MQTT.
- La **Raspberry Pi** corre la plataforma y el servicio `plc_bridge`, que
  reenvía el nivel y los comandos del dashboard al PLC por Modbus TCP, y
  publica el estado del PLC.
- El **PLC** tiene la lógica de control: decide la marcha de la bomba y la
  velocidad que le pide al **D700**.

## 1. Mapa de registros Modbus

El PLC actúa como **servidor Modbus TCP** (instrucción `MB_SERVER`) en el
puerto 502. Todos son *holding registers* (40001 = registro 0), tipo `Int`.

| Registro | Dirección | Sentido       | Contenido                          | Escala |
|---------:|-----------|---------------|------------------------------------|--------|
| 0        | 40001     | Raspberry → PLC | Comando marcha (manual) 0/1      | 1      |
| 1        | 40002     | Raspberry → PLC | Comando modo automático 0/1      | 1      |
| 2        | 40003     | Raspberry → PLC | Setpoint de nivel (%)            | ×10    |
| 3        | 40004     | Raspberry → PLC | Velocidad manual (Hz)            | ×10    |
| 4        | 40005     | Raspberry → PLC | Nivel medido por el ESP32 (%)    | ×10    |
| 5        | 40006     | Raspberry → PLC | Watchdog (contador que cambia)   | —      |
| 10       | 40011     | PLC → Raspberry | Bomba en marcha 0/1              | 1      |
| 11       | 40012     | PLC → Raspberry | Modo automático activo 0/1       | 1      |
| 12       | 40013     | PLC → Raspberry | Setpoint en uso (%)              | ×10    |
| 13       | 40014     | PLC → Raspberry | Velocidad manual en uso (Hz)     | ×10    |
| 14       | 40015     | PLC → Raspberry | Frecuencia real del D700 (Hz)    | ×10    |
| 15       | 40016     | PLC → Raspberry | Falla (D700 o sin comunicación)  | 1      |

"×10" significa que 70.5 % viaja como 705.

## 2. Cableado PLC ↔ D700

| Señal                 | PLC S7-1200                    | D700         | Notas |
|-----------------------|--------------------------------|--------------|-------|
| Marcha                | `Q0.0` → bobina de relé 24 VDC | `STF` – `SD` | El contacto del relé une STF con SD. Así funciona tanto con CPU de salidas a transistor (DC/DC/DC) como a relé, sin cambiar la lógica sink/source del variador. |
| Referencia de velocidad | Salida analógica 0-10 V (`%QW80` en una signal board SB 1232 o módulo SM 1232) | `2` (+) / `5` (−) | Si tu CPU es 1215C, sus salidas analógicas son 0-20 mA: ver "Alternativas" abajo. |
| Frecuencia real       | `AI0` de la CPU (0-10 V, `%IW64`) | `AM` (+) / `5` (−) | |
| Falla del variador    | `I0.0` (24 VDC por el contacto) | `A` – `C`   | El contacto A-C se cierra cuando el D700 está en falla. |

**Seguridad:** el lado de potencia (R/S/T o L1/N, U/V/W al motor) debe
cablearlo y revisarlo el profesor o el encargado del laboratorio. Este
proyecto solo usa las borneras de control.

### Alternativas si no tienes salida analógica 0-10 V

- **CPU 1215C (salidas 0-20 mA):** conecta la salida a los terminales `4` (+)
  y `5` (−) del D700 y activa la entrada de corriente (señal AU, ver manual,
  Pr.178–Pr.182 y Pr.267). En el PLC, 4 mA = 5530 y 20 mA = 27648.
- **Sin salida analógica:** usa 3 velocidades fijas con salidas digitales a
  `RH`, `RM`, `RL` (Pr.4, Pr.5, Pr.6). El slider de velocidad del dashboard
  elige la más cercana en el programa del PLC.

## 3. Parámetros del D700

Con el variador detenido, en modo PU:

| Parámetro | Valor | Qué hace |
|-----------|-------|----------|
| Pr.160    | 0     | Muestra todos los parámetros extendidos |
| Pr.79     | 2     | Operación externa (marcha y velocidad desde borneras) |
| Pr.1      | 50    | Frecuencia máxima 50 Hz |
| Pr.7 / Pr.8 | 5   | Tiempo de aceleración / desaceleración (s) |
| Pr.9      | (corriente nominal del motor, placa) | Protección térmica del motor |
| Pr.73     | 0     | Terminal 2 acepta 0-10 V (de fábrica es 0-5 V) |
| Pr.125    | 50    | 10 V en el terminal 2 = 50 Hz |
| Pr.158    | 1     | Terminal AM muestra la frecuencia de salida |
| Pr.55     | 50    | 10 V en AM = 50 Hz |

Prueba antes de conectar el PLC: con un puente STF-SD y un potenciómetro en
10/2/5, el motor debe girar y la frecuencia cambiar con el potenciómetro.

## 4. Programa del PLC (TIA Portal)

### 4.1 Configuración

1. Dispositivo → PROFINET → dirección IP del PLC, por ejemplo `192.168.0.1 / 255.255.255.0`.
2. Crea el bloque de datos **`MB_DATA`** (global DB) con una variable
   `reg : Array[0..19] of Int`. Valores de arranque útiles: `reg[1] = 1`
   (automático), `reg[2] = 700` (setpoint 70 %), `reg[3] = 350` (35 Hz).
3. Crea el DB **`MB_CONN`** con una variable `con : TCON_IP_v4`:
   `InterfaceId` = HW identifier de la interfaz PROFINET (suele ser 64),
   `ID` = 1, `ConnectionType` = 11, `ActiveEstablished` = FALSE,
   `RemotePort` = 0, `LocalPort` = 502.
4. En **OB1** llama a `MB_SERVER` (Instrucciones → Comunicación → Otros →
   MODBUS TCP) con `DISCONNECT := FALSE`, `MB_HOLD_REG := "MB_DATA".reg`,
   `CONNECT := "MB_CONN".con`.
5. Tabla de variables: `Marcha_VDF` `%Q0.0` (Bool), `Ref_VDF` `%QW80` (Int),
   `AI_Frecuencia` `%IW64` (Int), `Falla_VDF` `%I0.0` (Bool).

### 4.2 Bloque de control (FB en SCL)

¿Prefieres ladder (KOP)? El mismo programa, segmento por segmento, está en
[PLC_LADDER.md](PLC_LADDER.md).

Crea un FB **`FB_Bomba`** en SCL y llámalo en OB1 después de `MB_SERVER`.

- **Static:** `wdTimer : TON_TIME`, `wdAnterior : Int`, `comOk : Bool`, `marcha : Bool`
- **Temp:** `nivel : Real`, `setpoint : Real`, `ref : Real`, `falla : Bool`

```pascal
// --- Watchdog: si la Raspberry deja de escribir 5 s, se detiene la bomba ---
#wdTimer(IN := ("MB_DATA".reg[5] = #wdAnterior), PT := T#5S);
#wdAnterior := "MB_DATA".reg[5];
#comOk := NOT #wdTimer.Q;

#nivel := INT_TO_REAL("MB_DATA".reg[4]) / 10.0;
#setpoint := INT_TO_REAL("MB_DATA".reg[2]) / 10.0;

IF "MB_DATA".reg[1] <> 0 THEN
    // Automático: llena hasta el setpoint con histéresis de 3 %,
    // más rápido mientras más lejos está del setpoint.
    IF #nivel >= #setpoint THEN
        #marcha := FALSE;
    ELSIF #nivel < #setpoint - 3.0 THEN
        #marcha := TRUE;
    END_IF;
    #ref := LIMIT(MN := 20.0, IN := 4.0 * (#setpoint - #nivel), MX := 50.0);
ELSE
    // Manual: marcha y velocidad desde el dashboard
    #marcha := "MB_DATA".reg[0] <> 0;
    #ref := INT_TO_REAL("MB_DATA".reg[3]) / 10.0;
END_IF;

#falla := "Falla_VDF" OR NOT #comOk;
IF #falla THEN
    #marcha := FALSE;
END_IF;

// --- Salidas al D700 ---
"Marcha_VDF" := #marcha;
IF #marcha THEN
    "Ref_VDF" := REAL_TO_INT(#ref / 50.0 * 27648.0);   // 0-10 V = 0-50 Hz
ELSE
    "Ref_VDF" := 0;
END_IF;

// --- Estado hacia la Raspberry ---
"MB_DATA".reg[10] := SEL(G := #marcha, IN0 := 0, IN1 := 1);
"MB_DATA".reg[11] := SEL(G := "MB_DATA".reg[1] <> 0, IN0 := 0, IN1 := 1);
"MB_DATA".reg[12] := "MB_DATA".reg[2];
"MB_DATA".reg[13] := "MB_DATA".reg[3];
"MB_DATA".reg[14] := REAL_TO_INT(INT_TO_REAL("AI_Frecuencia") * 500.0 / 27648.0);  // Hz x10
"MB_DATA".reg[15] := SEL(G := #falla, IN0 := 0, IN1 := 1);
```

Es la misma lógica que usan `simulator/simulate.py` y
`plc_bridge/fake_plc.py`, así la demo simulada se comporta igual que la
planta real.

## 5. Raspberry Pi

1. Instala **Raspberry Pi OS de 64 bits** y Docker:
   ```
   curl -fsSL https://get.docker.com | sh
   sudo usermod -aG docker $USER   # cierra sesión y vuelve a entrar
   ```
2. Red: conecta el **Ethernet** de la Raspberry al PLC (o a su switch) con una
   IP fija en la misma red, por ejemplo `192.168.0.10/24`. El **WiFi** queda
   para la red del laboratorio, donde están el ESP32 y el navegador.
3. Clona el repo, crea `.env` desde `.env.example` y genera el archivo de
   contraseñas de Mosquitto (ver README, "Primer arranque").
4. Levanta todo y registra los dispositivos:
   ```
   docker compose up -d --build
   docker compose exec web python manage.py setup_planta
   ```
   Pega en `.env` las líneas que imprime (`SIMULATOR_DEVICE_TOKENS` y
   `PLC_DEVICE_TOKEN`) y pon `PLC_HOST` con la IP del PLC.
5. Con el PLC conectado, cambia en `.env` `SIMULATE_PLC=false` (y
   `SIMULATE_ESP32=false` cuando el ESP32 real esté publicando) y levanta el
   puente:
   ```
   docker compose --profile plc up -d
   docker compose logs -f plc_bridge
   ```
6. Abre `http://<ip-de-la-raspberry>:8000/monitoring/` desde cualquier equipo
   de la red.

### Probar el puente sin el PLC

`plc_bridge/fake_plc.py` es un servidor Modbus que imita al PLC (mismo mapa y
misma lógica):

```
cd plc_bridge
pip install -r requirements.txt
python fake_plc.py                                  # escucha en el puerto 5020
PLC_HOST=127.0.0.1 PLC_PORT=5020 PLC_DEVICE_TOKEN=<token> MQTT_BROKER_HOST=<ip-broker> python bridge.py
```

## 6. Qué se ve en el dashboard

| Gadget            | Dispositivo     | Tipo                | Qué hace |
|-------------------|-----------------|---------------------|----------|
| Nivel del estanque | esp32-estanque | Gráfico de línea    | Nivel medido por el ultrasónico (%) |
| Marcha bomba      | plc-bomba       | Botón               | En manual, arranca o detiene la bomba |
| Modo automático   | plc-bomba       | Botón               | Cambia entre automático y manual |
| Setpoint de nivel | plc-bomba       | Slider 0-100 %      | Nivel al que el PLC llena el estanque en automático |
| Velocidad manual  | plc-bomba       | Slider 0-50 Hz      | Velocidad de la bomba en manual |
| Frecuencia VDF    | plc-bomba       | Medidor 0-50 Hz     | Frecuencia real que entrega el D700 |
| Falla             | plc-bomba       | LED                 | Falla del D700 o pérdida de comunicación |

Los botones y sliders confirman el cambio solo cuando el PLC devuelve el nuevo
estado, así que lo que se ve es lo que el PLC realmente está haciendo.
