#include <Arduino.h>
#include <DHT.h>

// Defina como 1 para testar os componentes pelo Monitor Serial.
#define MODO_TESTE_COMPONENTES 0

// Etapa 1: Cloud opcional. Etapa 2: habilite também a API.
// Preencha as credenciais apenas em uma cópia privada deste arquivo.
#define USAR_ARDUINO_CLOUD 0
#define USAR_API 0

#define WIFI_SSID ""
#define WIFI_SENHA ""
#define CLOUD_DEVICE_ID ""
#define CLOUD_DEVICE_KEY ""

#define API_URL "http://192.168.1.100:8000/api/v1/leituras"
#define API_TOKEN ""
// Para HTTPS, informe o certificado PEM da CA que assinou o servidor.
#define API_CA_PEM ""

#if !MODO_TESTE_COMPONENTES && (USAR_ARDUINO_CLOUD || USAR_API)
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

#if !MODO_TESTE_COMPONENTES && USAR_API
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#endif

#if !MODO_TESTE_COMPONENTES && USAR_ARDUINO_CLOUD
#include <ArduinoIoTCloud.h>
#include <Arduino_ConnectionHandler.h>
#endif

// Pinagem e calibração.
constexpr int PINO_LDR = 4;
constexpr int PINO_POT = 5;
constexpr int PINO_TANQUE = 6;  // Entrada para sensor analógico de nível, se instalado.
constexpr int PINO_DHT = 7;
constexpr int PINO_BOTAO = 8;
constexpr int PINOS_RGB[] = {9, 10, 11};
constexpr int PINO_BUZZER = 12;
constexpr bool RGB_ANODO_COMUM = false;  // true: terminal comum ligado a 3V3.
constexpr bool BUZZER_PASSIVO = false;  // true: buzzer acionado por tom PWM.
constexpr uint32_t FREQUENCIA_BUZZER_HZ = 2400;

constexpr int LIMIAR_LUZ = 1700;  // Escala ADC de 0 a 4095.
constexpr int HISTERESE_LUZ = 80;
constexpr float TEMPERATURA_MIN = 0.0f;
constexpr float TEMPERATURA_MAX = 25.0f;
constexpr bool USAR_TANQUE = false;  // Com false, deixe o GPIO 6 livre.
constexpr int ADC_TANQUE_VAZIO = 300;  // Substitua pelas medições do sensor.
constexpr int ADC_TANQUE_CHEIO = 3200;
static_assert(!USAR_TANQUE || ADC_TANQUE_CHEIO != ADC_TANQUE_VAZIO,
              "Os pontos de calibração do tanque precisam ser diferentes.");

constexpr char ID_DISPOSITIVO[] = "lampada-s3-01";

DHT dht(PINO_DHT, DHT11);
void setup();
void loop();
void iniciarSaidas();
void escreverCanalRgb(int canal, uint8_t intensidade);
void escreverBuzzer(bool ligado, bool comTom);

bool rgbPronto[3] = {};
bool buzzerPronto = false;

void escreverCanalRgb(int canal, uint8_t intensidade) {
  if (!rgbPronto[canal]) return;
  ledcWrite(PINOS_RGB[canal], RGB_ANODO_COMUM ? 255 - intensidade : intensidade);
}

void escreverBuzzer(bool ligado, bool comTom) {
  if (!buzzerPronto) return;
  const uint8_t intensidade = comTom ? 128 : 255;
  ledcWrite(PINO_BUZZER, ligado ? intensidade : 0);
}

void iniciarSaidas() {
  for (int canal = 0; canal < 3; ++canal) {
    const int pino = PINOS_RGB[canal];
    pinMode(pino, OUTPUT);
    digitalWrite(pino, RGB_ANODO_COMUM ? HIGH : LOW);
    rgbPronto[canal] = ledcAttach(pino, 5000, 8);
    if (!rgbPronto[canal]) Serial.printf("Falha no PWM do RGB no GPIO %d\n", pino);
    escreverCanalRgb(canal, 0);
  }
  pinMode(PINO_BUZZER, OUTPUT);
  digitalWrite(PINO_BUZZER, LOW);
  buzzerPronto = ledcAttach(PINO_BUZZER, FREQUENCIA_BUZZER_HZ, 8);
  if (!buzzerPronto) Serial.printf("Falha no PWM do buzzer no GPIO %d\n", PINO_BUZZER);
  escreverBuzzer(false, false);
}

#if MODO_TESTE_COMPONENTES

// Teste de bancada. Após validar a montagem, volte MODO_TESTE_COMPONENTES para 0.
uint32_t ultimaLeitura = 0;
uint32_t inicioBipe = 0;
bool buzzerLigado = false;

void setup() {
  Serial.begin(115200);
  dht.begin();
  analogReadResolution(12);
  analogSetPinAttenuation(PINO_LDR, ADC_11db);
  analogSetPinAttenuation(PINO_POT, ADC_11db);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  iniciarSaidas();
  Serial.println("Teste de bancada: r/g/b = RGB, 0 = apagar, p = buzzer ativo, t = buzzer passivo.");
}

void loop() {
  const uint32_t agora = millis();
  while (Serial.available()) {
    const int comando = Serial.read();
    switch (comando) {
      case 'r': case 'g': case 'b': case '0': {
        constexpr char CANAIS[] = "rgb";
        for (int canal = 0; canal < 3; ++canal) {
          escreverCanalRgb(canal, comando == CANAIS[canal] ? 255 : 0);
        }
        Serial.printf("Comando RGB recebido: %c\n", comando);
        break;
      }
      case 'p': case 't':
        inicioBipe = agora;
        buzzerLigado = true;
        escreverBuzzer(true, comando == 't');
        Serial.println(comando == 't' ? "Teste do buzzer: tom PWM" : "Teste do buzzer: nível alto");
        break;
    }
  }
  if (buzzerLigado && agora - inicioBipe >= 500) {
    escreverBuzzer(false, false);
    buzzerLigado = false;
  }
  if (agora - ultimaLeitura >= 2500) {
    ultimaLeitura = agora;
    Serial.printf("LDR: %d | Pot: %d | Botão: %s | DHT11: %.1f °C / %.1f%%\n",
                  analogRead(PINO_LDR), analogRead(PINO_POT),
                  digitalRead(PINO_BOTAO) == LOW ? "pressionado" : "solto",
                  dht.readTemperature(), dht.readHumidity());
  }
  delay(10);
}

#else

// Dados compartilhados entre a montagem, a nuvem e a API.
enum class Cor : uint8_t { Apagado, Vermelho, Laranja, Amarelo, Branco, Turquesa, Azul };
constexpr const char *NOMES_CORES[] = {
  "Apagado", "Vermelho", "Laranja", "Amarelo", "Branco", "Turquesa", "Azul"
};

inline const char *nomeCor(Cor cor);

inline const char *nomeCor(Cor cor) {
  return NOMES_CORES[static_cast<uint8_t>(cor)];
}

enum class Comando : uint8_t {
  Ligar, Desligar, Automatico, Vermelho, Amarelo, Azul,
  AtivarTemperatura, DesativarTemperatura, AtivarDetector, DesativarDetector,
  AtivarBuzzer, DesativarBuzzer
};

struct Leitura {
  float temperatura = NAN;
  float umidade = NAN;
  float nivelTanque = NAN;
  int luminosidade = -1;
  int potenciometro = 0;
  int brilho = 0;
  Cor cor = Cor::Apagado;
  bool botaoPressionado = false;
  bool sistemaLigado = true;
  bool ledLigado = false;
  bool buzzerLigado = false;
  bool temperaturaAtiva = true;
  bool detectorAtivo = true;
  bool buzzerAtivo = true;
  bool alertaTemperatura = false;
};

// Declarações para o pré-processador de sketches da Arduino IDE.
void iniciarRede();
void publicarLeitura(const Leitura &leitura);
bool receberComando(Comando &valor);
void ARDUINO_ISR_ATTR interrupcaoBotao();
bool atualizarBotao(uint32_t agora);
void aplicarComando(Comando comando, uint32_t agora);
int lerAdc(int pino);
void atualizarSensores(uint32_t agora);
Cor corDoPotenciometro(int adc);
void atualizarSaidas(uint32_t agora);
void imprimirEstado();

// Integração com o InfluxDB e o Arduino Cloud.
#if USAR_ARDUINO_CLOUD || USAR_API
static QueueHandle_t filaLeitura = nullptr;
#endif

#if USAR_API

constexpr uint32_t INTERVALO_API_MS = 15000;
constexpr uint32_t INTERVALO_REENVIO_MS = 4000;
constexpr time_t INICIO_2024 = 1704067200;

struct Confirmacao {
  float temperatura;
  float umidade;
  int luminosidade;
  uint32_t recebidoEm;
};
static QueueHandle_t filaConfirmacao = nullptr;

static int enviarAoBanco(const Leitura &leitura, time_t timestamp, Confirmacao &confirmacao);
static void tarefaApi(void *);

static int enviarAoBanco(const Leitura &leitura, time_t timestamp, Confirmacao &confirmacao) {
  JsonDocument documento;
  documento["dispositivo"] = ID_DISPOSITIVO;
  documento["timestamp"] = static_cast<int64_t>(timestamp);
  // ArduinoJson serializa NAN como null na configuração padrão.
  documento["temperatura_c"] = leitura.temperatura;
  documento["umidade_pct"] = leitura.umidade;
  documento["nivel_tanque_pct"] = leitura.nivelTanque;
  if (leitura.luminosidade >= 0) documento["luminosidade_adc"] = leitura.luminosidade;
  else documento["luminosidade_adc"] = nullptr;
  documento["potenciometro_adc"] = leitura.potenciometro;
  documento["brilho_pct"] = leitura.brilho;
  documento["cor"] = nomeCor(leitura.cor);
  documento["botao_pressionado"] = leitura.botaoPressionado;
  documento["sistema_ligado"] = leitura.sistemaLigado;
  documento["led_ligado"] = leitura.ledLigado;
  documento["buzzer_ligado"] = leitura.buzzerLigado;
  documento["temperatura_ativa"] = leitura.temperaturaAtiva;
  documento["detector_ativo"] = leitura.detectorAtivo;
  documento["buzzer_ativo"] = leitura.buzzerAtivo;
  documento["alerta_temperatura"] = leitura.alertaTemperatura;
  String corpo;
  serializeJson(documento, corpo);

  const String url = API_URL;
  WiFiClient cliente;
  WiFiClientSecure clienteSeguro;
  HTTPClient http;
  if (url.startsWith("https://")) {
    clienteSeguro.setCACert(API_CA_PEM);
    if (!http.begin(clienteSeguro, url)) return -1;
  } else if (!http.begin(cliente, url)) {
    return -1;
  }
  http.setConnectTimeout(2500);
  http.setTimeout(4500);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);

  const int status = http.POST(corpo);
  if (status != 201) return status;

  JsonDocument resposta;
  if (deserializeJson(resposta, http.getString()) ||
      resposta["timestamp"].as<int64_t>() != static_cast<int64_t>(timestamp) ||
      strcmp(resposta["dispositivo"] | "", ID_DISPOSITIVO) != 0) {
    return -1;
  }
  confirmacao = {
    resposta["temperatura_c"] | NAN, resposta["umidade_pct"] | NAN,
    resposta["luminosidade_adc"] | -1, millis()
  };
  return status;
}

static void tarefaApi(void *) {
#if !USAR_ARDUINO_CLOUD
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  bool iniciouNtp = false;
#endif
  uint32_t ultimoEnvio = millis() - INTERVALO_API_MS;
  while (true) {
    const uint32_t agora = millis();
    if (WiFi.status() == WL_CONNECTED) {
#if !USAR_ARDUINO_CLOUD
      if (!iniciouNtp) {
        configTime(0, 0, "pool.ntp.org", "time.arduino.cc");
        iniciouNtp = true;
      }
#endif
      const time_t timestamp = time(nullptr);
      Leitura leitura;
      if (timestamp >= INICIO_2024 && agora - ultimoEnvio >= INTERVALO_API_MS &&
          xQueuePeek(filaLeitura, &leitura, 0) == pdTRUE) {
        ultimoEnvio = agora;
        for (int tentativa = 0; tentativa < 2; ++tentativa) {
          Confirmacao confirmacao;
          const int status = enviarAoBanco(leitura, timestamp, confirmacao);
          if (status == 201) {
            xQueueOverwrite(filaConfirmacao, &confirmacao);
            Serial.printf("InfluxDB: leitura %lld gravada e consultada\n", static_cast<long long>(timestamp));
            break;
          }
          Serial.printf("API: falha no envio (HTTP %d)\n", status);
          if ((status >= 400 && status < 500) || tentativa == 1) break;
          vTaskDelay(pdMS_TO_TICKS(INTERVALO_REENVIO_MS));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
#endif

#if USAR_ARDUINO_CLOUD

static QueueHandle_t filaComandos = nullptr;
static WiFiConnectionHandler conexaoCloud(WIFI_SSID, WIFI_SENHA);
static float temperatura = -99;
static float umidade = -1;
static int luminosidade = -1;
static String estado = "Aguardando primeira leitura";
static String comando;
static bool sincronizado = false;

static void receberComandoCloud();
static void aoConectarCloud();
static void aoSincronizarCloud();
static void tarefaCloud(void *);

static void receberComandoCloud() {
  if (!sincronizado || comando.isEmpty()) return;
  String texto = comando;
  comando = "";
  texto.trim();
  texto.toLowerCase();

  constexpr struct {
    const char *texto;
    Comando valor;
  } comandos[] = {
    {"ligar", Comando::Ligar}, {"desligar", Comando::Desligar},
    {"automático", Comando::Automatico}, {"automatico", Comando::Automatico},
    {"vermelho", Comando::Vermelho}, {"amarelo", Comando::Amarelo}, {"azul", Comando::Azul},
    {"ativar temperatura", Comando::AtivarTemperatura},
    {"desativar temperatura", Comando::DesativarTemperatura},
    {"ativar detector", Comando::AtivarDetector}, {"desativar detector", Comando::DesativarDetector},
    {"ativar buzzer", Comando::AtivarBuzzer}, {"desativar buzzer", Comando::DesativarBuzzer}
  };
  for (const auto &item : comandos) {
    if (texto != item.texto) continue;
    if (xQueueSend(filaComandos, &item.valor, 0) == pdTRUE) Serial.printf("Comando: %s\n", item.texto);
    else Serial.println("Cloud: fila de comandos cheia");
    return;
  }
  Serial.println("Cloud: comando desconhecido");
}

static void aoConectarCloud() {
  sincronizado = false;
}

static void aoSincronizarCloud() {
  comando = "";  // Descarta o comando antigo restaurado na sincronização.
  sincronizado = true;
}

static void tarefaCloud(void *) {
  ArduinoCloud.setBoardId(CLOUD_DEVICE_ID);
  ArduinoCloud.setSecretDeviceKey(CLOUD_DEVICE_KEY);
  auto &propTemperatura = ArduinoCloud.addProperty(temperatura, Permission::Read).publishOnDemand();
  auto &propUmidade = ArduinoCloud.addProperty(umidade, Permission::Read).publishOnDemand();
  auto &propLuminosidade = ArduinoCloud.addProperty(luminosidade, Permission::Read).publishOnDemand();
  ArduinoCloud.addProperty(estado, Permission::Read).publishOnChange(0, 100);
  ArduinoCloud.addProperty(comando, Permission::ReadWrite).onUpdate(receberComandoCloud);
  ArduinoCloud.addCallback(ArduinoIoTCloudEvent::CONNECT, aoConectarCloud);
  ArduinoCloud.addCallback(ArduinoIoTCloudEvent::DISCONNECT, aoConectarCloud);
  ArduinoCloud.addCallback(ArduinoIoTCloudEvent::SYNC, aoSincronizarCloud);
  ArduinoCloud.begin(conexaoCloud, false);
  setDebugMessageLevel(1);

  uint32_t ultimaPublicacao = millis() - 15000;
  while (true) {
    Leitura leitura;
    if (xQueuePeek(filaLeitura, &leitura, 0) == pdTRUE) {
      const uint32_t agora = millis();
      const char *origem = "Ao vivo";
      float temperaturaPublicada = leitura.temperatura;
      float umidadePublicada = leitura.umidade;
      int luzPublicada = leitura.luminosidade;
      bool atualizarNumeros = agora - ultimaPublicacao >= 15000;
#if USAR_API
      Confirmacao confirmacao;
      const bool recebeu = filaConfirmacao && xQueuePeek(filaConfirmacao, &confirmacao, 0) == pdTRUE;
      const bool recente = recebeu && agora - confirmacao.recebidoEm < 3 * INTERVALO_API_MS;
      origem = recente ? "Banco: sincronizado" : recebeu ? "Banco: atrasado" : "Banco: aguardando";
      temperaturaPublicada = recente ? confirmacao.temperatura : NAN;
      umidadePublicada = recente ? confirmacao.umidade : NAN;
      luzPublicada = recente ? confirmacao.luminosidade : -1;
      atualizarNumeros = recente && ultimaPublicacao != confirmacao.recebidoEm;
      if (atualizarNumeros) ultimaPublicacao = confirmacao.recebidoEm;
#else
      if (atualizarNumeros) ultimaPublicacao = agora;
#endif
      if (atualizarNumeros) {
        if (isfinite(temperaturaPublicada)) {
          temperatura = temperaturaPublicada;
          propTemperatura.requestUpdate();
        }
        if (isfinite(umidadePublicada)) {
          umidade = umidadePublicada;
          propUmidade.requestUpdate();
        }
        if (luzPublicada >= 0) {
          luminosidade = luzPublicada;
          propLuminosidade.requestUpdate();
        }
      }

      char mensagem[220];
      snprintf(mensagem, sizeof(mensagem), "Cor: %s | Botão: %s | Sistema: %s | Buzzer: %s\n"
               "%s | Sem leitura: %s%s%s%s%s",
               nomeCor(leitura.cor), leitura.botaoPressionado ? "pressionado" : "solto",
               leitura.sistemaLigado ? "ON" : "OFF", leitura.buzzerLigado ? "ON" : "OFF",
               origem, isfinite(temperaturaPublicada) ? "" : "temp ",
               isfinite(umidadePublicada) ? "" : "umidade ", luzPublicada >= 0 ? "" : "luz ",
               isfinite(temperaturaPublicada) && isfinite(umidadePublicada) && luzPublicada >= 0 ? "nenhuma" : "",
               leitura.alertaTemperatura ? " | Perigo! Desligar!" : "");
      estado = mensagem;
    }
    ArduinoCloud.update();
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}
#endif

void iniciarRede() {
#if USAR_ARDUINO_CLOUD || USAR_API
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("Rede: configure WIFI_SSID no início do sketch");
    return;
  }
#if USAR_ARDUINO_CLOUD
  if (strlen(CLOUD_DEVICE_ID) == 0 || strlen(CLOUD_DEVICE_KEY) == 0) {
    Serial.println("Cloud: configure o ID e a chave do dispositivo no início do sketch");
    return;
  }
#endif
  filaLeitura = xQueueCreate(1, sizeof(Leitura));
  if (!filaLeitura) {
    Serial.println("Rede: falha ao alocar a fila de leituras");
    return;
  }
#if USAR_API
  const String url = API_URL;
  if (strlen(API_TOKEN) < 32 || (!url.startsWith("http://") && !url.startsWith("https://")) ||
      (url.startsWith("https://") && strlen(API_CA_PEM) == 0)) {
    Serial.println("API: configure a URL, o token e, para HTTPS, a CA no início do sketch");
  } else {
    filaConfirmacao = xQueueCreate(1, sizeof(Confirmacao));
  }
#endif
#if USAR_ARDUINO_CLOUD
  filaComandos = xQueueCreate(8, sizeof(Comando));
  if (!filaComandos || xTaskCreate(tarefaCloud, "ArduinoCloud", 12288, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("Cloud: falha ao iniciar a tarefa");
  }
#endif
#if USAR_API
  if (filaConfirmacao && xTaskCreate(tarefaApi, "Telemetria", 12288, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("API: falha ao iniciar a tarefa");
  }
#endif
#else
  Serial.println("Modo local. Configure no início do sketch para ativar a rede.");
#endif
}

void publicarLeitura(const Leitura &leitura) {
#if USAR_ARDUINO_CLOUD || USAR_API
  if (filaLeitura) xQueueOverwrite(filaLeitura, &leitura);
#endif
}

bool receberComando(Comando &valor) {
#if USAR_ARDUINO_CLOUD
  return filaComandos && xQueueReceive(filaComandos, &valor, 0) == pdTRUE;
#else
  return false;
#endif
}

// Controle local dos componentes.
enum class ModoLuz { Automatico, Ligado, Desligado };

constexpr uint32_t INTERVALO_DHT_MS = 2500;
constexpr uint32_t INTERVALO_ADC_MS = 80;
constexpr uint32_t DEBOUNCE_MS = 40;
constexpr uint32_t DURACAO_COR_MS = 1000;
constexpr uint32_t INTERVALO_SERIAL_MS = 2500;

constexpr uint8_t PALETA[][3] = {
  {0, 0, 0}, {255, 0, 0}, {255, 70, 0}, {255, 185, 0},
  {255, 235, 205}, {0, 195, 255}, {0, 0, 255}
};

Leitura leitura;
ModoLuz modoLuz = ModoLuz::Automatico;
Cor corTemporaria = Cor::Apagado;
uint32_t inicioCor = 0;
uint32_t ultimoDht = 0;
uint32_t ultimoAdc = 0;
uint32_t ultimoSerial = 0;
bool ambienteEscuro = false;
bool ldrInicializado = false;

portMUX_TYPE travaBotao = portMUX_INITIALIZER_UNLOCKED;
volatile bool houveBorda = false;
bool debouncePendente = false;
uint32_t ultimaBorda = 0;

void ARDUINO_ISR_ATTR interrupcaoBotao() {
  portENTER_CRITICAL_ISR(&travaBotao);
  houveBorda = true;
  portEXIT_CRITICAL_ISR(&travaBotao);
}

bool atualizarBotao(uint32_t agora) {
  portENTER_CRITICAL(&travaBotao);
  const bool borda = houveBorda;
  houveBorda = false;
  portEXIT_CRITICAL(&travaBotao);
  if (borda) {
    debouncePendente = true;
    ultimaBorda = agora;
  }
  if (!debouncePendente || agora - ultimaBorda < DEBOUNCE_MS) return false;

  debouncePendente = false;
  const bool pressionado = digitalRead(PINO_BOTAO) == LOW;
  if (pressionado == leitura.botaoPressionado) return false;

  leitura.botaoPressionado = pressionado;
  if (pressionado) {
    leitura.sistemaLigado = !leitura.sistemaLigado;
    corTemporaria = Cor::Apagado;
  }
  return true;
}

void aplicarComando(Comando comando, uint32_t agora) {
  switch (comando) {
    case Comando::Ligar:
      modoLuz = ModoLuz::Ligado;
      break;
    case Comando::Desligar:
      modoLuz = ModoLuz::Desligado;
      corTemporaria = Cor::Apagado;
      break;
    case Comando::Automatico:
      modoLuz = ModoLuz::Automatico;
      break;
    case Comando::Vermelho:
    case Comando::Amarelo:
    case Comando::Azul:
      corTemporaria = comando == Comando::Vermelho ? Cor::Vermelho :
                      comando == Comando::Amarelo ? Cor::Amarelo : Cor::Azul;
      inicioCor = agora;
      break;
    case Comando::AtivarTemperatura:
      leitura.temperaturaAtiva = true;
      break;
    case Comando::DesativarTemperatura:
      leitura.temperaturaAtiva = false;
      leitura.temperatura = NAN;
      leitura.umidade = NAN;
      break;
    case Comando::AtivarDetector:
      if (!leitura.detectorAtivo) ldrInicializado = false;
      leitura.detectorAtivo = true;
      break;
    case Comando::DesativarDetector:
      leitura.detectorAtivo = false;
      leitura.luminosidade = -1;
      break;
    case Comando::AtivarBuzzer:
      leitura.buzzerAtivo = true;
      break;
    case Comando::DesativarBuzzer:
      leitura.buzzerAtivo = false;
      break;
  }
}

int lerAdc(int pino) {
  int soma = 0;
  for (int i = 0; i < 8; ++i) soma += analogRead(pino);
  return soma / 8;
}

void atualizarSensores(uint32_t agora) {
  if (leitura.temperaturaAtiva && agora - ultimoDht >= INTERVALO_DHT_MS) {
    ultimoDht = agora;
    const float temperatura = dht.readTemperature();
    const float umidade = dht.readHumidity();
    leitura.temperatura = isfinite(temperatura) && temperatura >= -40 && temperatura <= 80
                         ? temperatura : NAN;
    leitura.umidade = isfinite(umidade) && umidade >= 0 && umidade <= 100 ? umidade : NAN;
  }

  if (agora - ultimoAdc < INTERVALO_ADC_MS) return;
  ultimoAdc = agora;
  leitura.potenciometro = lerAdc(PINO_POT);
  if (leitura.detectorAtivo) {
    leitura.luminosidade = lerAdc(PINO_LDR);
    const int limiar = !ldrInicializado ? LIMIAR_LUZ :
                       ambienteEscuro ? LIMIAR_LUZ + HISTERESE_LUZ : LIMIAR_LUZ - HISTERESE_LUZ;
    ambienteEscuro = leitura.luminosidade < limiar;
    ldrInicializado = true;
  }
  if (USAR_TANQUE) {
    const int adc = lerAdc(PINO_TANQUE);
    const float porcentagem = 100.0f * (adc - ADC_TANQUE_VAZIO) / (ADC_TANQUE_CHEIO - ADC_TANQUE_VAZIO);
    leitura.nivelTanque = constrain(porcentagem, 0.0f, 100.0f);
  }
}

Cor corDoPotenciometro(int adc) {
  constexpr int LIMITES[] = {700, 1300, 2300, 2900, 3450};
  for (uint8_t i = 0; i < sizeof(LIMITES) / sizeof(LIMITES[0]); ++i) {
    if (adc < LIMITES[i]) return static_cast<Cor>(i + 1);
  }
  return Cor::Azul;
}

void atualizarSaidas(uint32_t agora) {
  if (agora - inicioCor >= DURACAO_COR_MS) corTemporaria = Cor::Apagado;

  const bool temperaturaValida = isfinite(leitura.temperatura);
  leitura.alertaTemperatura = leitura.temperaturaAtiva && temperaturaValida &&
    (leitura.temperatura < TEMPERATURA_MIN || leitura.temperatura > TEMPERATURA_MAX);
  const bool temperaturaPermite = !leitura.temperaturaAtiva ||
                                  (temperaturaValida && !leitura.alertaTemperatura);
  const bool luzAutomatica = !leitura.detectorAtivo ||
                             (leitura.luminosidade >= 0 && ambienteEscuro);
  const bool modoPermite = modoLuz == ModoLuz::Ligado ||
                          (modoLuz == ModoLuz::Automatico && luzAutomatica);
  leitura.ledLigado = leitura.sistemaLigado && temperaturaPermite && modoPermite;
  leitura.buzzerLigado = leitura.sistemaLigado && leitura.buzzerAtivo && leitura.alertaTemperatura;
  leitura.cor = Cor::Apagado;
  leitura.brilho = 0;
  if (leitura.ledLigado) {
    leitura.cor = corTemporaria == Cor::Apagado ? corDoPotenciometro(leitura.potenciometro) : corTemporaria;
    leitura.brilho = 100;
    if (modoLuz == ModoLuz::Automatico && leitura.detectorAtivo) {
      leitura.brilho = map(constrain(leitura.luminosidade, 0, LIMIAR_LUZ), 0, LIMIAR_LUZ, 100, 35);
    }
  }

  const auto &rgb = PALETA[static_cast<uint8_t>(leitura.cor)];
  for (int canal = 0; canal < 3; ++canal) {
    escreverCanalRgb(canal, rgb[canal] * leitura.brilho / 100);
  }
  escreverBuzzer(leitura.buzzerLigado, BUZZER_PASSIVO);
}

void imprimirEstado() {
  Serial.printf("LED RGB: %s (%s, %d%%) | Temperatura: ",
                leitura.ledLigado ? "ON" : "OFF", nomeCor(leitura.cor), leitura.brilho);
  if (isfinite(leitura.temperatura)) Serial.printf("%.1f °C", leitura.temperatura);
  else Serial.print(leitura.temperaturaAtiva ? "sem leitura" : "desativada");
  if (isfinite(leitura.umidade)) Serial.printf(" | Umidade: %.1f%%", leitura.umidade);
  if (isfinite(leitura.nivelTanque)) Serial.printf(" | Tanque: %.1f%%", leitura.nivelTanque);
  Serial.printf(" | LDR: %d | Pot: %d | Botão: %s | Sistema: %s | Buzzer: %s\n",
                leitura.luminosidade, leitura.potenciometro,
                leitura.botaoPressionado ? "pressionado" : "solto",
                leitura.sistemaLigado ? "ON" : "OFF", leitura.buzzerLigado ? "ON" : "OFF");
  if (!leitura.ledLigado) {
    const char *motivo = "ambiente claro";
    if (!leitura.sistemaLigado) motivo = "sistema desligado pelo botão";
    else if (leitura.alertaTemperatura) motivo = "temperatura fora da faixa";
    else if (leitura.temperaturaAtiva && !isfinite(leitura.temperatura)) motivo = "DHT11 sem leitura válida";
    else if (modoLuz == ModoLuz::Desligado) motivo = "modo remoto Desligar";
    else if (leitura.luminosidade < 0) motivo = "aguardando leitura do LDR";
    Serial.printf("Motivo do RGB OFF: %s\n", motivo);
  }
  if (leitura.alertaTemperatura) Serial.println("Perigo! Desligar!");
}

void setup() {
  Serial.begin(115200);
  iniciarSaidas();
  analogReadResolution(12);
  analogSetPinAttenuation(PINO_LDR, ADC_11db);
  analogSetPinAttenuation(PINO_POT, ADC_11db);
  if (USAR_TANQUE) analogSetPinAttenuation(PINO_TANQUE, ADC_11db);

  dht.begin();
  ultimoDht = millis();  // Aguarde a estabilização do DHT11 após ligar.
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  leitura.botaoPressionado = digitalRead(PINO_BOTAO) == LOW;
  attachInterrupt(digitalPinToInterrupt(PINO_BOTAO), interrupcaoBotao, CHANGE);
  Serial.println("Lâmpada inteligente ESP32-S3 iniciada");
  iniciarRede();
}

void loop() {
  const uint32_t agora = millis();
  const bool mudouBotao = atualizarBotao(agora);
  Comando comando;
  while (receberComando(comando)) aplicarComando(comando, agora);
  atualizarSensores(agora);
  atualizarSaidas(agora);
  publicarLeitura(leitura);

  if (mudouBotao || agora - ultimoSerial >= INTERVALO_SERIAL_MS) {
    ultimoSerial = agora;
    imprimirEstado();
  }
  delay(10);
}

#endif
