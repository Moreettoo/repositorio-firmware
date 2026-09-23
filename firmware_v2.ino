#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>


const char* FW_VERSION = "2.0";

const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_PASS  = "";
const int   WIFI_CANAL = 6;
const unsigned long WIFI_TIMEOUT_MS = 15000;

const char* MANIFEST_URL =
  "https://raw.githubusercontent.com/Moreettoo/repositorio-firmware/main/version.json";

const int PIN_LED_R = 25;
const int PIN_LED_G = 26;
const int PIN_LED_B = 27;

const int NUM_LEITURAS = 5;
const unsigned long INTERVALO_LEITURA_MS = 2000;
const unsigned long INTERVALO_SESSAO_MS  = 48000;
const int ALTURA_MIN_CM = 10;
const int ALTURA_MAX_CM = 20;

const unsigned int SESSOES_ANTES_DA_OTA = 3;


const int LIMITE_ALERTA_CM = 16;  
const int LIMITE_NORMAL_CM = 14;  


int leituras[NUM_LEITURAS];         
int leiturasFeitas = 0;
bool sessaoEmAndamento = false;
unsigned long inicioSessao = 0;   
unsigned int numeroSessao = 0;
int ultimoPercentualOta = -10;

enum EstadoSistema { NORMAL, ALERTA };
EstadoSistema estadoAtual = NORMAL; 


void definirCor(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

void ledEstado() {
  if (estadoAtual == ALERTA) definirCor(true, false, false); 
  else                       definirCor(false, true, false); 
}

void ledOtaEmAndamento() { definirCor(true, false, true); }   
void sinalizarErro() { 
  for (int i = 0; i < 3; i++) {
    definirCor(true, true, false);
    delay(200);
    definirCor(false, false, false);
    delay(200);
  }
  ledEstado();
}


bool conectarWiFi() {
  Serial.printf("[WiFi] Conectando a %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Conectado. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("[WiFi] Falha na conexao. As medicoes continuam; a OTA sera tentada depois.");
  return false;
}

bool garantirWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[WiFi] Sem conexao Wi-Fi. Tentando reconectar...");
  WiFi.disconnect();
  return conectarWiFi();
}

bool iniciarHttp(HTTPClient& http, const String& url,
                 WiFiClientSecure& clienteSeguro, WiFiClient& clienteSimples) {
  if (url.startsWith("https://")) {
    clienteSeguro.setInsecure();
    return http.begin(clienteSeguro, url);
  }
  return http.begin(clienteSimples, url);
}

int compararVersoes(const String& a, const String& b) {
  int ia = 0, ib = 0;
  while (ia < (int)a.length() || ib < (int)b.length()) {
    long na = 0, nb = 0;
    while (ia < (int)a.length() && a[ia] != '.') {
      if (isDigit(a[ia])) na = na * 10 + (a[ia] - '0');
      ia++;
    }
    while (ib < (int)b.length() && b[ib] != '.') {
      if (isDigit(b[ib])) nb = nb * 10 + (b[ib] - '0');
      ib++;
    }
    if (na != nb) return (na > nb) ? 1 : -1;
    ia++;
    ib++;
  }
  return 0;
}

bool buscarManifesto(String& versao, String& urlFirmware) {
  WiFiClientSecure clienteSeguro;
  WiFiClient clienteSimples;
  HTTPClient http;

  String url = String(MANIFEST_URL) + "?nocache=" + String(esp_random());

  Serial.printf("[OTA] Consultando manifesto: %s\n", MANIFEST_URL);
  if (!iniciarHttp(http, url, clienteSeguro, clienteSimples)) {
    Serial.println("[OTA] Erro: URL do manifesto invalida.");
    return false;
  }
  http.setTimeout(10000);

  int codigo = http.GET();
  if (codigo != HTTP_CODE_OK) {
    if (codigo < 0) {
      Serial.printf("[OTA] Erro: manifesto inacessivel (%s).\n",
                    HTTPClient::errorToString(codigo).c_str());
    } else {
      Serial.printf("[OTA] Erro: manifesto inacessivel (HTTP %d).\n", codigo);
    }
    http.end();
    return false;
  }

  String corpo = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, corpo);
  if (erro) {
    Serial.printf("[OTA] Erro: manifesto com JSON invalido (%s).\n", erro.c_str());
    return false;
  }

  versao = String(doc["version"] | "");
  urlFirmware = String(doc["url"] | "");
  versao.trim();
  urlFirmware.trim();

  if (versao.length() == 0 || urlFirmware.length() == 0) {
    Serial.println("[OTA] Erro: manifesto sem os campos \"version\" e/ou \"url\".");
    return false;
  }
  return true;
}

void mostrarProgresso(size_t gravado, size_t total) {
  if (total == 0) return;
  int pct = (int)((gravado * 100ULL) / total);
  if (pct / 10 != ultimoPercentualOta / 10) {
    ultimoPercentualOta = pct;
    Serial.printf("[OTA] Gravando... %3d%% (%u/%u bytes)\n",
                  pct, (unsigned)gravado, (unsigned)total);
  }
}

bool baixarEAtualizar(const String& urlFirmware) {
  WiFiClientSecure clienteSeguro;
  WiFiClient clienteSimples;
  HTTPClient http;

  ledOtaEmAndamento();
  Serial.printf("[OTA] Baixando firmware: %s\n", urlFirmware.c_str());

  if (!iniciarHttp(http, urlFirmware, clienteSeguro, clienteSimples)) {
    Serial.println("[OTA] Erro: URL do firmware invalida.");
    return false;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int codigo = http.GET();
  if (codigo != HTTP_CODE_OK) {
    if (codigo < 0) {
      Serial.printf("[OTA] Erro: firmware nao pode ser baixado (%s).\n",
                    HTTPClient::errorToString(codigo).c_str());
    } else {
      Serial.printf("[OTA] Erro: firmware nao pode ser baixado (HTTP %d).\n", codigo);
    }
    http.end();
    return false;
  }

  int tamanho = http.getSize();
  if (tamanho > 0) {
    Serial.printf("[OTA] Arquivo encontrado: %d bytes\n", tamanho);
  } else {
    Serial.println("[OTA] Arquivo encontrado (tamanho nao informado pelo servidor).");
  }

  if (!Update.begin(tamanho > 0 ? (size_t)tamanho : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Erro ao preparar a gravacao: %s\n", Update.errorString());
    http.end();
    return false;
  }

  ultimoPercentualOta = -10;
  Update.onProgress(mostrarProgresso);
  size_t gravados = Update.writeStream(*http.getStreamPtr());

  if (tamanho > 0 && gravados != (size_t)tamanho) {
    Serial.printf("[OTA] Erro: download incompleto (%u de %d bytes).\n",
                  (unsigned)gravados, tamanho);
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end(tamanho <= 0)) {
    Serial.printf("[OTA] Erro ao finalizar a atualizacao: %s\n", Update.errorString());
    http.end();
    return false;
  }
  http.end();

  Serial.println("[OTA] Firmware gravado e validado com sucesso!");
  Serial.println("[OTA] Reiniciando em 3 segundos...");
  delay(3000);
  ESP.restart();
  return true;
}

void verificarAtualizacao() {
  Serial.println();
  Serial.println("--------- VERIFICACAO DE ATUALIZACAO (OTA) ---------");

  if (!garantirWiFi()) {
    Serial.println("[OTA] Sem conexao Wi-Fi. Nova tentativa na proxima sessao.");
    sinalizarErro();
    return;
  }

  String versaoDisponivel, urlFirmware;
  if (!buscarManifesto(versaoDisponivel, urlFirmware)) {
    Serial.println("[OTA] Nova tentativa na proxima sessao.");
    sinalizarErro();
    return;
  }

  Serial.printf("[OTA] Versao instalada:  %s\n", FW_VERSION);
  Serial.printf("[OTA] Versao disponivel: %s\n", versaoDisponivel.c_str());

  int comparacao = compararVersoes(versaoDisponivel, FW_VERSION);
  if (comparacao == 0) {
    Serial.println("[OTA] A versao instalada ja e a mais recente. Nada a fazer.");
    return;
  }
  if (comparacao < 0) {
    Serial.println("[OTA] O repositorio tem uma versao mais antiga. Atualizacao ignorada.");
    return;
  }

  Serial.println("[OTA] Nova versao disponivel! Iniciando atualizacao...");
  if (!baixarEAtualizar(urlFirmware)) {
    Serial.println("[OTA] Atualizacao falhou. O Firmware atual continua em execucao.");
    Serial.println("[OTA] Nova tentativa na proxima sessao.");
    sinalizarErro();
  }
}


int gerarLeitura() {
  return random(ALTURA_MIN_CM, ALTURA_MAX_CM + 1); 
}

float calcularMedia(const int valores[], int n) {
  long soma = 0;
  for (int i = 0; i < n; i++) soma += valores[i];
  return (float)soma / n;
}


void copiarVetor(const int origem[], int destino[], int n) {
  for (int i = 0; i < n; i++) destino[i] = origem[i];
}

void ordenarCrescente(int v[], int n) {
  for (int i = 1; i < n; i++) {
    int atual = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > atual) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = atual;
  }
}

int calcularMediana(const int ordenados[], int n) {
  return ordenados[n / 2];
}

void imprimirVetor(const char* rotulo, const int v[], int n) {
  Serial.print(rotulo);
  for (int i = 0; i < n; i++) {
    Serial.print(v[i]);
    Serial.print(i < n - 1 ? " " : "\n");
  }
}

const char* nomeEstado(EstadoSistema e) {
  return (e == ALERTA) ? "ALERTA" : "NORMAL";
}

void aplicarHisterese(int mediana) {
  EstadoSistema anterior = estadoAtual;

  if (mediana >= LIMITE_ALERTA_CM) {
    estadoAtual = ALERTA;
    Serial.printf("Estado: ALERTA (mediana %d >= %d cm)", mediana, LIMITE_ALERTA_CM);
  } else if (mediana <= LIMITE_NORMAL_CM) {
    estadoAtual = NORMAL;
    Serial.printf("Estado: NORMAL (mediana %d <= %d cm)", mediana, LIMITE_NORMAL_CM);
  } else {
    Serial.printf("Estado: %s MANTIDO (mediana %d na faixa de tolerancia %d < m < %d cm)",
                  nomeEstado(estadoAtual), mediana, LIMITE_NORMAL_CM, LIMITE_ALERTA_CM);
  }

  if (estadoAtual != anterior) {
    Serial.printf("  <- mudou de %s para %s\n", nomeEstado(anterior), nomeEstado(estadoAtual));
  } else {
    Serial.println();
  }

  ledEstado();
  Serial.printf("LED: %s\n", estadoAtual == ALERTA ? "vermelho" : "verde");
}

void imprimirCabecalho() {
  Serial.println();
  Serial.println("========================================");
  Serial.printf("MONITORAMENTO DE VEGETACAO - FW %s\n", FW_VERSION);
  Serial.println("========================================");
}

void iniciarSessao() {
  numeroSessao++;
  leiturasFeitas = 0;
  sessaoEmAndamento = true;
  imprimirCabecalho();
  Serial.printf("Sessao #%u | inicio em t = %.1f s\n", numeroSessao, inicioSessao / 1000.0);
}

void realizarLeitura() {
  int valor = gerarLeitura();
  leituras[leiturasFeitas] = valor;
  leiturasFeitas++;
  Serial.printf("Leitura %d: %d cm   [+%.0f s]\n",
                leiturasFeitas, valor, (millis() - inicioSessao) / 1000.0);
}

void finalizarSessao() {
  sessaoEmAndamento = false;

  int ordenadas[NUM_LEITURAS];
  copiarVetor(leituras, ordenadas, NUM_LEITURAS); 
  ordenarCrescente(ordenadas, NUM_LEITURAS);

  float media = calcularMedia(leituras, NUM_LEITURAS);
  int mediana = calcularMediana(ordenadas, NUM_LEITURAS);

  Serial.println("----------------------------------------");
  imprimirVetor("Ordem original:  ", leituras, NUM_LEITURAS);
  imprimirVetor("Ordem crescente: ", ordenadas, NUM_LEITURAS);
  Serial.printf("Media da sessao:   %.1f cm\n", media);
  Serial.printf("Mediana da sessao: %d cm\n", mediana);
  aplicarHisterese(mediana);
  Serial.println("----------------------------------------");
  Serial.printf("Proxima sessao em 48 segundos (t = %.1f s).\n",
                (inicioSessao + INTERVALO_SESSAO_MS) / 1000.0);

  if (numeroSessao >= SESSOES_ANTES_DA_OTA) {
    verificarAtualizacao();
  } else {
    Serial.printf("[OTA] Verificacao de atualizacao apos %u sessoes (%u/%u).\n",
                  SESSOES_ANTES_DA_OTA, numeroSessao, SESSOES_ANTES_DA_OTA);
  }
}

void gerenciarSessoes() {
  unsigned long agora = millis();

  if (!sessaoEmAndamento) {
    bool primeira = (numeroSessao == 0);
    if (primeira || agora - inicioSessao >= INTERVALO_SESSAO_MS) {
      if (primeira) {
        inicioSessao = agora;
      } else {
        inicioSessao += INTERVALO_SESSAO_MS;               
        if (agora - inicioSessao >= INTERVALO_SESSAO_MS) { 
          inicioSessao = agora;
        }
      }
      iniciarSessao();
    }
    return;
  }

  if (agora - inicioSessao >= (unsigned long)leiturasFeitas * INTERVALO_LEITURA_MS) {
    realizarLeitura();
    if (leiturasFeitas == NUM_LEITURAS) {
      finalizarSessao();
    }
  }
}


void imprimirInfoBoot() {
  const esp_partition_t* particao = esp_ota_get_running_partition();

  Serial.println();
  Serial.println("########################################");
  Serial.printf("  BOOT - FIRMWARE %s EM EXECUCAO\n", FW_VERSION);
  Serial.println("  Novidades: ordenacao, mediana e histerese");
  Serial.println("  LED verde = NORMAL | LED vermelho = ALERTA");
  Serial.printf("  Estado inicial: %s\n", nomeEstado(estadoAtual));
  Serial.printf("  Particao em execucao: %s\n", particao ? particao->label : "?");
  if (esp_reset_reason() == ESP_RST_SW) {
    Serial.println("  Motivo do boot: reinicio por software (esperado apos a OTA)");
  }
  Serial.println("########################################");
}


void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  ledEstado();

  randomSeed(esp_random());
  imprimirInfoBoot();
  conectarWiFi();
}

void loop() {
  gerenciarSessoes();
  delay(5);
}
