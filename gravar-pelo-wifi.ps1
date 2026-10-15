# Smart Locker - grava o firmware no ESP32 PELO WI-FI (OTA), sem cabo USB
$ErrorActionPreference = "Continue"
$URL  = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
$FQBN = "esp32:esp32:esp32doit-devkit-v1"
$dir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$sketchDir = Join-Path $dir "sketch"
$build = Join-Path $env:TEMP "smartlocker-build"
function Passo($t) { Write-Host ""; Write-Host "==> $t" -ForegroundColor Cyan }
function Falha($t) { Write-Host ""; Write-Host "ERRO: $t" -ForegroundColor Red; Read-Host "Aperte Enter para fechar"; exit 1 }

$cands = @("$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
           "$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe")
$cli = $cands | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cli) { $c = Get-Command arduino-cli -ErrorAction SilentlyContinue; if ($c) { $cli = $c.Source } }
if (-not $cli) { Falha "arduino-cli nao encontrado (instale a Arduino IDE 2)." }

$espota = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\*\tools\espota.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $espota) { Falha "espota.exe nao encontrado. Rode o GRAVAR-ESP32 (USB) uma vez antes." }

Passo "Endereco do ESP32"
Write-Host "   O IP aparece no painel (aba Dispositivo) e no Monitor Serial."
$alvo = Read-Host "   IP do ESP32 (Enter = smartlocker.local)"
if (-not $alvo) { $alvo = "smartlocker.local" }
$senha = Read-Host "   Senha OTA (Enter = smartlocker123)"
if (-not $senha) { $senha = "smartlocker123" }

Passo "Compilando"
& $cli compile --fqbn $FQBN --output-dir $build $sketchDir --additional-urls $URL 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { Falha "a compilacao falhou (veja as mensagens acima)." }
$bin = Join-Path $build "sketch.ino.bin"

Passo "Enviando pelo Wi-Fi para $alvo (o computador precisa estar na mesma rede do ESP32)"
& $espota.FullName -i $alvo -p 3232 "--auth=$senha" -f $bin -r 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { Falha "o envio falhou. Confira o IP, a senha e se o computador esta no mesmo Wi-Fi." }
Write-Host ""; Write-Host "GRAVADO PELO WI-FI COM SUCESSO! O ESP32 vai reiniciar." -ForegroundColor Green
Read-Host "Aperte Enter para fechar"
