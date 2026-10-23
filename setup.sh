#!/usr/bin/env bash
# Deja la plataforma lista con un solo comando (Linux, Raspberry Pi o Mac).
#
#   ./setup.sh
#
# Hace, en orden: crea el .env con contraseñas aleatorias, genera el archivo de
# contraseñas de Mosquitto, levanta todo con Docker, registra los dispositivos
# de la planta, guarda sus tokens en el .env y deja listo el config.h del
# ESP32. Es seguro volver a correrlo: no pisa un .env que ya existe.
#
# Con un .env nuevo imprime el usuario y la contraseña del dashboard.
set -euo pipefail
cd "$(dirname "$0")"

die() { echo "ERROR: $*" >&2; exit 1; }
rand() { local hex; hex=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n'); echo "${hex:0:$1}"; }
get() { grep -E "^$1=" .env | head -n1 | cut -d= -f2-; }
set_var() {
  if grep -qE "^$1=" .env; then
    sed -i.bak -E "s|^$1=.*|$1=$2|" .env && rm -f .env.bak
  else
    echo "$1=$2" >> .env
  fi
}

command -v docker >/dev/null || die "Docker no está instalado. En la Raspberry: curl -fsSL https://get.docker.com | sh"
docker compose version >/dev/null 2>&1 || die "Falta el plugin 'docker compose'."
docker info >/dev/null 2>&1 || die "Docker no está corriendo (o tu usuario no tiene permiso: sudo usermod -aG docker \$USER y vuelve a entrar)."

NEW_ENV=false
if [ ! -f .env ]; then
  NEW_ENV=true
  cp .env.example .env
  set_var POSTGRES_PASSWORD "$(rand 24)"
  set_var MQTT_PASSWORD "$(rand 24)"
  set_var DJANGO_SECRET_KEY "$(rand 50)"
  set_var DJANGO_SUPERUSER_PASSWORD "$(rand 16)"
  # Se entra desde otros equipos del laboratorio por la IP de esta máquina.
  set_var DJANGO_ALLOWED_HOSTS "*"
  echo "Creado .env con contraseñas aleatorias."
fi

MQTT_USER=$(get MQTT_USERNAME)
MQTT_PASS=$(get MQTT_PASSWORD)
WEB_PORT=$(get WEB_HOST_PORT)
WEB_PORT=${WEB_PORT:-8000}
MQTT_PORT=$(get MOSQUITTO_HOST_PORT)
MQTT_PORT=${MQTT_PORT:-1883}

if [ ! -f mosquitto/config/passwd ]; then
  echo "Generando contraseñas de Mosquitto..."
  docker run --rm -v "$PWD/mosquitto/config:/mosquitto/config" eclipse-mosquitto:2 \
    sh -c "mosquitto_passwd -b -c /mosquitto/config/passwd '$MQTT_USER' '$MQTT_PASS' && chmod 644 /mosquitto/config/passwd"
fi

echo "Levantando la plataforma (la primera vez demora unos minutos)..."
docker compose up -d --build

echo "Esperando a que el dashboard responda..."
for _ in $(seq 1 60); do
  if curl -fs -o /dev/null "http://localhost:$WEB_PORT/accounts/login/"; then READY=true; break; fi
  READY=false; sleep 3
done
$READY || die "El dashboard no respondió. Revisa: docker compose logs web migrate"

echo "Registrando los dispositivos de la planta..."
OUT=$(docker compose exec -T web python manage.py setup_planta)
SIM_TOKENS=$(echo "$OUT" | grep -E '^SIMULATOR_DEVICE_TOKENS=' | cut -d= -f2-)
PLC_TOKEN=$(echo "$OUT" | grep -E '^PLC_DEVICE_TOKEN=' | cut -d= -f2-)
ESP_TOKEN=$(echo "$SIM_TOKENS" | tr ',' '\n' | grep '^esp32-estanque:' | cut -d: -f2-)
[ -n "$SIM_TOKENS" ] && [ -n "$PLC_TOKEN" ] && [ -n "$ESP_TOKEN" ] || die "No pude leer los tokens de setup_planta."

set_var SIMULATOR_DEVICE_TOKENS "$SIM_TOKENS"
set_var PLC_DEVICE_TOKEN "$PLC_TOKEN"
docker compose up -d simulator

# IP de esta máquina en la red (la que usará el ESP32 para llegar al broker)
HOST_IP=$( (hostname -I 2>/dev/null | awk '{print $1}') || true )
[ -n "${HOST_IP:-}" ] || HOST_IP=$(ipconfig getifaddr en0 2>/dev/null || echo "IP-DE-ESTA-MAQUINA")

CFG=firmware/esp32_ecs_iot/config.h
if [ ! -f "$CFG" ]; then
  cp firmware/esp32_ecs_iot/config.example.h "$CFG"
  sed -i.bak -E \
    -e "s|^#define DEVICE_TOKEN .*|#define DEVICE_TOKEN  \"$ESP_TOKEN\"|" \
    -e "s|^#define MQTT_BROKER_HOST .*|#define MQTT_BROKER_HOST \"$HOST_IP\"|" \
    -e "s|^#define MQTT_BROKER_PORT .*|#define MQTT_BROKER_PORT $MQTT_PORT|" \
    -e "s|^#define MQTT_USERNAME .*|#define MQTT_USERNAME    \"$MQTT_USER\"|" \
    -e "s|^#define MQTT_PASSWORD .*|#define MQTT_PASSWORD    \"$MQTT_PASS\"|" \
    "$CFG" && rm -f "$CFG.bak"
  echo "Creado $CFG para el ESP32 (broker $HOST_IP:$MQTT_PORT)."
fi

echo
echo "=============================================================="
echo " Listo. Dashboard:  http://$HOST_IP:$WEB_PORT/monitoring/"
echo "                    (en esta máquina: http://localhost:$WEB_PORT/monitoring/)"
if $NEW_ENV; then
  echo " Usuario:           $(get DJANGO_SUPERUSER_USERNAME)"
  echo " Contraseña:        $(get DJANGO_SUPERUSER_PASSWORD)"
else
  echo " Usuario y contraseña: están en tu .env (DJANGO_SUPERUSER_*)"
fi
echo " Profesor (solo lectura): cliente / 123456"
echo
echo " Ahora corre el SIMULADOR (nivel y bomba falsos)."
echo " Cuando conectes el hardware real, en .env:"
echo "   SIMULATE_ESP32=false   (ESP32 publicando)"
echo "   SIMULATE_PLC=false     (PLC conectado), PLC_HOST=<ip del PLC>"
echo "   y: docker compose up -d simulator && docker compose --profile plc up -d"
echo "=============================================================="
