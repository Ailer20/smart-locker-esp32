# Smart Locker - protege o banco (regras) e publica o painel na internet (Firebase Hosting)
$ErrorActionPreference = "Continue"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
$enc = New-Object System.Text.UTF8Encoding($false)
function Passo($t) { Write-Host ""; Write-Host "==> $t" -ForegroundColor Cyan }
function Falha($t) { Write-Host ""; Write-Host "ERRO: $t" -ForegroundColor Red; Read-Host "Aperte Enter para fechar"; exit 1 }

Passo "Verificando o Node.js"
if (-not (Get-Command npx -ErrorAction SilentlyContinue)) {
  Write-Host "   Node.js nao encontrado. Instale com:  winget install OpenJS.NodeJS.LTS" -ForegroundColor Yellow
  Write-Host "   (ou baixe em nodejs.org), feche esta janela e rode de novo."
  Falha "Node.js necessario."
}

# Projeto do Firebase: vem da URL configurada no sketch (https://<projeto>-default-rtdb.firebaseio.com)
$ino = Join-Path $dir "sketch\sketch.ino"
$fbUrl = [regex]::Match([IO.File]::ReadAllText($ino, $enc), '#define FIREBASE_URL\s+"([^"]*)"').Groups[1].Value
$proj = [regex]::Match($fbUrl, 'https://([a-z0-9-]+?)(-default-rtdb)?\.').Groups[1].Value
if (-not $proj -or $proj -eq "SEU-PROJETO") { Falha "configure a URL do Firebase no sketch (rode o GRAVAR-ESP32) antes de publicar." }
Write-Host "   Projeto Firebase: $proj"

Passo "Antes de continuar, confirme no console do Firebase:"
Write-Host "   1. Authentication > Metodo de login > E-mail/senha ATIVADO"
Write-Host "   2. Authentication > Usuarios > seu usuario administrador criado"
Write-Host "   3. O ESP32 ja foi gravado com o SEGREDO do banco (GRAVAR-ESP32)."
Write-Host "      Sem isso ele perde o acesso quando as regras forem publicadas!" -ForegroundColor Yellow
$ok = Read-Host "   Tudo certo? (s/n)"
if ($ok -ne "s") { Falha "cancelado. Faca os passos acima e rode de novo." }

Passo "Dados do administrador"
$email = Read-Host "   E-mail do administrador (o mesmo criado no Authentication)"
if (-not $email -or $email -notmatch "@") { Falha "e-mail invalido." }
$key = Read-Host "   Chave da API Web (Configuracoes do projeto > Geral; comeca com AIza)"

$regras = [IO.File]::ReadAllText((Join-Path $dir "database.rules.template.json"), $enc).Replace("__EMAIL_ADMIN__", $email.Trim().ToLower())
[IO.File]::WriteAllText((Join-Path $dir "database.rules.json"), $regras, $enc)

Passo "Preparando o painel"
New-Item -ItemType Directory -Force -Path (Join-Path $dir "publico") | Out-Null
$html = [IO.File]::ReadAllText((Join-Path $dir "painel-admin.html"), $enc)
$html = [regex]::Replace($html, 'const DEFAULT_URL = "[^"]*";', ('const DEFAULT_URL = "' + $fbUrl + '";'))
if ($key) { $html = $html.Replace('const DEFAULT_API_KEY = "";', 'const DEFAULT_API_KEY = "' + $key.Trim() + '";') }
[IO.File]::WriteAllText((Join-Path $dir "publico\index.html"), $html, $enc)
Copy-Item (Join-Path $dir "docs\logo-smart-locker.svg") (Join-Path $dir "publico\logo-smart-locker.svg") -Force -ErrorAction SilentlyContinue

Passo "Entrando na conta Google do Firebase (vai abrir o navegador)"
& npx -y firebase-tools@13 login
Passo "Publicando regras de seguranca e painel"
& npx -y firebase-tools@13 deploy --only "database,hosting" --project $proj
if ($LASTEXITCODE -ne 0) { Falha "a publicacao falhou (veja as mensagens acima)." }

Write-Host ""
Write-Host "PRONTO! Painel no ar em:  https://$proj.web.app" -ForegroundColor Green
Write-Host "Entre com o e-mail e a senha do administrador."
Start-Process "https://$proj.web.app"
Read-Host "Aperte Enter para fechar"
