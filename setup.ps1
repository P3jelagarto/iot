# Deja la plataforma lista con un solo comando (Windows + Docker Desktop).
#
#   powershell -ExecutionPolicy Bypass -File .\setup.ps1
#
# Hace lo mismo que setup.sh: crea el .env con contraseñas aleatorias, genera
# las contraseñas de Mosquitto, levanta todo con Docker, registra los
# dispositivos de la planta, guarda sus tokens en el .env y deja listo el
# config.h del ESP32. No pisa un .env que ya existe.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Die($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Rand($n) {
  $bytes = New-Object byte[] 32
  [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
  (($bytes | ForEach-Object { $_.ToString("x2") }) -join "").Substring(0, $n)
}
function Get-Var($name) {
  $line = Get-Content .env | Where-Object { $_ -match "^$name=" } | Select-Object -First 1
  if ($line) { return $line.Substring($name.Length + 1) } else { return "" }
}
function Set-Var($name, $value) {
  $lines = @(Get-Content .env)
  $found = $false
  $lines = $lines | ForEach-Object { if ($_ -match "^$name=") { $found = $true; "$name=$value" } else { $_ } }
  if (-not $found) { $lines += "$name=$value" }
  # UTF-8 sin BOM: docker compose lee mal el BOM en la primera variable
  [System.IO.File]::WriteAllLines((Join-Path $PWD ".env"), $lines)
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { Die "Docker no está instalado. Instala Docker Desktop y ábrelo." }
docker info *> $null
if ($LASTEXITCODE -ne 0) { Die "Docker no está corriendo. Abre Docker Desktop y espera a que diga 'running'." }

$newEnv = $false
if (-not (Test-Path .env)) {
  $newEnv = $true
  Copy-Item .env.example .env
  Set-Var "POSTGRES_PASSWORD" (Rand 24)
  Set-Var "MQTT_PASSWORD" (Rand 24)
  Set-Var "DJANGO_SECRET_KEY" (Rand 50)
  Set-Var "DJANGO_SUPERUSER_PASSWORD" (Rand 16)
  Set-Var "DJANGO_ALLOWED_HOSTS" "*"
  Write-Host "Creado .env con contraseñas aleatorias."
}

$mqttUser = Get-Var "MQTT_USERNAME"
$mqttPass = Get-Var "MQTT_PASSWORD"
$webPort = Get-Var "WEB_HOST_PORT"; if (-not $webPort) { $webPort = "8000" }
$mqttPort = Get-Var "MOSQUITTO_HOST_PORT"; if (-not $mqttPort) { $mqttPort = "1883" }

if (-not (Test-Path mosquitto/config/passwd)) {
  Write-Host "Generando contraseñas de Mosquitto..."
  $cfgDir = (Resolve-Path mosquitto/config).Path
  docker run --rm -v "${cfgDir}:/mosquitto/config" eclipse-mosquitto:2 sh -c "mosquitto_passwd -b -c /mosquitto/config/passwd '$mqttUser' '$mqttPass' && chmod 644 /mosquitto/config/passwd"
  if ($LASTEXITCODE -ne 0) { Die "No pude generar mosquitto/config/passwd." }
}

Write-Host "Levantando la plataforma (la primera vez demora unos minutos)..."
docker compose up -d --build
if ($LASTEXITCODE -ne 0) { Die "docker compose falló. Revisa el mensaje de arriba." }

Write-Host "Esperando a que el dashboard responda..."
$ready = $false
for ($i = 0; $i -lt 60 -and -not $ready; $i++) {
  try { Invoke-WebRequest -UseBasicParsing "http://localhost:$webPort/accounts/login/" | Out-Null; $ready = $true }
  catch { Start-Sleep -Seconds 3 }
}
if (-not $ready) { Die "El dashboard no respondió. Revisa: docker compose logs web migrate" }

Write-Host "Registrando los dispositivos de la planta..."
$out = docker compose exec -T web python manage.py setup_planta
$simTokens = ($out | Where-Object { $_ -match "^SIMULATOR_DEVICE_TOKENS=" } | Select-Object -First 1) -replace "^SIMULATOR_DEVICE_TOKENS=", ""
$plcToken = ($out | Where-Object { $_ -match "^PLC_DEVICE_TOKEN=" } | Select-Object -First 1) -replace "^PLC_DEVICE_TOKEN=", ""
$espToken = (($simTokens -split ",") | Where-Object { $_ -like "esp32-estanque:*" } | Select-Object -First 1) -replace "^esp32-estanque:", ""
if (-not $simTokens -or -not $plcToken -or -not $espToken) { Die "No pude leer los tokens de setup_planta." }

Set-Var "SIMULATOR_DEVICE_TOKENS" $simTokens
Set-Var "PLC_DEVICE_TOKEN" $plcToken
docker compose up -d simulator

# IP de esta máquina en la red (la que usará el ESP32 para llegar al broker)
# Prefiere la tarjeta con puerta de enlace (el WiFi/Ethernet real), no las virtuales de WSL/Docker.
$hostIp = (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq "Up" } |
  Select-Object -First 1).IPv4Address.IPAddress
if (-not $hostIp) { $hostIp = (Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" -and $_.InterfaceAlias -notmatch "vEthernet|WSL|Loopback" } |
  Select-Object -First 1).IPAddress }
if (-not $hostIp) { $hostIp = "IP-DE-ESTA-MAQUINA" }

$cfg = "firmware/esp32_ecs_iot/config.h"
if (-not (Test-Path $cfg)) {
  $text = Get-Content firmware/esp32_ecs_iot/config.example.h | ForEach-Object {
    $_ -replace '^#define DEVICE_TOKEN .*', "#define DEVICE_TOKEN  `"$espToken`"" `
       -replace '^#define MQTT_BROKER_HOST .*', "#define MQTT_BROKER_HOST `"$hostIp`"" `
       -replace '^#define MQTT_BROKER_PORT .*', "#define MQTT_BROKER_PORT $mqttPort" `
       -replace '^#define MQTT_USERNAME .*', "#define MQTT_USERNAME    `"$mqttUser`"" `
       -replace '^#define MQTT_PASSWORD .*', "#define MQTT_PASSWORD    `"$mqttPass`""
  }
  [System.IO.File]::WriteAllLines((Join-Path $PWD $cfg), $text)
  Write-Host "Creado $cfg para el ESP32 (broker ${hostIp}:$mqttPort)."
}

Write-Host ""
Write-Host "=============================================================="
Write-Host " Listo. Dashboard:  http://${hostIp}:$webPort/monitoring/"
Write-Host "                    (en esta máquina: http://localhost:$webPort/monitoring/)"
if ($newEnv) {
  Write-Host " Usuario:           $(Get-Var 'DJANGO_SUPERUSER_USERNAME')"
  Write-Host " Contraseña:        $(Get-Var 'DJANGO_SUPERUSER_PASSWORD')"
} else {
  Write-Host " Usuario y contraseña: están en tu .env (DJANGO_SUPERUSER_*)"
}
Write-Host " Profesor (solo lectura): cliente / 123456"
Write-Host ""
Write-Host " Ahora corre el SIMULADOR (nivel y bomba falsos)."
Write-Host " Cuando conectes el hardware real, en .env:"
Write-Host "   SIMULATE_ESP32=false   (ESP32 publicando)"
Write-Host "   SIMULATE_PLC=false     (PLC conectado), PLC_HOST=<ip del PLC>"
Write-Host "   y: docker compose up -d simulator; docker compose --profile plc up -d"
Write-Host "=============================================================="
