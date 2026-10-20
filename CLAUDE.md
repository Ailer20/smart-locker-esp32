# CLAUDE.md — Smart Locker (ESP32 + RFID + Firebase)

Guia para o Claude (ou qualquer pessoa) continuar este projeto. Leia antes de mudar qualquer coisa.

## O que é

Armário inteligente com **4 compartimentos** para laboratório, feito como projeto acadêmico de IoT.

- **Fluxo:** a pessoa aproxima um cartão **RFID**, o **ESP32** consulta o **Firebase Realtime Database** e libera só os compartimentos permitidos, abrindo um **servo** que funciona como trava.
- **Registro:** cada ação vira um evento com data e hora.
- **Painel:** um painel web (HTML único) administra usuários e permissões e mostra tudo **em tempo real**.

Regra de ouro do projeto (requisito do professor): **nenhuma autorização fica fixa no código**. Nunca escreva `if (uid == "...")`. Tudo vem do Firebase, e o cache local (NVS) é só uma cópia para o modo offline.

## Arquitetura

```
 Cartão RFID ──► MFRC522 ──SPI──► ESP32 (sketch/sketch.ino) ──HTTPS REST──► Firebase RTDB  /locker/*
                                   │  ├─ LCD 1602 I2C (mensagens)                 ▲
                                   │  ├─ 4 servos (travas) + 4 botões + buzzer    │ SSE (EventSource) + REST
                                   │  └─ NVS: cache de usuários + config          │
                                   └─ fila de eventos offline (RAM)       painel-admin.html (navegador / Firebase Hosting)
```

- **ESP32 → Firebase:** REST com `HTTPClient` + `WiFiClientSecure` (`setInsecure`). A autenticação é `?auth=<segredo do banco>` (`FIREBASE_AUTH`).
- **Painel → Firebase:** o tempo real usa `EventSource` em `/locker.json` (streaming REST do RTDB). As escritas usam `fetch` (PUT/PATCH/DELETE). O login é o Firebase Auth via REST (`identitytoolkit`), e o token vai como `?auth=<idToken>`.
- **Comandos remotos:** o painel escreve `config/comandos/cN = "abrir" | "fechar" | "testar"`. O ESP32 lê `config` a cada 5 s, executa o comando e o limpa.

## Mapa de arquivos

| Caminho | O que é |
|---|---|
| `sketch/sketch.ino` | Firmware inteiro (um arquivo só, ~650 linhas). Arduino core ESP32 3.x |
| `painel-admin.html` | Painel completo em um único HTML (CSS + JS inline, sem build). Abre direto no navegador |
| `GRAVAR-ESP32.bat` + `gravar-esp32.ps1` | Usa o `arduino-cli` da Arduino IDE 2: instala core e libs, pede Wi-Fi, URL e segredo, compila, grava pelo USB e abre o monitor |
| `GRAVAR-PELO-WIFI.bat` + `gravar-pelo-wifi.ps1` | Compila e envia por OTA (`espota.exe`) |
| `PUBLICAR-PAINEL.bat` + `publicar-painel.ps1` | Gera `database.rules.json` com o e-mail do admin, copia o painel para `publico/` e faz `firebase deploy` (regras + hosting) |
| `database.rules.template.json` | Regras: só `auth.token.email == admin` lê e escreve em `/locker` |
| `firebase.json`, `.firebaserc` | Configuração do Firebase CLI (o projeto é passado por `--project` no script) |
| `wokwi/diagram.json`, `wokwi/libraries.txt` | Circuito para o simulador Wokwi |
| `docs/logo-smart-locker.svg` | Logo (coruja com bico em forma de fechadura) |
| `README.md` | Manual do usuário (em português) |

## Hardware (DOIT ESP32 DEVKIT V1, 30 pinos)

| Peça | Pinos |
|---|---|
| MFRC522 | SS=GPIO17 (TX2), SCK=16 (RX2), MOSI=4, MISO=2. **RST ligado direto no 3V3** (`MFRC522::UNUSED_PIN`). Alimentação **3V3** |
| LCD 1602 + PCF8574 | SDA=23, SCL=22, VCC 5V (VIN). O endereço e a inversão de SDA/SCL são **detectados no boot** (`detectarLCD()`) |
| Servos C1..C4 | 27, 14, 12, 13 (`ESP32Servo`, 0° = travado, 90° = liberado) |
| Botões C1..C4 | 32, 33, 25, 26 (`INPUT_PULLUP`, ativo em LOW) |
| Buzzer | 15 |
| Livres | 21, 19, 18, 5 (já foram de sensores de porta, recurso removido a pedido) |

Servos precisam de **fonte 5 V externa com GND comum**. Pelo USB eles ficam fracos e o LCD escurece.

## Firmware (`sketch/sketch.ino`)

Constantes no topo:
- **Rede e Firebase:** `WIFI_SSID/PASS`, `WIFI_SSID2/PASS2` (reserva), `FIREBASE_URL`, `FIREBASE_AUTH`.
- **OTA e watchdog:** `OTA_NOME`, `OTA_SENHA`, `WATCHDOG_S`.
- **Fuso horário:** `GMT_OFFSET_SEC` (Manaus, UTC-4).

Funções principais:
- **`tratarCartao(uid)`:** o fluxo de autorização. Avalia, nesta ordem:
  1. bloqueio geral;
  2. `buscarUsuario` (Firebase; se offline, cache);
  3. `ativo`;
  4. `verificarRegras` (validade, dias e horário);
  5. permissões.

  Depois entra no estado `ESCOLHENDO` e espera um botão por 20 s.
- **`abrirCompartimento` / `fecharCompartimento`:** mexem no servo, registram o evento e publicam `/compartimentos/cN`. O fechamento é automático depois de `cfg.tempoAbertura`.
- **`registrarEvento(...)`:** monta o JSON com `servidorTs: {".sv":"timestamp"}`. Se o POST falhar, vai para a fila (`MAX_FILA=25`), que é esvaziada ao reconectar.
- **`lerConfigRemota()`:** lê `/locker/config` (tempo, bloqueio, offline, comandos) e salva na NVS.
- **`publicarStatus()`:** a cada 10 s envia `ssid`, `rssi`, `ip`, `uptimeS`, `heapLivre`, `versao` e `ts` para `/locker/status`. O painel considera o ESP32 offline depois de 30 s sem status.
- **`gerenciarWifi()`:** reconecta a cada 8 s, alternando com a rede reserva. Ao conectar, inicia o OTA, esvazia a fila, atualiza o cache e lê a config.
- **Watchdog (`iniciarWatchdog`):** compatível com core 2.x e 3.x (`#if ESP_ARDUINO_VERSION_MAJOR >= 3`). É alimentado no `loop` e dentro de `fbReq`.

Códigos de resultado usados pelo painel (não renomeie sem atualizar `classify()` no painel):
- **Liberado:** `autorizado`, `ok`, `ok_cache`.
- **Negado:** `negado_cartao_desconhecido`, `negado_usuario_inativo`, `negado_cartao_expirado`, `negado_fora_do_horario`, `negado_sem_horario`, `negado_sem_permissao`, `negado_sem_permissoes`, `negado_sistema_bloqueado`, `negado_sem_dados`.

Operações: `identificacao`, `abertura`, `abertura_remota`, `fechamento`, `teste_servo`, `inicializacao`, `alerta_seguranca`.

## Banco (`/locker`)

```
config               { tempoAbertura, bloqueado, permitirOffline, comandoPor, comandos:{c1..c4} }
usuarios/<UID>       { nome, perfil(aluno|professor|tecnico|admin), ativo, permissoes:{c1..c4},
                       horaInicio "HH:MM", horaFim "HH:MM", dias "0123456" (0=dom), validoAte "AAAA-MM-DD" }
compartimentos/<cN>  { estado(aberto|fechado), ultimoUsuario, atualizadoEm }
eventos/<pushId>     { uid, usuario, compartimento, operacao, resultado, origem, horario, epoch, servidorTs }
cartoesDesconhecidos/<UID> { ultimaTentativa, epoch }
status               { online, ssid, rssi, ip, gateway, mac, canal, uptimeS, heapLivre, quedasWifi,
                       redeReserva, eventosNaFila, bloqueado, versao, ota, ts }
```

O UID é hexadecimal maiúsculo, sem separadores (ex.: `611AE7A9`). Os horários do ESP32 (`horario`, `atualizadoEm`) estão em **UTC-4**, no formato `AAAA-MM-DD HH:MM:SS`.

## Painel (`painel-admin.html`)

Arquivo único, sem framework.

- **Telas:** `geral`, `usuarios`, `eventos`, `relatorios`, `dispositivo`, `config`. A navegação é feita por `go(v)` e os títulos ficam em `TITLES`.
- **Estado:**
  - `data` espelha `/locker`;
  - `onChange` aplica os eventos `put`/`patch` do stream;
  - `onNewEvent` dispara o aviso ao vivo, o som e a mensagem no visor (`lcdShow`).
- **Renderização:**
  - `render()` roda a cada mudança e a cada 1 s.
  - Use **`setHTML(el, html)`**, que só troca o DOM quando o HTML muda. Trocar o `innerHTML` direto em listas com botões faz os cliques se perderem.
  - As 4 portas (`renderLockers`) são montadas uma vez e atualizadas no lugar, para a abertura animar.
- **Login:** só é exigido quando existe uma chave de API (`apiKey` no localStorage ou `DEFAULT_API_KEY`). Sem ela, o painel funciona no modo aberto (regras de teste).
- **Visual:**
  - O tema é feito com tokens CSS em `:root` (claro) e `[data-theme=dark]`. Sem preferência salva, segue o tema do sistema.
  - A paleta é **sóbria de propósito**: o dono do projeto pediu para evitar cores neon. Tons quentes (papel e grafite), azul-tinta como destaque e verde/vermelho só para liberado/negado.
  - O dono também pediu para o painel ter **menos cara de IA**. Não use faixa lateral colorida em avisos ou cartões, vidro (`backdrop-filter`), brilho, degradê chamativo, fundo animado nem ícones dentro de quadradinhos coloridos. A cor de estado fica num ponto pequeno.
  - Fontes: IBM Plex Sans (texto) e IBM Plex Mono (UID, IP, horários).
  - **Visor:** `<canvas id="lcd">` reproduz o LCD 16x2 com uma fonte 5x7. `lcdTexto()` traduz cada evento para as mesmas mensagens do sketch. Se mudar um texto do LCD no firmware, atualize lá também.
- **Atalhos:** `Ctrl+K` abre a paleta de comandos e as teclas `1`–`6` trocam de tela.

## Como testar

- **Sintaxe do painel:** extraia o `<script>` e rode `node --check`. Para testar visualmente, use o Playwright interceptando `**/*firebaseio.com/**`: responda JSON para GET e `text/event-stream` com `event: put` para o stream.
- **Firmware:** compile com `arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 sketch` (core `esp32:esp32` 3.x, libs `MFRC522`, `ArduinoJson` v7, `LiquidCrystal I2C`, `ESP32Servo`).
- **Wokwi:** cole o sketch com `WIFI_SSID "Wokwi-GUEST"`, junto com `wokwi/diagram.json`. A RTDB precisa estar com regras abertas ou o segredo configurado.

## Armadilhas já encontradas (não repita)

- **`.ps1`:** o PowerShell 5.1 lê `.ps1` sem BOM como ANSI. Mantenha os scripts em **ASCII puro**, sem acentos.
- **Nomes de função no PowerShell:** não nomeie função como `CLI`, porque `cli` é alias de `Clear-Item`. Use `Rodar-Cli`.
- **Nome da rede:** o hotspot do iPhone pode ter **espaço no fim do nome** ("Ailer "), e o SSID precisa ser exato. O ESP32 só conecta em **2,4 GHz**.
- **LCD com quadradinhos na linha 1:** é falta de comunicação I2C (SDA/SCL trocados ou endereço errado). O `detectarLCD()` já trata os dois casos.
- **HTTP no ESP32:** use um `HTTPClient` global com `setReuse(true)` para manter o TLS e evitar leituras de cartão perdidas.
- **`millis()`:** compare com `(long)(agora - alvo) >= 0`, nunca com subtração de `unsigned` direta.
- **Simulação no Wokwi:** ela pausa quando a aba não está visível.

## Convenções

- **Idioma:** textos de UI, mensagens de LCD (máx. 16 colunas, sem acentos) e comentários ficam em **português**.
- **Segredos:** nunca versione segredos. No repositório `WIFI_*`, `FIREBASE_AUTH` e `FIREBASE_URL` ficam com placeholders (`SEU-PROJETO`). Os scripts gravam os valores reais só na cópia local.
- **Versão:** ao mudar o firmware, atualize `FIRMWARE_VERSAO`. O painel mostra essa versão em *Dispositivo*.
- **Painel e firmware andam juntos:** um campo novo no banco precisa ser tratado nos dois lados e documentado no README.
