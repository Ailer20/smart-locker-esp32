/*
 * ============================================================
 *  SMART LOCKER - Armario inteligente para laboratorio
 *  ESP32 + MFRC522 (RFID) + Firebase Realtime Database
 * ============================================================
 *  - Identificacao por RFID (MFRC522)
 *  - Usuarios/permissoes ficam no FIREBASE (nada fixo no codigo)
 *  - Registro de eventos com data/hora (NTP)
 *  - Controle remoto: abrir compartimento, bloquear sistema,
 *    alterar tempo de abertura, alterar permissoes
 *  - 4 fechaduras (servos), buzzer e LCD 1602A 16x2 com modulo I2C
 *  - Modo offline: cache das permissoes + fila de eventos
 *  - v3: horario de acesso e validade por usuario, teste de servos pelo
 *        painel, watchdog, atualizacao pelo Wi-Fi (OTA) e Wi-Fi reserva
 *
 *  Estrutura no Firebase (Realtime Database):
 *  /locker
 *    /config            { tempoAbertura, bloqueado, permitirOffline,
 *                         comandos:{c1..c4: "abrir"|"fechar"|"testar"|""} }
 *    /usuarios/<UID>    { nome, perfil, ativo, permissoes:{c1,c2,c3,c4},
 *                         horaInicio:"07:00", horaFim:"18:00",
 *                         dias:"12345" (0=dom..6=sab), validoAte:"2026-12-31" }
 *    /compartimentos/cN { estado, ultimoUsuario, atualizadoEm }
 *    /eventos/<pushId>  { uid, usuario, compartimento, operacao,
 *                         resultado, origem, horario, epoch }
 *    /cartoesDesconhecidos/<UID> { ultimaTentativa, epoch }
 *    /status            { online, ip, rssi, ultimaAtualizacao }
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

// ===================== CONFIGURACOES =====================
#define WIFI_SSID      "NOME_DO_SEU_WIFI"   // no Wokwi use "Wokwi-GUEST"
#define WIFI_PASS      "SENHA_DO_SEU_WIFI"  // no Wokwi deixe ""
// Wi-Fi reserva (opcional): usado quando a rede principal nao responde. Deixe "" se nao tiver.
#define WIFI_SSID2     ""
#define WIFI_PASS2     ""

// URL do seu Realtime Database (sem barra no final)
#define FIREBASE_URL   "https://SEU-PROJETO-default-rtdb.firebaseio.com"
// Segredo do banco (Configuracoes do projeto > Contas de servico >
// Chaves secretas do banco de dados). Deixe "" se as regras estiverem abertas.
#define FIREBASE_AUTH  ""

// Atualizacao do firmware pelo Wi-Fi (OTA). Nome na rede e senha exigida no envio.
#define OTA_NOME       "smartlocker"
#define OTA_SENHA      "smartlocker123"
#define WATCHDOG_S     30            // reinicia o ESP32 se o programa travar por mais que isso

#define GMT_OFFSET_SEC (-4 * 3600)   // Manaus (UTC-4). Brasilia = -3 * 3600
#define NTP_SERVER     "pool.ntp.org"

// ===================== PINOS =====================
// Placa: DOIT ESP32 DEVKIT V1 (30 pinos)
// RFID MFRC522 (SPI): SDA/SS TX2(17), SCK RX2(16), MOSI D4, MISO D2, RST ligado ao 3V3
#define RFID_SS   17
#define RFID_SCK  16
#define RFID_MISO 2
#define RFID_MOSI 4
// LCD 1602A com modulo I2C (PCF8574 / HW-61): SDA D23, SCL D22, VCC 5V, GND
// Endereco I2C: 0x27 (se a tela ficar vazia, teste 0x3F)
#define LCD_SDA  23
#define LCD_SCL  22
#define LCD_ADDR 0x27

const int NUM_COMP = 4;
const int PIN_SERVO[NUM_COMP] = {27, 14, 12, 13};  // fechaduras C1..C4 (D27, D14, D12, D13)
const int PIN_BTN[NUM_COMP]   = {32, 33, 25, 26};  // botoes C1..C4 (D32, D33, D25, D26)

#define BUZZER       15   // D15

// ===================== OBJETOS =====================
MFRC522 rfid(RFID_SS, MFRC522::UNUSED_PIN);   // RST fixo em 3.3V (reset por software)
LiquidCrystal_I2C* lcdPtr = nullptr;      // criado no setup() com o endereco detectado
#define lcd (*lcdPtr)

// Procura o modulo I2C do LCD: testa SDA/SCL normais e invertidos e todos os enderecos
uint8_t detectarLCD() {
  int pinos[2][2] = {{LCD_SDA, LCD_SCL}, {LCD_SCL, LCD_SDA}};
  for (int t = 0; t < 2; t++) {
    Wire.end();
    Wire.begin(pinos[t][0], pinos[t][1]);
    for (uint8_t a = 0x20; a <= 0x3F; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.printf("[LCD] Modulo I2C encontrado no endereco 0x%02X (SDA=GPIO%d, SCL=GPIO%d)%s\n",
                      a, pinos[t][0], pinos[t][1], t ? "  <-- fios SDA/SCL invertidos!" : "");
        return a;
      }
    }
  }
  Serial.println("[LCD] NENHUM modulo I2C encontrado! Confira GND, VCC(5V), SDA->D23 e SCL->D22.");
  Wire.end();
  Wire.begin(LCD_SDA, LCD_SCL);
  return LCD_ADDR;
}
Servo trava[NUM_COMP];
Preferences prefs;
WiFiClientSecure tls;

// ===================== ESTADO =====================
struct Config {
  int  tempoAbertura   = 5;     // segundos
  bool bloqueado       = false; // bloqueio geral remoto
  bool permitirOffline = true;  // usar cache quando sem internet
} cfg;

enum Estado { OCIOSO, ESCOLHENDO };
Estado estado = OCIOSO;

String uidAtual, nomeAtual;
bool   permAtual[NUM_COMP];
unsigned long escolhaInicio = 0;
const unsigned long TEMPO_ESCOLHA = 20000;

unsigned long abertoAte[NUM_COMP] = {0, 0, 0, 0};
bool aberto[NUM_COMP] = {false, false, false, false};

bool btnAnterior[NUM_COMP] = {true, true, true, true};

unsigned long btnTempo[NUM_COMP] = {0, 0, 0, 0};

// Fila de eventos para quando estiver offline
const int MAX_FILA = 25;
String filaEventos[MAX_FILA];
int filaQtd = 0;

// Controle de tentativas invalidas
int tentativasInvalidas = 0;
unsigned long primeiraTentativa = 0;

unsigned long tPoll = 0, tStatus = 0, tWifi = 0, tCache = 0, tLcd = 0;
bool wifiOk = false, ntpOk = false, otaOk = false, wdtAtivo = false;
uint32_t quedasWifi = 0;
int redeAtual = 0;                 // 0 = principal, 1 = reserva
#define FIRMWARE_VERSAO "3.0"

// Prototipos
void iniciarOTA();
void testarServo(int i, const String& quem);
inline void alimentarWdt() { if (wdtAtivo) esp_task_wdt_reset(); }
int  fbReq(const char* metodo, const String& path, const String& corpo = "", String* resp = nullptr);
void abrirCompartimento(int i, const String& usuario, const String& uid, const String& operacao, const String& origem);
void fecharCompartimento(int i, const String& usuario);
void telaOciosa();

// ============================================================
//                        UTILITARIOS
// ============================================================
String agoraTexto() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return "sem-hora+" + String(millis() / 1000) + "s";
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

long agoraEpoch() {
  time_t now; time(&now);
  return (now > 1600000000) ? (long)now : 0;
}

void bip(int freq, int ms) { tone(BUZZER, freq, ms); }

void sinalOk()    { bip(2000, 120); delay(150); }
void sinalErro()  {
  for (int i = 0; i < 3; i++) { bip(400, 150); delay(250); }
}
void sinalAlarme() {
  for (int i = 0; i < 8; i++) { bip(1500, 120); delay(130); bip(900, 120); delay(130); }
}

// Escreve uma linha (0 ou 1) do LCD 16x2, completando com espacos
void lcdLinha(int l, String txt) {
  while (txt.length() < 16) txt += ' ';
  lcd.setCursor(0, l);
  lcd.print(txt.substring(0, 16));
}

String compKey(int i) { return "c" + String(i + 1); }

// ============================================================
//                    FIREBASE (REST API)
// ============================================================
String fbUrl(const String& path) {
  String u = String(FIREBASE_URL) + path + ".json";
  if (strlen(FIREBASE_AUTH) > 0) u += "?auth=" + String(FIREBASE_AUTH);
  return u;
}

// Retorna o codigo HTTP (ou -1 se offline)
HTTPClient http;   // objeto unico: reaproveita a conexao TLS (keep-alive) = bem mais rapido

int fbReq(const char* metodo, const String& path, const String& corpo, String* resp) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  alimentarWdt();
  http.setReuse(true);
  http.setTimeout(6000);
  http.setConnectTimeout(6000);
  if (!http.begin(tls, fbUrl(path))) return -1;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(metodo, corpo);
  if (resp) *resp = (code > 0) ? http.getString() : "";
  http.end();
  if (code <= 0) Serial.printf("[FB] Falha %s %s -> %d\n", metodo, path.c_str(), code);
  return code;
}

// ---------- Eventos ----------
void enviarOuEnfileirar(const String& json) {
  int c = fbReq("POST", "/locker/eventos", json);
  if (c == 200) return;
  if (filaQtd < MAX_FILA) {
    filaEventos[filaQtd++] = json;
  } else { // fila cheia: descarta o mais antigo
    for (int i = 1; i < MAX_FILA; i++) filaEventos[i - 1] = filaEventos[i];
    filaEventos[MAX_FILA - 1] = json;
  }
  Serial.printf("[FILA] Evento guardado offline (%d na fila)\n", filaQtd);
}

void registrarEvento(const String& uid, const String& usuario, int comp,
                     const String& operacao, const String& resultado, const String& origem) {
  JsonDocument d;
  d["uid"] = uid;
  d["usuario"] = usuario;
  d["compartimento"] = comp > 0 ? compKey(comp - 1) : "-";
  d["operacao"] = operacao;
  d["resultado"] = resultado;
  d["origem"] = (WiFi.status() == WL_CONNECTED) ? origem : origem + "-offline";
  d["horario"] = agoraTexto();
  d["epoch"] = agoraEpoch();
  d["servidorTs"][".sv"] = "timestamp";
  String s; serializeJson(d, s);
  Serial.println("[EVENTO] " + s);
  enviarOuEnfileirar(s);
}

void descarregarFila() {
  if (filaQtd == 0 || WiFi.status() != WL_CONNECTED) return;
  Serial.printf("[FILA] Enviando %d eventos pendentes...\n", filaQtd);
  int enviados = 0;
  for (int i = 0; i < filaQtd; i++) {
    if (fbReq("POST", "/locker/eventos", filaEventos[i]) == 200) enviados++;
    else break;
  }
  for (int i = enviados; i < filaQtd; i++) filaEventos[i - enviados] = filaEventos[i];
  filaQtd -= enviados;
}

// ---------- Estado dos compartimentos ----------
void publicarCompartimento(int i, const String& usuario) {
  JsonDocument d;
  d["estado"] = aberto[i] ? "aberto" : "fechado";
  d["ultimoUsuario"] = usuario;
  d["atualizadoEm"] = agoraTexto();
  String s; serializeJson(d, s);
  fbReq("PATCH", "/locker/compartimentos/" + compKey(i), s);
}

void publicarStatus() {
  JsonDocument d;
  d["online"] = true;
  d["ip"] = WiFi.localIP().toString();
  d["rssi"] = WiFi.RSSI();
  d["ultimaAtualizacao"] = agoraTexto();
  d["eventosNaFila"] = filaQtd;
  d["bloqueado"] = cfg.bloqueado;
  // detalhes da rede e do ESP32 para o painel
  d["ssid"] = WiFi.SSID();
  d["canal"] = WiFi.channel();
  d["mac"] = WiFi.macAddress();
  d["gateway"] = WiFi.gatewayIP().toString();
  d["uptimeS"] = (uint32_t)(millis() / 1000);
  d["heapLivre"] = ESP.getFreeHeap();
  d["quedasWifi"] = quedasWifi;
  d["versao"] = FIRMWARE_VERSAO;
  d["redeReserva"] = (redeAtual == 1);
  d["ota"] = otaOk ? (String(OTA_NOME) + ".local") : "";
  d["ts"][".sv"] = "timestamp";      // horario do servidor (o painel usa para saber se esta online)
  String s; serializeJson(d, s);
  fbReq("PATCH", "/locker/status", s);
}

// ---------- Cache local de usuarios (somente copia do Firebase) ----------
void atualizarCacheUsuarios() {
  String r;
  if (fbReq("GET", "/locker/usuarios", "", &r) == 200 && r.length() > 0) {
    prefs.putString("usuarios", r);
    Serial.printf("[CACHE] Usuarios sincronizados (%d bytes)\n", r.length());
  }
}

// Busca o usuario: primeiro no Firebase; se offline, no cache
// Retorna: 1 = encontrado, 0 = nao cadastrado, -1 = sem dados
int buscarUsuario(const String& uid, JsonDocument& user, bool& veioDoCache) {
  String r;
  veioDoCache = false;
  int c = fbReq("GET", "/locker/usuarios/" + uid, "", &r);
  if (c == 200) {
    if (r == "null") return 0;
    if (deserializeJson(user, r)) return -1;
    return 1;
  }
  // Offline -> cache
  if (!cfg.permitirOffline) return -1;
  String cache = prefs.getString("usuarios", "");
  if (cache.length() == 0) return -1;
  JsonDocument todos;
  if (deserializeJson(todos, cache)) return -1;
  veioDoCache = true;
  if (todos[uid].isNull()) return 0;
  user.set(todos[uid]);
  return 1;
}

// ---------- Configuracao e comandos remotos ----------
// Uma unica leitura de /locker/config traz a configuracao E os comandos remotos
// Teste da fechadura pelo painel: gira para 90 graus e volta (nao conta como abertura)
void testarServo(int i, const String& quem) {
  Serial.printf("[SERVO] Teste do C%d (GPIO%d)\n", i + 1, PIN_SERVO[i]);
  lcd.clear(); lcdLinha(0, "Teste servo C" + String(i + 1)); lcdLinha(1, "GPIO" + String(PIN_SERVO[i]));
  for (int k = 0; k < 2; k++) {
    trava[i].write(90); delay(600);
    trava[i].write(0);  delay(600);
  }
  if (aberto[i]) trava[i].write(90);
  registrarEvento("-", quem, i + 1, "teste_servo", "executado", "remoto");
  tLcd = millis() + 800;
}

void executarComando(int i, const String& cmd, const String& quem) {
  // limpa o comando no Firebase para nao repetir
  fbReq("PUT", "/locker/config/comandos/" + compKey(i), "\"\"");
  if (cmd == "abrir") {
    if (cfg.bloqueado) {
      registrarEvento("-", quem, i + 1, "abertura_remota", "negado_sistema_bloqueado", "remoto");
    } else {
      Serial.printf("[REMOTO] Abrindo %s por %s\n", compKey(i).c_str(), quem.c_str());
      abrirCompartimento(i, quem, "-", "abertura_remota", "remoto");
    }
  } else if (cmd == "fechar") {
    if (aberto[i]) fecharCompartimento(i, quem);
  } else if (cmd == "testar") {
    testarServo(i, quem);
  }
}

void lerConfigRemota() {
  String r;
  if (fbReq("GET", "/locker/config", "", &r) != 200 || r == "null") return;
  JsonDocument d;
  if (deserializeJson(d, r)) return;
  int t = d["tempoAbertura"] | cfg.tempoAbertura;
  cfg.tempoAbertura = constrain(t, 2, 60);
  bool b = d["bloqueado"] | false;
  if (b != cfg.bloqueado) {
    cfg.bloqueado = b;
    Serial.printf("[CONFIG] Bloqueio geral: %s\n", b ? "ATIVADO" : "desativado");
    if (b) for (int i = 0; i < NUM_COMP; i++) if (aberto[i]) fecharCompartimento(i, "bloqueio-remoto");
    telaOciosa();
  }
  cfg.permitirOffline = d["permitirOffline"] | true;
  prefs.putInt("tempo", cfg.tempoAbertura);
  prefs.putBool("bloq", cfg.bloqueado);
  prefs.putBool("offline", cfg.permitirOffline);

  String quem = d["comandoPor"] | "admin";
  for (int i = 0; i < NUM_COMP; i++) {
    String cmd = d["comandos"][compKey(i)] | "";
    if (cmd.length() > 0) executarComando(i, cmd, quem);
  }
}

// ============================================================
//                     FECHADURAS (SERVOS)
// ============================================================
void abrirCompartimento(int i, const String& usuario, const String& uid,
                        const String& operacao, const String& origem) {
  trava[i].write(90);              // 90 graus = destravado
  aberto[i] = true;
  abertoAte[i] = millis() + (unsigned long)cfg.tempoAbertura * 1000UL;
  bip(2500, 200);
  lcd.clear();
  lcdLinha(0, "LIBERADO C" + String(i + 1) + " " + String(cfg.tempoAbertura) + "s");
  lcdLinha(1, usuario);
  tLcd = millis() + 2500;
  registrarEvento(uid, usuario, i + 1, operacao, "autorizado", origem);
  publicarCompartimento(i, usuario);
}

void fecharCompartimento(int i, const String& usuario) {
  trava[i].write(0);               // 0 graus = travado
  aberto[i] = false;
  registrarEvento("-", usuario, i + 1, "fechamento", "travado", "sistema");
  publicarCompartimento(i, usuario);
}

// ============================================================
//                          TELAS
// ============================================================
void telaOciosa() {
  estado = OCIOSO;
  lcd.clear();
  // Linha 0: nome + estado da rede | Linha 1: instrucao
  String l0 = wifiOk ? "SmartLocker WiFi" : "SmartLocker OFF ";
  if (!wifiOk && filaQtd > 0) l0 = "OFFLINE fila:" + String(filaQtd);
  lcdLinha(0, l0);
  lcdLinha(1, cfg.bloqueado ? "SIST. BLOQUEADO" : "Aproxime cartao");
}

void telaEscolha() {
  lcd.clear();
  lcdLinha(0, nomeAtual);
  // Ex.: "1OK 2OK 3-- 4--" -> compartimentos liberados para este usuario
  String l = "";
  for (int i = 0; i < NUM_COMP; i++) l += String(i + 1) + (permAtual[i] ? "OK " : "-- ");
  lcdLinha(1, l);
}

// ============================================================
//            REGRAS DE ACESSO: HORARIO E VALIDADE
// ============================================================
// "07:30" -> 450 minutos (ou -1 se invalido)
int minutosDe(const char* s) {
  if (!s || strlen(s) < 4) return -1;
  int h = atoi(s), m = 0;
  const char* p = strchr(s, ':');
  if (p) m = atoi(p + 1);
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

// Retorna "" se pode acessar agora, ou o codigo do motivo da negacao
String verificarRegras(JsonDocument& user, String& msgLcd) {
  const char* validoAte = user["validoAte"] | "";
  const char* hIni = user["horaInicio"] | "";
  const char* hFim = user["horaFim"] | "";
  const char* dias = user["dias"] | "";
  bool temRegra = strlen(validoAte) || strlen(hIni) || strlen(hFim) || strlen(dias);
  if (!temRegra) return "";

  struct tm t;
  if (!getLocalTime(&t, 50)) { msgLcd = "Sem relogio"; return "negado_sem_horario"; }

  if (strlen(validoAte) >= 10) {
    char hoje[11]; strftime(hoje, sizeof(hoje), "%Y-%m-%d", &t);
    if (strcmp(hoje, validoAte) > 0) { msgLcd = "Cartao expirado"; return "negado_cartao_expirado"; }
  }
  if (strlen(dias) > 0) {
    char d = '0' + t.tm_wday;           // 0 = domingo ... 6 = sabado
    if (!strchr(dias, d)) { msgLcd = "Dia nao liberado"; return "negado_fora_do_horario"; }
  }
  int ini = minutosDe(hIni), fim = minutosDe(hFim);
  if (ini >= 0 && fim >= 0) {
    int agora = t.tm_hour * 60 + t.tm_min;
    bool dentro = (ini <= fim) ? (agora >= ini && agora <= fim)      // ex.: 07:00-18:00
                               : (agora >= ini || agora <= fim);     // ex.: 22:00-06:00
    if (!dentro) { msgLcd = String("So ") + hIni + "-" + hFim; return "negado_fora_do_horario"; }
  }
  return "";
}

// ============================================================
//                    PROCESSAMENTO RFID
// ============================================================
void tratarCartao(const String& uid) {
  Serial.println("\n[RFID] Cartao lido: " + uid);
  lcd.clear();
  lcdLinha(0, "ID " + uid);
  lcdLinha(1, "Consultando...");

  if (cfg.bloqueado) {
    lcdLinha(1, "SIST. BLOQUEADO");
    registrarEvento(uid, "-", 0, "identificacao", "negado_sistema_bloqueado", "rfid");
    sinalErro(); delay(1200); telaOciosa();
    return;
  }

  JsonDocument user;
  bool cache;
  int res = buscarUsuario(uid, user, cache);

  if (res == -1) {
    lcdLinha(0, "Sem conexao");
    lcdLinha(1, "Acesso negado");
    registrarEvento(uid, "-", 0, "identificacao", "negado_sem_dados", "rfid");
    sinalErro(); delay(1500); telaOciosa();
    return;
  }

  if (res == 0) {   // ---------- RFID NAO AUTORIZADO ----------
    lcdLinha(0, "Nao cadastrado!");
    lcdLinha(1, "Acesso negado");
    registrarEvento(uid, "desconhecido", 0, "identificacao", "negado_cartao_desconhecido", "rfid");
    JsonDocument d;
    d["ultimaTentativa"] = agoraTexto();
    d["epoch"] = agoraEpoch();
    String s; serializeJson(d, s);
    fbReq("PATCH", "/locker/cartoesDesconhecidos/" + uid, s);

    // 3 tentativas invalidas em 60s -> alarme
    if (millis() - primeiraTentativa > 60000) { tentativasInvalidas = 0; primeiraTentativa = millis(); }
    tentativasInvalidas++;
    if (tentativasInvalidas >= 3) {
      lcdLinha(0, "!!! ALERTA !!!");
      lcdLinha(1, "3 tentativas");
      registrarEvento(uid, "desconhecido", 0, "alerta_seguranca", "3_tentativas_invalidas", "rfid");
      sinalAlarme();
      tentativasInvalidas = 0;
    } else {
      sinalErro();
    }
    delay(1200); telaOciosa();
    return;
  }

  nomeAtual = user["nome"] | "Sem nome";
  bool ativo = user["ativo"] | true;
  if (!ativo) {
    lcdLinha(0, nomeAtual);
    lcdLinha(1, "Usuario suspenso");
    registrarEvento(uid, nomeAtual, 0, "identificacao", "negado_usuario_inativo", "rfid");
    sinalErro(); delay(1500); telaOciosa();
    return;
  }

  String msgRegra;
  String motivo = verificarRegras(user, msgRegra);
  if (motivo.length() > 0) {
    lcdLinha(0, nomeAtual);
    lcdLinha(1, msgRegra);
    registrarEvento(uid, nomeAtual, 0, "identificacao", motivo, "rfid");
    sinalErro(); delay(1800); telaOciosa();
    return;
  }

  bool algum = false;
  for (int i = 0; i < NUM_COMP; i++) {
    permAtual[i] = user["permissoes"][compKey(i)] | false;
    algum |= permAtual[i];
  }
  uidAtual = uid;
  Serial.printf("[RFID] Usuario: %s (%s) %s\n", nomeAtual.c_str(),
                (const char*)(user["perfil"] | "-"), cache ? "[CACHE OFFLINE]" : "[FIREBASE]");

  if (!algum) {
    lcdLinha(0, nomeAtual);
    lcdLinha(1, "Sem permissao");
    registrarEvento(uid, nomeAtual, 0, "identificacao", "negado_sem_permissoes", "rfid");
    sinalErro(); delay(1500); telaOciosa();
    return;
  }

  sinalOk();
  registrarEvento(uid, nomeAtual, 0, "identificacao", cache ? "ok_cache" : "ok", "rfid");
  estado = ESCOLHENDO;
  escolhaInicio = millis();
  telaEscolha();
}

void tratarEscolha(int i) {
  if (permAtual[i]) {
    abrirCompartimento(i, nomeAtual, uidAtual, "abertura", "rfid");
  } else {
    lcd.clear();
    lcdLinha(0, "C" + String(i + 1) + " NEGADO");
    lcdLinha(1, nomeAtual);
    registrarEvento(uidAtual, nomeAtual, i + 1, "abertura", "negado_sem_permissao", "rfid");
    sinalErro();
    tLcd = millis() + 1500;
  }
  estado = OCIOSO;
}

// ============================================================
//                         WI-FI
// ============================================================
void gerenciarWifi() {
  bool agora = (WiFi.status() == WL_CONNECTED);
  if (agora && !wifiOk) {
    wifiOk = true;
    Serial.println("[WIFI] Conectado! IP: " + WiFi.localIP().toString());
    if (!ntpOk) { configTime(GMT_OFFSET_SEC, 0, NTP_SERVER); ntpOk = true; }
    iniciarOTA();
    descarregarFila();
    atualizarCacheUsuarios();
    lerConfigRemota();
    publicarStatus();
    if (estado == OCIOSO && tLcd == 0) telaOciosa();
  } else if (!agora && wifiOk) {
    wifiOk = false;
    quedasWifi++;
    Serial.println("[WIFI] Conexao perdida! Operando em modo OFFLINE (cache).");
    if (estado == OCIOSO) telaOciosa();
  }
  if (!agora && millis() - tWifi > 8000) {
    tWifi = millis();
    // alterna entre a rede principal e a reserva (se existir)
    if (strlen(WIFI_SSID2) > 0) redeAtual = 1 - redeAtual;
    Serial.printf("[WIFI] Tentando reconectar em \"%s\"...\n", redeAtual ? WIFI_SSID2 : WIFI_SSID);
    WiFi.disconnect();
    if (redeAtual) WiFi.begin(WIFI_SSID2, WIFI_PASS2); else WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ============================================================
//              ATUALIZACAO PELO WI-FI (OTA) E WATCHDOG
// ============================================================
void iniciarOTA() {
  if (otaOk) return;
  ArduinoOTA.setHostname(OTA_NOME);
  ArduinoOTA.setPassword(OTA_SENHA);
  ArduinoOTA.onStart([]() {
    if (wdtAtivo) { esp_task_wdt_delete(NULL); wdtAtivo = false; }   // a gravacao demora: pausa o watchdog
    for (int i = 0; i < NUM_COMP; i++) trava[i].write(0);
    lcd.clear(); lcdLinha(0, "Atualizando..."); lcdLinha(1, "Nao desligue!");
    Serial.println("[OTA] Recebendo novo firmware...");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
    static int ult = -1; int pct = total ? (p * 100 / total) : 0;
    if (pct / 10 != ult) { ult = pct / 10; lcdLinha(1, "Gravando " + String(pct) + "%"); }
  });
  ArduinoOTA.onEnd([]() { lcdLinha(1, "Concluido!"); Serial.println("[OTA] Concluido, reiniciando"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Erro %u\n", e); lcdLinha(1, "Erro na gravacao"); esp_task_wdt_add(NULL); wdtAtivo = true; });
  ArduinoOTA.begin();
  otaOk = true;
  Serial.printf("[OTA] Pronto: %s.local (IP %s)\n", OTA_NOME, WiFi.localIP().toString().c_str());
}

void iniciarWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t c = { .timeout_ms = WATCHDOG_S * 1000, .idle_core_mask = 0, .trigger_panic = true };
  if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
#else
  esp_task_wdt_init(WATCHDOG_S, true);
#endif
  esp_task_wdt_add(NULL);
  wdtAtivo = true;
  Serial.printf("[WDT] Watchdog ativo (%d s)\n", WATCHDOG_S);
}

// ============================================================
//                       SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== SMART LOCKER - iniciando ===");

  pinMode(BUZZER, OUTPUT);
  for (int i = 0; i < NUM_COMP; i++) {
    pinMode(PIN_BTN[i], INPUT_PULLUP);
    trava[i].setPeriodHertz(50);
    trava[i].attach(PIN_SERVO[i], 500, 2400);
    trava[i].write(0);   // comeca travado
    Serial.printf("[SERVO] C%d no GPIO%d: %s\n", i + 1, PIN_SERVO[i],
                  trava[i].attached() ? "ok" : "FALHOU ao iniciar");
  }

  lcdPtr = new LiquidCrystal_I2C(detectarLCD(), 16, 2);
  lcd.init();
  lcd.backlight();
  lcdLinha(0, "SMART LOCKER LAB");
  lcdLinha(1, "Iniciando...");

  prefs.begin("locker", false);
  cfg.tempoAbertura   = prefs.getInt("tempo", 5);
  cfg.bloqueado       = prefs.getBool("bloq", false);
  cfg.permitirOffline = prefs.getBool("offline", true);

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  Serial.print("[RFID] Versao do leitor: ");
  rfid.PCD_DumpVersionToSerial();

  tls.setInsecure();   // simplificacao para o simulador (sem validar certificado)

  lcdLinha(1, "Conectando WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { delay(250); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED && strlen(WIFI_SSID2) > 0) {     // tenta a rede reserva
    Serial.printf("\n[WIFI] Principal sem resposta, tentando reserva \"%s\"", WIFI_SSID2);
    lcdLinha(1, "WiFi reserva...");
    redeAtual = 1; WiFi.disconnect(); WiFi.begin(WIFI_SSID2, WIFI_PASS2);
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { delay(250); Serial.print("."); }
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, 0, NTP_SERVER);
    ntpOk = true;
    struct tm t; getLocalTime(&t, 5000);
  }
  gerenciarWifi();
  registrarEvento("-", "sistema", 0, "inicializacao", "ok", "sistema");
  for (int i = 0; i < NUM_COMP; i++) publicarCompartimento(i, "-");
  telaOciosa();
  iniciarWatchdog();
  Serial.println("=== Pronto! Aproxime um cartao (painel do MFRC522) ===");
}

void loop() {
  unsigned long agora = millis();
  alimentarWdt();
  if (otaOk) ArduinoOTA.handle();

  gerenciarWifi();

  // ---------- Leitura RFID ----------
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    tLcd = 0;
    tratarCartao(uid);
  }

  // ---------- Botoes de escolha ----------
  for (int i = 0; i < NUM_COMP; i++) {
    bool b = digitalRead(PIN_BTN[i]);
    if (b != btnAnterior[i] && agora - btnTempo[i] > 50) {
      btnTempo[i] = agora;
      btnAnterior[i] = b;
      if (b == LOW) {
        if (estado == ESCOLHENDO) tratarEscolha(i);
        else { lcd.clear(); lcdLinha(0, "Passe o cartao"); lcdLinha(1, "primeiro!"); bip(600, 100); tLcd = agora + 1200; }
      }
    }
  }

  // ---------- Timeout da escolha ----------
  if (estado == ESCOLHENDO && millis() - escolhaInicio > TEMPO_ESCOLHA) {
    lcd.clear(); lcdLinha(0, "Tempo esgotado"); tLcd = agora + 1000;
    estado = OCIOSO;
  }

  // ---------- Fechamento automatico ----------
  for (int i = 0; i < NUM_COMP; i++) {
    if (aberto[i] && (long)(agora - abertoAte[i]) >= 0) fecharCompartimento(i, "auto");
  }

  // ---------- Volta a tela inicial ----------
  if (tLcd != 0 && (long)(agora - tLcd) >= 0 && estado == OCIOSO) { tLcd = 0; telaOciosa(); }

  // ---------- Sincronizacao com Firebase ----------
  agora = millis();
  if (wifiOk && estado == OCIOSO) {
    if (agora - tPoll > 5000) { lerConfigRemota(); tPoll = millis(); }
    if (agora - tCache > 60000) { atualizarCacheUsuarios(); descarregarFila(); tCache = millis(); }
    if (agora - tStatus > 10000) { publicarStatus(); tStatus = millis(); }
  }

  delay(10);
}
