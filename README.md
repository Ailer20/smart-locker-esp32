<p align="center"><img src="docs/logo-smart-locker.svg" width="96" alt="Smart Locker"></p>

# Smart Locker — Armário inteligente (ESP32 + RFID + Firebase)

Armário com 4 compartimentos controlado por um ESP32. A pessoa aproxima o cartão RFID, o ESP32 consulta as permissões no Firebase e libera só os compartimentos permitidos. Tudo fica registrado com data e hora, e um painel web permite administrar e acompanhar ao vivo.

![ESP32](https://img.shields.io/badge/ESP32-DOIT%20DevKit%20V1-blue) ![Firebase](https://img.shields.io/badge/Firebase-Realtime%20Database-orange) ![Arduino](https://img.shields.io/badge/Arduino-core%203.x-teal)

**Principais recursos:** identificação por RFID, permissões por compartimento, horário e validade por usuário, painel com login e atualização em tempo real, relatórios, abertura remota, funcionamento offline com cache e fila de eventos, watchdog, gravação pelo Wi-Fi (OTA) e Wi-Fi reserva.

> Projeto acadêmico de IoT. A interface e o código estão em português.

## Início rápido

1. **Hardware:** monte o circuito da seção 1.
2. **Firebase:** crie um projeto no [console do Firebase](https://console.firebase.google.com). Abra *Realtime Database → Criar banco → Modo de teste* e copie a URL (`https://<projeto>-default-rtdb.firebaseio.com`).
3. **Gravar:** no Windows, com a Arduino IDE 2 instalada, dê dois cliques em **GRAVAR-ESP32.bat**. Informe o Wi-Fi e a URL do Firebase.
4. **Painel:** abra **painel-admin.html** no Chrome, coloque a URL em *Configurações* e clique em *Criar dados de exemplo*.
5. **Segurança:** antes de usar de verdade (o modo de teste expira em 30 dias), siga a seção 2.2.

**Simulador:** dá para testar sem hardware no [Wokwi](https://wokwi.com). Crie um projeto ESP32 e cole `sketch/sketch.ino`, `wokwi/diagram.json` e `wokwi/libraries.txt`. No sketch, use `WIFI_SSID "Wokwi-GUEST"` e senha `""`.

## Arquivos do projeto

| Arquivo | Para quê |
|---|---|
| `sketch/sketch.ino` | Firmware do ESP32 (v3.0) |
| `GRAVAR-ESP32.bat` | Compila e grava pelo **cabo USB** (pede Wi-Fi, URL e segredo do Firebase) |
| `GRAVAR-PELO-WIFI.bat` | Grava pelo **Wi-Fi (OTA)**, sem cabo |
| `painel-admin.html` | Painel de administração (abre no Chrome) |
| `PUBLICAR-PAINEL.bat` | Protege o banco com regras e publica o painel na internet |
| `database.rules.template.json` | Regras de segurança do Firebase (modelo) |
| `firebase.json`, `.firebaserc` | Configuração do Firebase Hosting |
| `docs/logo-smart-locker.svg` | Logo do sistema |
| `wokwi/diagram.json`, `wokwi/libraries.txt` | Projeto do simulador Wokwi |
| `CLAUDE.md` | Guia do projeto para quem for continuar com o Claude |

---

## 1. Circuito (DOIT ESP32 DEVKIT V1)

| Componente | Pinos do ESP32 | Observação |
|---|---|---|
| RFID MFRC522 | SDA→**D17 (TX2)**, SCK→**D16 (RX2)**, MOSI→**D4**, MISO→**D2**, RST e 3.3V→**3V3**, GND→GND | **Nunca no 5V** |
| LCD 1602A + módulo I2C | SDA→**D23**, SCL→**D22**, VCC→5V, GND→GND | Endereço detectado sozinho |
| Servos C1–C4 (sinal) | **D27, D14, D12, D13** | V+ e GND no trilho de 5V |
| Botões C1–C4 | **D32, D33, D25, D26** | Outra perna no GND |
| Buzzer | **D15** | Outro terminal no GND |

**Alimentação dos servos:** 4 servos puxam mais corrente do que o USB fornece, e o ESP32 pode reiniciar ou os servos não se mexerem. Use uma **fonte de 5 V / 2 A** no trilho de 5 V da protoboard, com o **GND da fonte ligado ao GND do ESP32**.

---

## 2. Firebase e segurança

### 2.1 Banco
Crie um Realtime Database em **modo de teste**. A URL fica assim: `https://<projeto>-default-rtdb.firebaseio.com`. Ela é pedida pelo GRAVAR-ESP32 e configurada no painel.

> ⚠️ No **modo de teste** o banco fica aberto para qualquer pessoa e **para de funcionar 30 dias depois da criação**. Siga a seção 2.2 antes disso.

### 2.2 Proteger o banco (login de administrador)
Faça nesta ordem. Se publicar as regras antes do passo 4, o ESP32 perde o acesso.

1. **Ativar login:** no console do Firebase, abra *Authentication → Começar → Método de login → E-mail/senha → Ativar*.
2. **Criar o administrador:** em *Authentication → Usuários → Adicionar usuário*, cadastre o seu e-mail e uma senha.
3. **Copiar as chaves**, em *Configurações do projeto* (engrenagem):
   - *Geral* → **Chave de API da Web** (começa com `AIza…`). Ela vai para o painel e não é secreta.
   - *Contas de serviço → Chaves secretas do banco de dados → Mostrar* → **segredo do banco**. Ele vai para o ESP32 e **é secreto**.
4. **Gravar o ESP32:** rode o **GRAVAR-ESP32** e cole o **segredo** quando ele pedir.
5. **Publicar:** rode o **PUBLICAR-PAINEL** (precisa do Node.js: `winget install OpenJS.NodeJS.LTS`). Ele pede o e-mail do admin e a chave da API, publica as regras e coloca o painel no ar.
6. **Entrar:** abra **https://<projeto>.web.app** e entre com e-mail e senha. Funciona também no celular.

O que as regras fazem:
- **Painel:** só o e-mail do administrador lê e altera os dados.
- **ESP32:** usa o segredo, que passa pelas regras.
- **Cadastro pelo painel:** se alguém criar uma conta com a chave da API, continua sem acesso.

---

## 3. Gravar o firmware

- **Pelo USB:** dê dois cliques em **GRAVAR-ESP32**.
  - Na primeira vez ele instala o suporte ao ESP32 e as bibliotecas.
  - Ele pede o Wi-Fi (rede de **2,4 GHz**), a URL e o segredo do Firebase. Enter mantém o que já está salvo.
  - Se parar em "Connecting…", segure o botão **BOOT**.
- **Pelo Wi-Fi (OTA):** dê dois cliques em **GRAVAR-PELO-WIFI**.
  - O computador precisa estar na mesma rede do ESP32.
  - Endereço: `smartlocker.local`, ou o IP que aparece no painel em *Dispositivo*.
  - Senha OTA padrão: `smartlocker123`. Troque em `OTA_SENHA` no sketch.
- **Wi-Fi reserva:** preencha `WIFI_SSID2` e `WIFI_PASS2` no sketch. Se a rede principal cair, o ESP32 alterna entre as duas.
- **Watchdog:** se o programa travar por mais de 30 s, o ESP32 reinicia sozinho.

---

## 4. Painel de administração

| Tela | O que faz |
|---|---|
| **Visão geral** | Wi-Fi do ESP32 (rede, sinal, IP), reprodução do visor LCD, aberturas e negados do dia, os 4 compartimentos (com abertura remota), gráfico de 24 h e atividade **ao vivo** |
| **Usuários** | Cadastro de cartões, permissões C1–C4, **horário de acesso**, **dias da semana**, **data de validade**, ativar ou desativar e excluir |
| **Eventos** | Histórico completo com filtros e exportação CSV |
| **Relatórios** | Aberturas por usuário e por compartimento, horários de pico, negados por motivo; **Imprimir / salvar PDF** |
| **Dispositivo** | Detalhes do Wi-Fi e do ESP32, gráfico do sinal e **Testar fechaduras** (gira cada servo e volta) |
| **Configurações** | Tempo de abertura, acesso offline, bloqueio geral e conexão |

**Tempo real:** quando alguém abre ou é negado, aparece um aviso no topo, toca um som, a porta do compartimento abre na tela e o visor do painel mostra a mesma mensagem do LCD. Atalhos: **Ctrl+K** abre a busca e as teclas **1–6** trocam de tela.

### Regras de acesso avaliadas pelo ESP32
O ESP32 avalia, nesta ordem:
1. Bloqueio geral
2. Cartão cadastrado
3. Usuário ativo
4. Validade
5. Dia da semana e horário
6. Permissão do compartimento

Os motivos de negação são registrados assim: `negado_cartao_desconhecido`, `negado_usuario_inativo`, `negado_cartao_expirado`, `negado_fora_do_horario`, `negado_sem_permissao`, `negado_sistema_bloqueado`, `negado_sem_dados`.

---

## 5. Estrutura do banco

```
/locker
  /config                 { tempoAbertura, bloqueado, permitirOffline,
                            comandoPor, comandos:{c1..c4: "abrir"|"fechar"|"testar"} }
  /usuarios/<UID>         { nome, perfil, ativo, permissoes:{c1..c4},
                            horaInicio "07:00", horaFim "18:00", dias "12345" (0=dom), validoAte "2026-12-31" }
  /compartimentos/<cN>    { estado, ultimoUsuario, atualizadoEm }
  /eventos/<id>           { uid, usuario, compartimento, operacao, resultado, origem, horario, epoch, servidorTs }
  /cartoesDesconhecidos/<UID> { ultimaTentativa, epoch }
  /status                 { online, ssid, rssi, ip, gateway, mac, canal, uptimeS, heapLivre,
                            quedasWifi, redeReserva, eventosNaFila, versao, ota, ts }
```

---

## 6. Requisitos do projeto

| # | Requisito | Como foi atendido |
|---|---|---|
| 1 | ESP32 + MFRC522 | DOIT ESP32 DEVKIT V1 + leitor MFRC522 via SPI |
| 2 | Firebase para persistência | Realtime Database (`/locker/*`) |
| 3 | Identificação por RFID | UID lido pelo MFRC522 e consultado no Firebase |
| 4 | Eventos com data/hora | NTP no ESP32 + carimbo do servidor (`servidorTs`) |
| 5 | Consulta de dados | ESP32 consulta usuário e config; painel lista, filtra e gera relatórios |
| 6 | Alteração remota | Permissões, horários, validade, bloqueio, tempo e abertura remota pelo painel |
| 7 | Decisão no ESP32 com dados do Firebase | Libera ou nega por ativo, validade, dia, horário, permissão e bloqueio |
| 8 | Atuador físico | 4 servos (fechaduras) + buzzer + LCD |
| 9 | RFID não autorizado | Mensagem, buzzer, evento, lista para cadastro, alarme após 3 tentativas |
| 10 | Perda de Wi-Fi | Reconexão automática, Wi-Fi reserva, cache das permissões, fila de eventos offline |
| 11 | Mais de um usuário | Cadastro ilimitado pelo painel, cada um com suas permissões e horários |
| 12 | Interface de administração | Painel web com login, tempo real, relatórios e controle remoto |

**Nenhum UID fica fixo no código.** Toda a autorização vem do Firebase. O cache local é só uma cópia, usada quando não há internet (dá para desligar em *Acesso sem internet*).

---

## 7. Roteiro da demonstração (2–3 min, bom para gravar em vídeo)

1. **Abertura (15 s):** mostre o painel com o ESP32 online, o nome do Wi-Fi e o sinal.
2. **Acesso liberado (30 s):** passe o cartão do admin, escolha C1 no botão, o servo abre e o painel mostra o aviso ao vivo.
3. **Cartão desconhecido (30 s):** passe um cartão novo, que é negado. No painel, clique em *Cadastrar*, marque só C2 e passe de novo: agora funciona.
4. **Sem permissão / horário (20 s):** tente abrir C3 com esse cartão (negado), ou limite o horário dele e mostre a negação "Fora do horário".
5. **Controle remoto (20 s):** use *Abrir remotamente* no C4, depois ative o *Bloqueio geral* e mostre que nada abre.
6. **Queda de Wi-Fi (30 s):** desligue o roteador do celular. O LCD mostra OFFLINE e o cartão continua funcionando pelo cache. Religue: a fila de eventos é enviada.
7. **Relatórios (15 s):** mostre a tela de Relatórios e o histórico com data e hora.

> Grave o vídeo antes da apresentação, como plano B.

---

## 8. Problemas comuns

| Sintoma | Solução |
|---|---|
| LCD aceso com quadradinhos | Fios SDA/SCL invertidos ou soltos. O Monitor Serial mostra `[LCD] ...` |
| LCD aceso sem letras | Gire o potenciômetro azul atrás do display |
| Servo não gira | Fonte 5 V externa e GND comum. Teste em *Dispositivo → Testar fechaduras* |
| ESP32 não conecta | Rede precisa ser 2,4 GHz. Nome e senha são exatos, inclusive espaços (o iPhone pode pôr espaço no nome) |
| Painel "sem permissão" | Faça login. Confira a chave da API e se o e-mail é o mesmo das regras |
| ESP32 com erro 401 | Falta o segredo do banco no firmware. Rode o GRAVAR-ESP32 e cole o segredo |
| Upload trava em "Connecting…" | Segure **BOOT** durante o envio. Feche o Monitor Serial da Arduino IDE |

---

## Autor

**Ailer Pimentel** — [@Ailer20](https://github.com/Ailer20)

Desenvolvido com a ajuda do Claude (Anthropic).
