# Programa del PLC en ladder (KOP), TIA Portal

Es el mismo programa que el SCL de [PLC_VDF.md](PLC_VDF.md), sección 4.2,
pero en **KOP (ladder)** y paso a paso, para armarlo a mano en TIA Portal.
Usa el mismo mapa de registros Modbus, así que funciona con `plc_bridge`
sin cambiar nada.

Pensado para: CPU 1214C DC/DC/DC (`6ES7 214-1AG40-0XB0`) + signal board
SB 1232 AQ para la referencia 0-10 V. No está probado en un PLC real: la
lógica es la misma que corre el PLC de prueba (`plc_bridge/fake_plc.py`). Si no tienes la SB 1232, mira el
final ("Sin salida analógica").

> Convención de los dibujos: `─┤ ├─` contacto normalmente abierto,
> `─┤/├─` contacto normalmente cerrado, `─( )─` bobina, `─(/)─` bobina
> negada, `─(S)─` / `─(R)─` set / reset. Las cajas (`MOVE`, `TON`, etc.)
> se arrastran desde **Instrucciones básicas**. Los comparadores (`CMP ==`,
> `CMP <>`, `CMP >=`, `CMP <`) están en **Comparación** y funcionan como
> un contacto.

---

## Paso 1. Tabla de variables PLC

**Variables PLC → Tabla de variables estándar**, agrega:

| Nombre          | Tipo | Dirección | Conexión |
|-----------------|------|-----------|----------|
| `Marcha_VDF`    | Bool | `%Q0.0`   | Bobina del relé de interfaz → contacto STF–SD del D700 |
| `Falla_VDF`     | Bool | `%I0.0`   | Contacto A–C del D700 (cierra en falla) |
| `AI_Frecuencia` | Int  | `%IW64`   | AI0 de la CPU ← terminal AM del D700 (0-10 V) |
| `Ref_VDF`       | Int  | `%QW80`   | Salida de la SB 1232 → terminal 2 del D700 (0-10 V) |

Revisa en **Configuración de dispositivos** que la AI0 y la SB 1232 tengan
esas direcciones (`%IW64` y `%QW80` son las de fábrica) y que la AI0 esté en
rango **0-10 V**.

## Paso 2. Bloques de datos

### `MB_DATA` (DB global)

1. **Bloques de programa → Agregar nuevo bloque → Bloque de datos**, nombre `MB_DATA`.
2. Clic derecho en `MB_DATA` → **Propiedades → Atributos** → **desmarca
   "Acceso optimizado al bloque"**. Así Modbus ve los registros en orden.
3. Agrega una variable:

| Nombre | Tipo               | Valor de arranque |
|--------|--------------------|-------------------|
| `reg`  | `Array[0..19] of Int` | ver abajo |

Despliega el array y pon estos valores de arranque (lo que tendrá el PLC
antes de que la Raspberry escriba):

| Elemento  | Valor | Significado |
|-----------|-------|-------------|
| `reg[1]`  | 1     | Arranca en automático |
| `reg[2]`  | 700   | Setpoint 70.0 % |
| `reg[3]`  | 350   | Velocidad manual 35.0 Hz |

### `MB_CONN` (DB global)

| Nombre | Tipo         |
|--------|--------------|
| `con`  | `TCON_IP_v4` |

Valores de arranque de `con`:

| Campo               | Valor | |
|---------------------|-------|-|
| `InterfaceId`       | 64    | El **HW identifier** de la interfaz PROFINET (lo ves en Propiedades de la interfaz → Identificador de hardware; casi siempre 64) |
| `ID`                | 1     | Número de conexión (cualquiera, único) |
| `ConnectionType`    | 11    | TCP |
| `ActiveEstablished` | FALSE | El PLC espera a que la Raspberry se conecte |
| `RemoteAddress`     | 0.0.0.0 | Acepta cualquier cliente |
| `RemotePort`        | 0     | |
| `LocalPort`         | 502   | Puerto Modbus TCP |

### `Control` (DB global)

Variables internas del programa (este puede quedar optimizado):

| Nombre         | Tipo        | Para qué |
|----------------|-------------|----------|
| `wdTimer`      | `IEC_TIMER` | Temporizador del watchdog |
| `wdAnterior`   | Int         | Último valor de watchdog leído |
| `comOk`        | Bool        | La Raspberry está escribiendo |
| `auto`         | Bool        | Modo automático pedido |
| `marchaAuto`   | Bool        | Marcha que decide el modo automático |
| `marchaManual` | Bool        | Marcha pedida desde el dashboard |
| `marcha`       | Bool        | Marcha real hacia el D700 |
| `falla`        | Bool        | Falla del D700 o sin comunicación |
| `nivel`        | Real        | Nivel del estanque (%) |
| `setpoint`     | Real        | Setpoint de nivel (%) |
| `spMenosHist`  | Real        | Setpoint − 3 % (arranque) |
| `refAuto`      | Real        | Velocidad calculada en automático (Hz) |
| `refManual`    | Real        | Velocidad manual (Hz) |
| `ref`          | Real        | Velocidad final (Hz) |
| `tmpReal`      | Real        | Auxiliar para conversiones |
| `tmpNorm`      | Real        | Auxiliar para NORM_X |

## Paso 3. Segmentos en OB1 (Main)

Abre **Main [OB1]** en KOP y crea estos segmentos en este orden (el orden
importa: cada segmento usa lo que calculó el anterior).

### Segmento 1. Servidor Modbus TCP

Arrastra **Comunicación → Otros → MODBUS TCP → MB_SERVER** (acepta crear su
DB de instancia).

```
         ┌──────────────── MB_SERVER ────────────────┐
  ───────┤EN                                      ENO├───
  FALSE ─┤DISCONNECT                         NDR     ├─
         │                                   DR      ├─
  "MB_DATA".reg ─┤MB_HOLD_REG                ERROR   ├─
  "MB_CONN".con ─┤CONNECT                    STATUS  ├─
         └───────────────────────────────────────────┘
```

Si tu versión de `MB_SERVER` no tiene `CONNECT` sino `CONNECT_ID` e
`IP_PORT`, pon `CONNECT_ID = 1` e `IP_PORT = 502`, y no necesitas `MB_CONN`.

### Segmento 2. Watchdog (¿la Raspberry sigue viva?)

Si el registro 5 no cambia en 5 segundos, `comOk` se apaga y la bomba se
detiene.

```
     "MB_DATA".reg[5]       "Control".wdTimer
  ──┤   ==   ├──────────┬──┤ TON ├──Q──────────────────( / )──
       Int              │   PT: T#5S                "Control".comOk
     "Control".wdAnterior
```

- El comparador `==` (tipo **Int**) compara `"MB_DATA".reg[5]` con
  `"Control".wdAnterior`. Mientras sean iguales, el TON cuenta.
- En el TON pon **instancia: `"Control".wdTimer`** (elige "Multiinstancia /
  variable existente" o escribe el nombre) y `PT = T#5S`.
- La bobina es **negada** `─( / )─`: `comOk = NOT Q`.

### Segmento 3. Guardar el watchdog

```
                ┌──── MOVE ────┐
  ──────────────┤EN         ENO├──
  "MB_DATA".reg[5] ─┤IN     OUT1├─ "Control".wdAnterior
                └──────────────┘
```

### Segmento 4. Modo y marcha manual desde el dashboard

Dos ramas en paralelo, cada una con su bobina:

```
     "MB_DATA".reg[1]
  ──┤   <>   ├─────────────────────────────( )── "Control".auto
       Int
        0
     "MB_DATA".reg[0]
  ──┤   <>   ├─────────────────────────────( )── "Control".marchaManual
       Int
        0
```

(Si TIA no te deja dos ramas independientes en un segmento, hazlas en dos
segmentos seguidos.)

### Segmento 5. Nivel y setpoint a Real (÷10)

```
                ┌ CONV Int→Real ┐      ┌──── DIV Real ────┐
  ──────────────┤EN          ENO├──────┤EN             ENO├──
  "MB_DATA".reg[4] ─┤IN     OUT├─ "Control".tmpReal ─┤IN1  OUT├─ "Control".nivel
                └───────────────┘        10.0 ─┤IN2           │
                                               └──────────────┘
```

Repite el mismo par de cajas (puedes copiar el segmento) para:

| Entrada           | Salida               |
|-------------------|----------------------|
| `"MB_DATA".reg[2]` | `"Control".setpoint` |
| `"MB_DATA".reg[3]` | `"Control".refManual` |

### Segmento 6. Setpoint menos histéresis

```
                ┌──── SUB Real ────┐
  ──────────────┤EN             ENO├──
  "Control".setpoint ─┤IN1      OUT├─ "Control".spMenosHist
               3.0 ─┤IN2           │
                └──────────────────┘
```

### Segmento 7. Automático: parar al llegar al setpoint

```
   "Control".auto     "Control".nivel
  ──┤ ├──────────────┤   >=   ├─────────────────(R)── "Control".marchaAuto
                         Real
                     "Control".setpoint
```

### Segmento 8. Automático: arrancar bajo setpoint − 3 %

```
   "Control".auto     "Control".nivel
  ──┤ ├──────────────┤   <    ├─────────────────(S)── "Control".marchaAuto
                         Real
                     "Control".spMenosHist
```

Así la bomba no está prendiendo y apagando todo el rato: arranca bajo
67 % y para en 70 % (con setpoint 70).

### Segmento 9. Automático: velocidad según qué tan lejos está del setpoint

Usa la caja **CALCULATE** (Matemáticas → CALCULATE), tipo **Real**. Haz clic
en el ícono de la calculadora de la caja y escribe la fórmula
`IN1 * (IN2 - IN3)`:

```
                ┌──── CALCULATE Real ────┐      ┌──── LIMIT Real ────┐
  ──────────────┤EN                   ENO├──────┤EN               ENO├──
                │ OUT := IN1*(IN2-IN3)   │  20.0 ┤MN                  │
          4.0 ──┤IN1                  OUT├─ tmp ─┤IN            OUT├─ "Control".refAuto
"Control".setpoint ─┤IN2                 │  50.0 ┤MX                  │
"Control".nivel ──┤IN3                   │      └────────────────────┘
                └────────────────────────┘
```

(`tmp` es `"Control".tmpReal`.) Resultado: a 4 Hz por cada % que falte,
con mínimo 20 Hz y máximo 50 Hz.

### Segmento 10. Falla

```
   "Falla_VDF"
  ──┤ ├──────┬─────────────────────────────────( )── "Control".falla
             │
   "Control".comOk
  ──┤/├──────┘
```

Falla del D700 **o** la Raspberry dejó de escribir.

### Segmento 11. Marcha final hacia el D700

```
   "Control".auto  "Control".marchaAuto
  ──┤ ├────────────┤ ├──────────┬──"Falla_VDF"──"Control".comOk──┬──( )── "Marcha_VDF"
                                │     ┤/├          ┤ ├            │
   "Control".auto  "Control".marchaManual                        └──( )── "Control".marcha
  ──┤/├────────────┤ ├──────────┘
```

- Rama de arriba: automático **y** el automático pide marcha.
- Rama de abajo: manual (`auto` con contacto **cerrado**) **y** el dashboard
  pide marcha.
- En serie: sin falla del D700 (`┤/├`) **y** con comunicación (`┤ ├`).
- Dos bobinas en paralelo al final: la salida física y la copia interna.

### Segmento 12. Velocidad final según el modo

```
   "Control".auto       ┌──── MOVE ────┐
  ──┤ ├─────────────────┤EN         ENO├──
          "Control".refAuto ─┤IN   OUT1├─ "Control".ref
                        └──────────────┘
   "Control".auto       ┌──── MOVE ────┐
  ──┤/├─────────────────┤EN         ENO├──
        "Control".refManual ─┤IN   OUT1├─ "Control".ref
                        └──────────────┘
```

### Segmento 13. Referencia analógica al D700 (0-10 V = 0-50 Hz)

```
   "Marcha_VDF"   ┌──── NORM_X Real ────┐      ┌──── SCALE_X Int ────┐
  ──┤ ├───────────┤EN                ENO├──────┤EN                ENO├──
           0.0 ───┤MIN                  │    0 ┤MIN                   │
  "Control".ref ──┤VALUE             OUT├─ n ──┤VALUE            OUT├─ "Ref_VDF"
          50.0 ───┤MAX                  │ 27648┤MAX                   │
                  └─────────────────────┘      └─────────────────────┘
```

(`n` es `"Control".tmpNorm`.) En `NORM_X` elige tipo de entrada **Real**;
en `SCALE_X`, salida **Int**.

### Segmento 14. Bomba parada: referencia en 0

```
   "Marcha_VDF"   ┌──── MOVE ────┐
  ──┤/├───────────┤EN         ENO├──
              0 ──┤IN       OUT1├─ "Ref_VDF"
                  └──────────────┘
```

### Segmento 15. Estado hacia la Raspberry (registros 10 a 15)

Usa la caja **SEL** (Transferencia → SEL) para pasar Bool a 0/1:

```
                  ┌──── SEL Int ────┐
  ────────────────┤EN            ENO├──
  "Control".marcha ┤G                │
              0 ──┤IN0           OUT├─ "MB_DATA".reg[10]
              1 ──┤IN1              │
                  └─────────────────┘
```

Copia la caja (o haz un segmento por cada una) con:

| G                 | OUT                 |
|-------------------|---------------------|
| `"Control".marcha` | `"MB_DATA".reg[10]` |
| `"Control".auto`   | `"MB_DATA".reg[11]` |
| `"Control".falla`  | `"MB_DATA".reg[15]` |

Y con `MOVE`, devuelve el setpoint y la velocidad que está usando el PLC:

```
                  ┌──── MOVE ────┐
  ────────────────┤EN         ENO├──
  "MB_DATA".reg[2] ┤IN       OUT1├─ "MB_DATA".reg[12]
                  └──────────────┘
                  ┌──── MOVE ────┐
  ────────────────┤EN         ENO├──
  "MB_DATA".reg[3] ┤IN       OUT1├─ "MB_DATA".reg[13]
                  └──────────────┘
```

### Segmento 16. Frecuencia real del D700 → registro 14 (Hz ×10)

```
                  ┌──── NORM_X Int→Real ────┐      ┌──── SCALE_X Real→Int ────┐
  ────────────────┤EN                    ENO├──────┤EN                     ENO├──
              0 ──┤MIN                      │    0 ┤MIN                        │
  "AI_Frecuencia" ┤VALUE                 OUT├─ n ──┤VALUE                 OUT├─ "MB_DATA".reg[14]
          27648 ──┤MAX                      │  500 ┤MAX                        │
                  └─────────────────────────┘      └──────────────────────────┘
```

27648 es lo que lee la CPU con 10 V, y 10 V en AM = 50 Hz (Pr.55 = 50), así
que la salida queda en Hz ×10 (por ejemplo 35.0 Hz → 350).

## Paso 4. Cargar y probar

1. **Compilar** (botón de compilar, sin errores) y **cargar en el dispositivo**.
2. **En línea → Observar** en `MB_DATA`. Sin la Raspberry, después de 5 s
   `"Control".comOk` queda en FALSE y `"Control".falla` en TRUE: es lo correcto.
3. Levanta el puente en la Raspberry (`docker compose --profile plc up -d`).
   `reg[5]` empieza a cambiar, `comOk` pasa a TRUE y en el dashboard aparece
   el estado del PLC.
4. Prueba en **manual** con el motor desacoplado o sin carga: apaga "Modo
   automático", sube "Velocidad manual" a 10 Hz y prende "Marcha bomba". El
   D700 debe arrancar y la "Frecuencia VDF" del dashboard debe subir a ~10 Hz.

## Sin salida analógica (sin SB 1232)

Cambia los segmentos 13 y 14 por 3 velocidades fijas (D700: terminales
RH/RM/RL con Pr.4 = 50 Hz, Pr.5 = 35 Hz, Pr.6 = 20 Hz), cada una con su
relé de interfaz, igual que la marcha:

| Variable | Dirección | D700 |
|----------|-----------|------|
| `Vel_Alta`  | `%Q0.1` | RH |
| `Vel_Media` | `%Q0.2` | RM |
| `Vel_Baja`  | `%Q0.3` | RL |

```
   "Marcha_VDF"   "Control".ref
  ──┤ ├──────────┤   >=   ├──────────────────────────────( )── "Vel_Alta"
                     Real  42.5

   "Marcha_VDF"   "Control".ref      "Control".ref
  ──┤ ├──────────┤   >=   ├─────────┤   <    ├──────────( )── "Vel_Media"
                     Real  27.5          Real  42.5

   "Marcha_VDF"   "Control".ref
  ──┤ ├──────────┤   <    ├──────────────────────────────( )── "Vel_Baja"
                     Real  27.5
```

El slider "Velocidad manual" del dashboard sigue funcionando: el PLC elige la
velocidad fija más cercana.

---

## Tablero PLC03 del laboratorio

Lo que trae ese tablero es distinto de lo que supone esta guía:

| Qué | Esta guía | Tablero PLC03 |
|---|---|---|
| IP del PLC | `192.168.0.1` | `10.41.32.2` (no la cambies, es del laboratorio) |
| Salida analógica | SB 1232 (`%QW80`) | SM 1234 AI/AQ, salida AQ0 (`%QW96` de fábrica) |
| Variador | FR-D700 | FR-D720S-042-NA (es un D700, los parámetros sirven igual) |
| Cableado al variador | STF, SD, 2, 5, AM, A, C | Solo STF, SD, PC, 2 y 5. **AM y A/B/C libres** |

### Cambios en el programa

1. **Tabla de variables:** `Ref_VDF` pasa a `%QW96`, o la dirección que muestre
   el SM 1234 en **Configuración de dispositivos**. Ahí mismo pon su
   salida analógica 0 en **Tensión, ±10 V** (0-10 V = 0-27648, igual que antes).
   Revisa también a qué `%Q` llega el cable de STF y usa esa dirección para `Marcha_VDF`.
2. **Sin señal de falla:** `Falla_VDF` (`%I0.0`) queda siempre en FALSE, así
   que los segmentos 10 y 11 funcionan sin cambios. La falla del dashboard
   solo avisa si se corta la comunicación con la Raspberry.
3. **Sin retorno de frecuencia:** el segmento 16 se reemplaza por esto, que
   informa la velocidad que se le está mandando al variador (Hz ×10):

```
   "Marcha_VDF"   ┌──── MUL Real ────┐      ┌──── CONV Real→Int ────┐
  ──┤ ├───────────┤EN             ENO├──────┤EN                  ENO├──
  "Control".ref ──┤IN1          OUT├─ tmp ──┤IN                  OUT├─ "MB_DATA".reg[14]
           10.0 ──┤IN2              │       └───────────────────────┘
                  └─────────────────┘
   "Marcha_VDF"   ┌──── MOVE ────┐
  ──┤/├───────────┤EN         ENO├──
              0 ──┤IN       OUT1├─ "MB_DATA".reg[14]
                  └──────────────┘
```

(`tmp` es `"Control".tmpReal`.) Si te dejan agregar dos cables, **AM → AI0
del SM 1234** y **5 → M de esa entrada**, vuelve al segmento 16 original con
`AI_Frecuencia` en `%IW96` (entrada 0 del SM 1234 en 0-10 V) y tendrás la frecuencia real.

### Variador

- Puente **SINK/SOURCE en SOURCE**, con el variador apagado. Las salidas del
  S7-1200 entregan +24 V, así que en SINK el STF no se activa. Con SOURCE y
  fuente externa: salida del PLC → STF, `M` del PLC → SD.
- Parámetros: los del paso 3.1 de [PUESTA_EN_MARCHA.md](PUESTA_EN_MARCHA.md).
  **Pr.73 = 0**, porque de fábrica el borne 2 viene en 0-5 V.

### Red

La Raspberry tiene que quedar en la misma red que el PLC. Conéctala a un puerto
libre del switch CSM 1277. Antes de elegir la IP, revisa que nadie la esté usando
(`ping 10.41.32.50` no debe responder):

```
sudo nmcli connection modify plc ipv4.addresses 10.41.32.50/24
sudo nmcli connection up plc
ping -c 3 10.41.32.2
cd ~/iot
sed -i 's/^PLC_HOST=.*/PLC_HOST=10.41.32.2/' .env
```

Antes de cargar tu programa al PLC03, pídele permiso al profesor y respalda
el programa que tiene (**En línea → Cargar de dispositivo**).
