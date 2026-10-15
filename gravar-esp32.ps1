# Smart Locker - compila e grava o firmware no ESP32 (DOIT ESP32 DEVKIT V1)
$ErrorActionPreference = "Continue"
$URL   = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
$FQBN  = "esp32:esp32:esp32doit-devkit-v1"
$dir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$sketchDir = Join-Path $dir "sketch"
$ino   = Join-Path $sketchDir "sketch.ino"

try { Start-Transcript -Path (Join-Path $dir "gravar-log.txt") -Force | Out-Null } catch {}
function Passo($t) { Write-Host ""; Write-Host "==> $t" -ForegroundColor Cyan }
function Falha($t) { Write-Host ""; Write-Host "ERRO: $t" -ForegroundColor Red; Read-Host "Aperte Enter para fechar"; exit 1 }

# 1. Achar o arduino-cli (vem junto com a Arduino IDE 2)
Passo "Procurando o arduino-cli"
$cands = @(
  "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
  "$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
  "${env:ProgramFiles(x86)}\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
)
$cli = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $cli) { $c = Get-Command arduino-cli -ErrorAction SilentlyContinue; if ($c) { $cli = $c.Source } }
if (-not $cli) { Falha "arduino-cli nao encontrado. Instale a Arduino IDE 2 (arduino.cc/en/software)." }
Write-Host "   $cli"

function Rodar-Cli { & $cli @args --additional-urls $URL 2>&1 | ForEach-Object { "$_" }; if ($LASTEXITCODE -ne 0) { Falha "o comando 'arduino-cli $args' falhou." } }

# 2. Placa ESP32 e bibliotecas
Passo "Atualizando lista de placas"
Rodar-Cli core update-index
Passo "Instalando suporte ao ESP32 (na primeira vez demora alguns minutos)"
Rodar-Cli core install esp32:esp32
Passo "Instalando bibliotecas"
Rodar-Cli lib install MFRC522 ArduinoJson "LiquidCrystal I2C" ESP32Servo

# 3. Wi-Fi
Passo "Configurar Wi-Fi (rede de 2,4 GHz)"
$enc  = New-Object System.Text.UTF8Encoding($false)
$code = [IO.File]::ReadAllText($ino, $enc)
$atual = [regex]::Match($code, '#define WIFI_SSID\s+"([^"]*)"').Groups[1].Value
Write-Host "   Wi-Fi atual no codigo: $atual"
$ssid = Read-Host "   Nome da rede Wi-Fi (Enter = manter)"
if ($ssid) {
  $pass = Read-Host "   Senha do Wi-Fi"
  $code = [regex]::Replace($code, '#define WIFI_SSID\s+"[^"]*"', ('#define WIFI_SSID      "' + $ssid.Replace('\','\\').Replace('"','\"') + '"'))
  $code = [regex]::Replace($code, '#define WIFI_PASS\s+"[^"]*"', ('#define WIFI_PASS      "' + $pass.Replace('\','\\').Replace('"','\"') + '"'))
  [IO.File]::WriteAllText($ino, $code, $enc)
  Write-Host "   Wi-Fi salvo no sketch.ino"
} elseif ($atual -eq "NOME_DO_SEU_WIFI") {
  Write-Host "   Atencao: o Wi-Fi ainda nao foi configurado; o ESP32 vai funcionar so em modo OFFLINE." -ForegroundColor Yellow
}

# 3a. URL do Firebase
$code = [IO.File]::ReadAllText($ino, $enc)
$urlAtual = [regex]::Match($code, '#define FIREBASE_URL\s+"([^"]*)"').Groups[1].Value
Write-Host ""
Write-Host "   Firebase atual: $urlAtual"
$novaUrl = Read-Host "   URL do Realtime Database (Enter = manter)"
if ($novaUrl) {
  $novaUrl = $novaUrl.Trim().TrimEnd('/')
  $code = [regex]::Replace($code, '#define FIREBASE_URL\s+"[^"]*"', ('#define FIREBASE_URL   "' + $novaUrl + '"'))
  [IO.File]::WriteAllText($ino, $code, $enc)
  Write-Host "   URL salva no sketch.ino"
} elseif ($urlAtual -match "SEU-PROJETO") {
  Write-Host "   Atencao: configure a URL do seu Firebase, senao o ESP32 nao salva nada." -ForegroundColor Yellow
}

# 3b. Segredo do Firebase (necessario depois de proteger o banco com regras)
$code = [IO.File]::ReadAllText($ino, $enc)
$temSeg = [regex]::Match($code, '#define FIREBASE_AUTH\s+"([^"]*)"').Groups[1].Value
Write-Host ""
if ($temSeg) { Write-Host "   Segredo do Firebase: ja configurado" } else { Write-Host "   Segredo do Firebase: nao configurado (banco aberto)" }
$seg = Read-Host "   Cole o segredo do banco do Firebase (Enter = manter)"
if ($seg) {
  $code = [regex]::Replace($code, '#define FIREBASE_AUTH\s+"[^"]*"', ('#define FIREBASE_AUTH  "' + $seg.Trim() + '"'))
  [IO.File]::WriteAllText($ino, $code, $enc)
  Write-Host "   Segredo salvo no sketch.ino"
}

# 4. Porta USB
Passo "Procurando o ESP32 na USB"
$json  = & $cli board list --format json | Out-String | ConvertFrom-Json
$lista = if ($json.detected_ports) { $json.detected_ports } else { $json }
$portas = @($lista | Where-Object { $_.port.protocol -eq "serial" -and $_.port.properties.pid } | ForEach-Object { $_.port.address })
if ($portas.Count -eq 0) { $portas = @($lista | Where-Object { $_.port.protocol -eq "serial" } | ForEach-Object { $_.port.address }) }
if ($portas.Count -eq 0) { Falha "nenhuma porta encontrada. Confira o cabo USB (precisa ser de dados) e o driver CP210x/CH340." }
if ($portas.Count -eq 1) { $porta = $portas[0] } else {
  Write-Host "   Portas: $($portas -join ', ')"
  $porta = Read-Host "   Digite a porta do ESP32 (ex.: COM3)"
}
Write-Host "   Usando $porta"

# 5. Compilar e gravar
Passo "Compilando"
& $cli compile --fqbn $FQBN $sketchDir 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { Falha "a compilacao falhou (veja as mensagens acima)." }
Passo "Gravando no ESP32 em $porta  (se travar em 'Connecting...', segure o botao BOOT da placa)"
& $cli upload -p $porta --fqbn $FQBN $sketchDir 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { Falha "a gravacao falhou. Feche o Monitor Serial da Arduino IDE, se estiver aberto, e tente de novo segurando BOOT." }

Write-Host ""
Write-Host "GRAVADO COM SUCESSO!" -ForegroundColor Green
Passo "Monitor Serial (115200). Feche esta janela para sair."
try { Stop-Transcript | Out-Null } catch {}
& $cli monitor -p $porta -c baudrate=115200
