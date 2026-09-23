#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

const char *FW_VERSION = "1.0";

const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASS = "";
const int WIFI_CANAL = 6;
const unsigned long WIFI_TIMEOUT_MS = 15000;

const char *MANIFEST_URL =
    "https://raw.githubusercontent.com/Moreettoo/repositorio-firmware/main/version.json";

const int PIN_LED_R = 25;
const int PIN_LED_G = 26;
const int PIN_LED_B = 27;

const int NUM_LEITURAS = 5;
const unsigned long INTERVALO_LEITURA_MS = 2000;
const unsigned long INTERVALO_SESSAO_MS = 48000;
const int ALTURA_MIN_CM = 10;
const int ALTURA_MAX_CM = 20;
const unsigned int SESSOES_ANTES_DA_OTA = 3;
const unsigned long OTA_TIMEOUT_SEM_DADOS_MS = 30000;

int leituras[NUM_LEITURAS];
int leiturasFeitas = 0;
bool sessaoEmAndamento = false;
unsigned long inicioSessao = 0;
unsigned int numeroSessao = 0;

void definirCor(bool r, bool g, bool b)
{
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

void ledVersao()
{
  definirCor(false, false, true);
}

void ledOtaEmAndamento()
{
  definirCor(true, false, true);
}

void sinalizarErro()
{
  for (int i = 0; i < 3; i++)
  {
    definirCor(true, true, false);
    delay(200);
    definirCor(false, false, false);
    delay(200);
  }
  ledVersao();
}

bool conectarWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_TIMEOUT_MS)
  {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("WiFi conectado");
    return true;
  }
  Serial.println("WiFi sem conexao");
  return false;
}

bool garantirWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
    return true;
  WiFi.disconnect();
  return conectarWiFi();
}

String descreverErroHttp(int codigo)
{
  if (codigo < 0)
    return HTTPClient::errorToString(codigo);
  return "HTTP " + String(codigo);
}

bool iniciarHttp(HTTPClient &http, const String &url,
                 WiFiClientSecure &clienteSeguro, WiFiClient &clienteSimples)
{
  if (url.startsWith("https://"))
  {
    clienteSeguro.setInsecure();
    return http.begin(clienteSeguro, url);
  }
  return http.begin(clienteSimples, url);
}

int compararVersoes(const String &a, const String &b)
{
  int ia = 0, ib = 0;
  while (ia < (int)a.length() || ib < (int)b.length())
  {
    long na = 0, nb = 0;
    while (ia < (int)a.length() && a[ia] != '.')
    {
      if (isDigit(a[ia]))
        na = na * 10 + (a[ia] - '0');
      ia++;
    }
    while (ib < (int)b.length() && b[ib] != '.')
    {
      if (isDigit(b[ib]))
        nb = nb * 10 + (b[ib] - '0');
      ib++;
    }
    if (na != nb)
      return (na > nb) ? 1 : -1;
    ia++;
    ib++;
  }
  return 0;
}

bool buscarManifesto(String &versao, String &urlFirmware)
{
  WiFiClientSecure clienteSeguro;
  WiFiClient clienteSimples;
  HTTPClient http;

  String url = String(MANIFEST_URL) + "?nocache=" + String(esp_random());
  if (!iniciarHttp(http, url, clienteSeguro, clienteSimples))
  {
    Serial.println("OTA erro: URL do manifesto invalida");
    return false;
  }
  http.setTimeout(10000);

  int codigo = http.GET();
  if (codigo != HTTP_CODE_OK)
  {
    Serial.printf("OTA erro: manifesto (%s)\n", descreverErroHttp(codigo).c_str());
    http.end();
    return false;
  }

  String corpo = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, corpo))
  {
    Serial.println("OTA erro: manifesto invalido");
    return false;
  }

  versao = String(doc["version"] | "");
  urlFirmware = String(doc["url"] | "");
  versao.trim();
  urlFirmware.trim();

  if (versao.length() == 0 || urlFirmware.length() == 0)
  {
    Serial.println("OTA erro: manifesto invalido");
    return false;
  }
  return true;
}

size_t receberFirmware(WiFiClient *stream, int tamanho)
{
  uint8_t buffer[1024];
  size_t gravados = 0;
  unsigned long ultimoDado = millis();

  while (tamanho <= 0 || gravados < (size_t)tamanho)
  {
    size_t disponivel = stream->available();
    if (disponivel > 0)
    {
      size_t lidos = stream->readBytes(buffer, disponivel < sizeof(buffer) ? disponivel : sizeof(buffer));
      if (lidos > 0)
      {
        if (Update.write(buffer, lidos) != lidos)
          return 0;
        gravados += lidos;
        ultimoDado = millis();
      }
    }
    else if (!stream->connected())
    {
      break;
    }
    else if (millis() - ultimoDado > OTA_TIMEOUT_SEM_DADOS_MS)
    {
      break;
    }
    else
    {
      delay(1);
    }
  }
  return gravados;
}

bool baixarEAtualizar(const String &urlFirmware)
{
  WiFiClientSecure clienteSeguro;
  WiFiClient clienteSimples;
  HTTPClient http;

  ledOtaEmAndamento();

  if (!iniciarHttp(http, urlFirmware, clienteSeguro, clienteSimples))
  {
    Serial.println("OTA erro: URL do firmware invalida");
    return false;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int codigo = http.GET();
  if (codigo != HTTP_CODE_OK)
  {
    Serial.printf("OTA erro: download (%s)\n", descreverErroHttp(codigo).c_str());
    http.end();
    return false;
  }

  int tamanho = http.getSize();
  if (!Update.begin(tamanho > 0 ? (size_t)tamanho : UPDATE_SIZE_UNKNOWN))
  {
    Serial.printf("OTA erro: gravacao (%s)\n", Update.errorString());
    http.end();
    return false;
  }

  size_t gravados = receberFirmware(http.getStreamPtr(), tamanho);
  if (gravados == 0 && Update.hasError())
  {
    Serial.printf("OTA erro: gravacao (%s)\n", Update.errorString());
    Update.abort();
    http.end();
    return false;
  }

  if (tamanho > 0 && gravados != (size_t)tamanho)
  {
    Serial.println("OTA erro: download incompleto");
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end(tamanho <= 0))
  {
    Serial.printf("OTA erro: gravacao (%s)\n", Update.errorString());
    http.end();
    return false;
  }
  http.end();

  Serial.println("OTA concluida, reiniciando");
  delay(2000);
  ESP.restart();
  return true;
}

void verificarAtualizacao()
{
  if (!garantirWiFi())
  {
    Serial.println("OTA erro: sem WiFi");
    sinalizarErro();
    return;
  }

  String versaoDisponivel, urlFirmware;
  if (!buscarManifesto(versaoDisponivel, urlFirmware))
  {
    sinalizarErro();
    return;
  }

  int comparacao = compararVersoes(versaoDisponivel, FW_VERSION);
  if (comparacao == 0)
  {
    Serial.println("OTA: ja esta na versao mais recente");
    return;
  }
  if (comparacao < 0)
  {
    Serial.println("OTA: versao do repositorio e mais antiga");
    return;
  }

  Serial.printf("OTA: nova versao %s, atualizando\n", versaoDisponivel.c_str());
  if (!baixarEAtualizar(urlFirmware))
  {
    Serial.printf("OTA falhou, mantendo FW %s\n", FW_VERSION);
    sinalizarErro();
  }
}

int gerarLeitura()
{
  return random(ALTURA_MIN_CM, ALTURA_MAX_CM + 1);
}

float calcularMedia(const int valores[], int n)
{
  long soma = 0;
  for (int i = 0; i < n; i++)
    soma += valores[i];
  return (float)soma / n;
}

void iniciarSessao()
{
  numeroSessao++;
  leiturasFeitas = 0;
  sessaoEmAndamento = true;
  Serial.printf("\nSessao %u - FW %s (t=%.1fs)\n", numeroSessao, FW_VERSION, inicioSessao / 1000.0);
}

void realizarLeitura()
{
  int valor = gerarLeitura();
  leituras[leiturasFeitas] = valor;
  leiturasFeitas++;
  Serial.printf("Leitura %d: %d cm\n", leiturasFeitas, valor);
}

void finalizarSessao()
{
  sessaoEmAndamento = false;

  float media = calcularMedia(leituras, NUM_LEITURAS);
  Serial.printf("Media: %.1f cm\n", media);

  if (numeroSessao >= SESSOES_ANTES_DA_OTA)
  {
    verificarAtualizacao();
  }
}

void gerenciarSessoes()
{
  unsigned long agora = millis();

  if (!sessaoEmAndamento)
  {
    bool primeira = (numeroSessao == 0);
    if (primeira || agora - inicioSessao >= INTERVALO_SESSAO_MS)
    {
      if (primeira)
      {
        inicioSessao = agora;
      }
      else
      {
        inicioSessao += INTERVALO_SESSAO_MS;
        if (agora - inicioSessao >= INTERVALO_SESSAO_MS)
        {
          inicioSessao = agora;
        }
      }
      iniciarSessao();
    }
    return;
  }

  if (agora - inicioSessao >= (unsigned long)leiturasFeitas * INTERVALO_LEITURA_MS)
  {
    realizarLeitura();
    if (leiturasFeitas == NUM_LEITURAS)
    {
      finalizarSessao();
    }
  }
}

void imprimirInfoBoot()
{
  const esp_partition_t *particao = esp_ota_get_running_partition();
  Serial.printf("\nFW %s | particao %s\n", FW_VERSION, particao ? particao->label : "?");
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  ledVersao();

  randomSeed(esp_random());
  imprimirInfoBoot();
  conectarWiFi();
}

void loop()
{
  gerenciarSessoes();
  delay(5);
}
