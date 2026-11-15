#include <Arduino.h>
#include <DHT.h>

/* ============================================================
   LAMPADA INTELIGENTE - ESP32-WROOM
   Arduino-ESP32 Core 3.x

   Para habilitar o Arduino IoT Cloud:
   1. Instale ArduinoIoTCloud e Arduino_ConnectionHandler.
   2. Altere USAR_ARDUINO_CLOUD para 1.
   3. Preencha Wi-Fi, Device ID e Secret Key.
   4. Crie no Cloud as propriedades com os mesmos nomes/tipos
      indicados na seção de integração abaixo.

   Para habilitar o envio ao Banco de Dados (Etapa 02):
   1. Rode backend/iniciar_banco.bat (ou o compose.yaml).
   2. Instale a biblioteca ArduinoJson 7.x.
   3. Altere USAR_API para 1, ajuste API_URL com o IPv4 do PC
      e cole em API_TOKEN o token gerado pelo backend.
   ATENCAO: apague senha/token antes de enviar ao GitHub.
   ============================================================ */

#define USAR_ARDUINO_CLOUD 1
#define USAR_API 1

// O ESP32 so conecta em rede 2,4 GHz.
#define WIFI_SSID       ""
#define WIFI_SENHA      ""
#define CLOUD_DEVICE_ID ""
#define CLOUD_DEVICE_KEY ""

// IPv4 do computador que roda a API (comando ipconfig), porta 8000.
#define API_URL         "http://IP_DO_PC:8000/api/v1/leituras"
#define API_TOKEN       ""                  // Token da API (config_local.json ou .env)
#define ID_DISPOSITIVO  "lampada-wroom-01"  // Letras, numeros, - e _

#if USAR_ARDUINO_CLOUD || USAR_API
  #include <WiFi.h>
#endif

#if USAR_API
  #include <ArduinoJson.h>
  #include <HTTPClient.h>
  #include <time.h>
#endif

// -------------------- CONFIGURACAO DOS SENSORES --------------------
#define TIPO_DHT DHT22       // Troque para DHT11 se esse for seu sensor.

constexpr int PINO_LDR = 34;       // ADC1: funciona com Wi-Fi
constexpr int PINO_POT = 36;       // ADC1: somente entrada
constexpr int PINO_DHT = 32;
constexpr int PINO_BOTAO = 17;
constexpr int PINO_BUZZER = 14;

// Ordem dos canais: vermelho, verde, azul.
constexpr int PINO_RGB_R = 25;
constexpr int PINO_RGB_G = 26;
constexpr int PINO_RGB_B = 27;
constexpr int PINOS_RGB[] = {PINO_RGB_R, PINO_RGB_G, PINO_RGB_B};

// Ajuste LIMIAR_LUZ conforme os valores observados no Monitor Serial.
// Este código considera: leitura menor = ambiente mais escuro.
constexpr int LIMIAR_LUZ = 4500;
constexpr int HISTERESE_LUZ = 80;

constexpr float TEMPERATURA_MIN = 0.0f;
constexpr float TEMPERATURA_MAX = 28.0f;

// LED RGB de catodo comum: false. Para anodo comum, altere para true.
constexpr bool LED_ANODO_COMUM = false;

// -------------------- INTERVALOS --------------------
constexpr uint32_t INTERVALO_DHT_MS = 2500; // DHT22 exige intervalo adequado
constexpr uint32_t INTERVALO_ADC_MS = 100;
constexpr uint32_t INTERVALO_SERIAL_MS = 2000;
constexpr uint32_t DEBOUNCE_MS = 50;
constexpr uint32_t DURACAO_COR_MS = 1000;

DHT dht(PINO_DHT, TIPO_DHT);

// -------------------- ESTADOS DO SISTEMA --------------------
enum class ModoLuz : uint8_t { Automatico, Ligado, Desligado };
enum class CorFixa : uint8_t { Nenhuma, Vermelho, Amarelo, Azul };

struct RGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

bool sistemaLigado = true;
bool temperaturaAtiva = true;
bool detectorAtivo = true;
bool buzzerAtivo = true;
bool botaoPressionado = false;
bool ambienteEscuro = false;
bool ldrInicializado = false;
bool alertaTemperatura = false;
bool ledLigado = false;
bool buzzerLigado = false;
const char *nomeCorAtual = "Apagado";  // Enviado ao banco

ModoLuz modoLuz = ModoLuz::Automatico;
CorFixa corTemporaria = CorFixa::Nenhuma;
uint32_t inicioCorTemporaria = 0;

float temperaturaC = NAN;
float umidade = NAN;
int valorLDR = -1;
int valorPot = 0;

uint32_t ultimaLeituraDHT = 0;
uint32_t ultimaLeituraADC = 0;
uint32_t ultimaImpressaoSerial = 0;

// Variaveis compartilhadas com a interrupcao do botao.
portMUX_TYPE travaBotao = portMUX_INITIALIZER_UNLOCKED;
volatile bool interrupcaoRecebida = false;
bool debouncePendente = false;
uint32_t instanteBorda = 0;

// -------------------- BANCO DE DADOS: API + INFLUXDB (OPCIONAL) --------------------
// O envio roda em uma tarefa separada do FreeRTOS para o HTTP nao travar
// o botao, os sensores e o LED enquanto espera a resposta da API.
struct Telemetria {
  float temperatura;
  float umidade;
  int luminosidade;
  int potenciometro;
  int brilho;
  const char *cor;
  bool botaoPressionado;
  bool sistemaLigado;
  bool ledLigado;
  bool buzzerLigado;
  bool temperaturaAtiva;
  bool detectorAtivo;
  bool buzzerAtivo;
  bool alertaTemperatura;
};

#if USAR_API
  constexpr uint32_t INTERVALO_API_MS = 15000;
  constexpr uint32_t INTERVALO_REENVIO_MS = 4000;
  constexpr time_t INICIO_2024 = 1704067200;  // Relogio ainda nao sincronizado se for menor

  portMUX_TYPE travaTelemetria = portMUX_INITIALIZER_UNLOCKED;
  Telemetria telemetria;
  bool telemetriaPronta = false;
  volatile int ultimoStatusApi = 0;  // 0 = ainda nao enviou
  volatile uint32_t ultimoSucessoApi = 0;

  int enviarLeitura(const Telemetria &t, time_t timestamp) {
    // A API exige todos estes campos, com estes nomes e tipos.
    JsonDocument doc;
    doc["dispositivo"] = ID_DISPOSITIVO;
    doc["timestamp"] = (int64_t)timestamp;
    if (isfinite(t.temperatura)) doc["temperatura_c"] = t.temperatura; else doc["temperatura_c"] = nullptr;
    if (isfinite(t.umidade)) doc["umidade_pct"] = t.umidade; else doc["umidade_pct"] = nullptr;
    if (t.luminosidade >= 0) doc["luminosidade_adc"] = t.luminosidade; else doc["luminosidade_adc"] = nullptr;
    doc["nivel_tanque_pct"] = nullptr;  // Projeto sem sensor de nivel
    doc["potenciometro_adc"] = t.potenciometro;
    doc["brilho_pct"] = t.brilho;
    doc["cor"] = t.cor;
    doc["botao_pressionado"] = t.botaoPressionado;
    doc["sistema_ligado"] = t.sistemaLigado;
    doc["led_ligado"] = t.ledLigado;
    doc["buzzer_ligado"] = t.buzzerLigado;
    doc["temperatura_ativa"] = t.temperaturaAtiva;
    doc["detector_ativo"] = t.detectorAtivo;
    doc["buzzer_ativo"] = t.buzzerAtivo;
    doc["alerta_temperatura"] = t.alertaTemperatura;

    String corpo;
    serializeJson(doc, corpo);

    WiFiClient cliente;
    HTTPClient http;
    if (!http.begin(cliente, API_URL)) return -1;
    http.setConnectTimeout(2500);
    http.setTimeout(4500);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + API_TOKEN);

    int status = http.POST(corpo);
    if (status != 201) {
      Serial.printf("API: falha HTTP %d %s\n", status, status > 0 ? http.getString().c_str() : "(sem conexao com a API)");
    }
    http.end();
    return status;
  }

  void tarefaApi(void *) {
  #if !USAR_ARDUINO_CLOUD
    // Sem o Cloud, esta tarefa cuida do Wi-Fi. Com o Cloud, o
    // WiFiConnectionHandler conecta e aqui so esperamos a rede.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
  #endif
    bool ntpIniciado = false;
    uint32_t ultimoEnvio = millis() - INTERVALO_API_MS;

    for (;;) {
      if (WiFi.status() == WL_CONNECTED) {
        if (!ntpIniciado) {
          // Timestamp em UTC, exigido pela API.
          configTime(0, 0, "pool.ntp.org", "time.google.com");
          ntpIniciado = true;
          Serial.printf("Wi-Fi conectado. IP do ESP32: %s\n", WiFi.localIP().toString().c_str());
        }

        time_t timestamp = time(nullptr);
        uint32_t agora = millis();
        if (timestamp >= INICIO_2024 && agora - ultimoEnvio >= INTERVALO_API_MS) {
          Telemetria copia;
          bool pronta;
          portENTER_CRITICAL(&travaTelemetria);
          copia = telemetria;
          pronta = telemetriaPronta;
          portEXIT_CRITICAL(&travaTelemetria);

          if (pronta) {
            ultimoEnvio = agora;
            for (int tentativa = 0; tentativa < 2; tentativa++) {
              int status = enviarLeitura(copia, timestamp);
              ultimoStatusApi = status;
              if (status == 201) {
                ultimoSucessoApi = millis();
                Serial.printf("Banco: leitura %lld gravada no InfluxDB\n", (long long)timestamp);
                break;
              }
              // Erro 4xx (token, campos) nao melhora tentando de novo.
              if (status >= 400 && status < 500) break;
              vTaskDelay(pdMS_TO_TICKS(INTERVALO_REENVIO_MS));
            }
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }

  void iniciarApi() {
    if (strlen(WIFI_SSID) == 0 || strlen(API_TOKEN) < 32) {
      Serial.println("API: preencha WIFI_SSID e API_TOKEN (32+ caracteres, igual ao .env).");
      return;
    }
    if (xTaskCreate(tarefaApi, "EnvioAPI", 8192, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("API: falha ao criar a tarefa de envio.");
      return;
    }
    Serial.printf("API: enviando leituras a cada %lu s para %s\n",
                  (unsigned long)(INTERVALO_API_MS / 1000), API_URL);
  }

  String textoStatusBanco() {
    if (ultimoStatusApi == 0) return "Banco: aguardando";
    if (ultimoStatusApi == 201 && millis() - ultimoSucessoApi < 3 * INTERVALO_API_MS) return "Banco: OK";
    return String("Banco: falha (HTTP ") + ultimoStatusApi + ")";
  }
#endif

void publicarTelemetria() {
#if USAR_API
  Telemetria t = {
    temperaturaC, umidade, valorLDR, valorPot, ledLigado ? 100 : 0, nomeCorAtual,
    botaoPressionado, sistemaLigado, ledLigado, buzzerLigado,
    temperaturaAtiva, detectorAtivo, buzzerAtivo, alertaTemperatura
  };
  portENTER_CRITICAL(&travaTelemetria);
  telemetria = t;
  telemetriaPronta = true;
  portEXIT_CRITICAL(&travaTelemetria);
#endif
}

// -------------------- ARDUINO IOT CLOUD (OPCIONAL) --------------------
#if USAR_ARDUINO_CLOUD
  #include <ArduinoIoTCloud.h>
  #include <Arduino_ConnectionHandler.h>

  WiFiConnectionHandler conexaoCloud(WIFI_SSID, WIFI_SENHA);

  float cloudTemperatura = -99.0f;
  float cloudUmidade = -1.0f;
  int cloudLuminosidade = -1;
  String cloudEstado = "Iniciando";
  String comandoCloud = "";

  void processarComando(const String &texto);

  void aoAtualizarComandoCloud() {
    String recebido = comandoCloud;
    comandoCloud = "";
    processarComando(recebido);
  }

  void iniciarArduinoCloud() {
    ArduinoCloud.setBoardId(CLOUD_DEVICE_ID);
    ArduinoCloud.setSecretDeviceKey(CLOUD_DEVICE_KEY);

    ArduinoCloud.addProperty(cloudTemperatura, Permission::Read).publishOnChange(0.1, 5000);
    ArduinoCloud.addProperty(cloudUmidade, Permission::Read).publishOnChange(0.5, 5000);
    ArduinoCloud.addProperty(cloudLuminosidade, Permission::Read).publishOnChange(20, 5000);
    ArduinoCloud.addProperty(cloudEstado, Permission::Read).publishOnChange(0, 1000);
    ArduinoCloud.addProperty(comandoCloud, Permission::ReadWrite).onUpdate(aoAtualizarComandoCloud);

    ArduinoCloud.begin(conexaoCloud);
    setDebugMessageLevel(1);
    ArduinoCloud.printDebugInfo();
  }
#endif

// -------------------- FUNCOES AUXILIARES --------------------
void ARDUINO_ISR_ATTR interrupcaoBotao() {
  portENTER_CRITICAL_ISR(&travaBotao);
  interrupcaoRecebida = true;
  portEXIT_CRITICAL_ISR(&travaBotao);
}

void atualizarBotao(uint32_t agora) {
  portENTER_CRITICAL(&travaBotao);
  bool houveInterrupcao = interrupcaoRecebida;
  interrupcaoRecebida = false;
  portEXIT_CRITICAL(&travaBotao);

  if (houveInterrupcao) {
    debouncePendente = true;
    instanteBorda = agora;
  }

  if (!debouncePendente || (agora - instanteBorda < DEBOUNCE_MS)) return;
  debouncePendente = false;

  bool estadoAtual = (digitalRead(PINO_BOTAO) == LOW);
  if (estadoAtual && !botaoPressionado) {
    sistemaLigado = !sistemaLigado;
    corTemporaria = CorFixa::Nenhuma;
    Serial.printf("Botao: sistema %s\n", sistemaLigado ? "LIGADO" : "DESLIGADO");
  }
  botaoPressionado = estadoAtual;
}

int lerAdcMedio(int pino) {
  uint32_t soma = 0;
  for (int i = 0; i < 8; i++) soma += analogRead(pino);
  return soma / 8;
}

void atualizarSensores(uint32_t agora) {
  if (temperaturaAtiva && agora - ultimaLeituraDHT >= INTERVALO_DHT_MS) {
    ultimaLeituraDHT = agora;
    float novaTemperatura = dht.readTemperature();
    float novaUmidade = dht.readHumidity();

    temperaturaC = isfinite(novaTemperatura) ? novaTemperatura : NAN;
    umidade = isfinite(novaUmidade) ? novaUmidade : NAN;
  }

  if (agora - ultimaLeituraADC >= INTERVALO_ADC_MS) {
    ultimaLeituraADC = agora;
    valorPot = lerAdcMedio(PINO_POT);

    if (detectorAtivo) {
      valorLDR = lerAdcMedio(PINO_LDR);

      if (!ldrInicializado) {
        ambienteEscuro = (valorLDR < LIMIAR_LUZ);
        ldrInicializado = true;
      } else if (ambienteEscuro && valorLDR > LIMIAR_LUZ + HISTERESE_LUZ) {
        ambienteEscuro = false;
      } else if (!ambienteEscuro && valorLDR < LIMIAR_LUZ - HISTERESE_LUZ) {
        ambienteEscuro = true;
      }
    } else {
      valorLDR = -1;
      ambienteEscuro = true; // Detector desativado: nao bloqueia a lampada.
    }
  }
}

// Interpola gradualmente entre vermelho, laranja, branco e azul.
RGB corDoPotenciometro(int adc) {
  // Pontos do espectro: vermelho -> laranja -> amarelo -> branco -> turquesa -> azul.
  const RGB paleta[] = {
    {255, 0, 0},
    {255, 70, 0},
    {255, 185, 0},
    {255, 235, 205},
    {0, 195, 255},
    {0, 0, 255}
  };
  constexpr int quantidade = sizeof(paleta) / sizeof(paleta[0]);

  float posicao = (constrain(adc, 0, 4095) / 4095.0f) * (quantidade - 1);
  int indice = (int)posicao;
  if (indice >= quantidade - 1) return paleta[quantidade - 1];

  float fracao = posicao - indice;
  RGB resultado;
  resultado.r = (uint8_t)(paleta[indice].r + (paleta[indice + 1].r - paleta[indice].r) * fracao);
  resultado.g = (uint8_t)(paleta[indice].g + (paleta[indice + 1].g - paleta[indice].g) * fracao);
  resultado.b = (uint8_t)(paleta[indice].b + (paleta[indice + 1].b - paleta[indice].b) * fracao);
  return resultado;
}

RGB obterCorAtual(uint32_t agora) {
  if (corTemporaria != CorFixa::Nenhuma && agora - inicioCorTemporaria < DURACAO_COR_MS) {
    switch (corTemporaria) {
      case CorFixa::Vermelho: return {255, 0, 0};
      case CorFixa::Amarelo:  return {255, 180, 0};
      case CorFixa::Azul:     return {0, 0, 255};
      default: break;
    }
  }
  corTemporaria = CorFixa::Nenhuma;
  return corDoPotenciometro(valorPot);
}

// Nome da cor exibida, no formato aceito pela API/banco.
const char *nomeDaCor() {
  if (!ledLigado) return "Apagado";
  switch (corTemporaria) {
    case CorFixa::Vermelho: return "Vermelho";
    case CorFixa::Amarelo:  return "Amarelo";
    case CorFixa::Azul:     return "Azul";
    default: break;
  }
  // Mesmos 6 pontos da paleta do potenciometro: pega o mais proximo.
  static const char *nomes[] = {"Vermelho", "Laranja", "Amarelo", "Branco", "Turquesa", "Azul"};
  int indice = lroundf(constrain(valorPot, 0, 4095) / 4095.0f * 5);
  return nomes[indice];
}

void escreverCanalRGB(int pino, uint8_t valor) {
  uint8_t saida = LED_ANODO_COMUM ? 255 - valor : valor;
  ledcWrite(pino, saida);
}

void escreverRGB(const RGB &cor) {
  if (!ledLigado) {
    escreverCanalRGB(PINO_RGB_R, 0);
    escreverCanalRGB(PINO_RGB_G, 0);
    escreverCanalRGB(PINO_RGB_B, 0);
    return;
  }
  escreverCanalRGB(PINO_RGB_R, cor.r);
  escreverCanalRGB(PINO_RGB_G, cor.g);
  escreverCanalRGB(PINO_RGB_B, cor.b);
}

void atualizarSaidas(uint32_t agora) {
  bool temperaturaValida = isfinite(temperaturaC);
  alertaTemperatura = temperaturaAtiva && temperaturaValida &&
    (temperaturaC < TEMPERATURA_MIN || temperaturaC > TEMPERATURA_MAX);

  // Com sensor ativo, uma leitura invalida tambem impede acender a lampada.
  bool temperaturaPermite = !temperaturaAtiva || (temperaturaValida && !alertaTemperatura);
  bool luminosidadePermite = !detectorAtivo || ambienteEscuro;
  // "Ligar" acende mesmo com ambiente claro; "Automatico" segue o LDR.
  bool modoPermite = (modoLuz == ModoLuz::Ligado) ||
                     (modoLuz == ModoLuz::Automatico && luminosidadePermite);

  ledLigado = sistemaLigado && temperaturaPermite && modoPermite;
  buzzerLigado = sistemaLigado && buzzerAtivo && alertaTemperatura;

  RGB cor = obterCorAtual(agora);
  nomeCorAtual = nomeDaCor();
  escreverRGB(cor);
  digitalWrite(PINO_BUZZER, buzzerLigado ? HIGH : LOW);
}

void imprimirEstado() {
  Serial.print("LED RGB: ");
  Serial.print(ledLigado ? "ON" : "OFF");
  Serial.print(" | Temperatura: ");

  if (temperaturaAtiva && isfinite(temperaturaC)) {
    Serial.printf("%.1f C", temperaturaC);
  } else if (!temperaturaAtiva) {
    Serial.print("desativada");
  } else {
    Serial.print("sem leitura");
  }

  Serial.printf(" | LDR: %d | Ambiente: %s | Pot: %d | Buzzer: %s | Sistema: %s\n",
                valorLDR,
                detectorAtivo ? (ambienteEscuro ? "escuro" : "claro") : "desativado",
                valorPot,
                buzzerLigado ? "ON" : "OFF",
                sistemaLigado ? "ON" : "OFF");

  if (alertaTemperatura) Serial.println("Perigo! Desligar!");
}

// -------------------- COMANDOS --------------------
void processarComando(const String &textoOriginal) {
  String texto = textoOriginal;
  texto.trim();
  texto.toLowerCase();

  if (texto == "ligar") {
    modoLuz = ModoLuz::Ligado;
  } else if (texto == "desligar") {
    modoLuz = ModoLuz::Desligado;
    corTemporaria = CorFixa::Nenhuma;
  } else if (texto == "automatico" || texto == "automático") {
    modoLuz = ModoLuz::Automatico;
  } else if (texto == "vermelho") {
    corTemporaria = CorFixa::Vermelho;
    inicioCorTemporaria = millis();
  } else if (texto == "amarelo") {
    corTemporaria = CorFixa::Amarelo;
    inicioCorTemporaria = millis();
  } else if (texto == "azul") {
    corTemporaria = CorFixa::Azul;
    inicioCorTemporaria = millis();
  } else if (texto == "desativar temperatura") {
    temperaturaAtiva = false;
    temperaturaC = NAN;
    umidade = NAN;
  } else if (texto == "ativar temperatura") {
    temperaturaAtiva = true;
    ultimaLeituraDHT = millis();
  } else if (texto == "desativar detector") {
    detectorAtivo = false;
    valorLDR = -1;
    ambienteEscuro = true;
  } else if (texto == "ativar detector") {
    detectorAtivo = true;
    ldrInicializado = false;
  } else if (texto == "desativar buzzer") {
    buzzerAtivo = false;
  } else if (texto == "ativar buzzer") {
    buzzerAtivo = true;
  } else {
    Serial.println("Comando desconhecido.");
    return;
  }

  Serial.print("Comando recebido: ");
  Serial.println(texto);
}

void lerComandosSerial() {
  static String buffer = "";
  while (Serial.available()) {
    char caractere = (char)Serial.read();
    if (caractere == '\n' || caractere == '\r') {
      if (buffer.length() > 0) {
        processarComando(buffer);
        buffer = "";
      }
    } else if (buffer.length() < 80) {
      buffer += caractere;
    }
  }
}

// -------------------- SETUP E LOOP --------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  analogReadResolution(12);
  analogSetPinAttenuation(PINO_LDR, ADC_11db);
  analogSetPinAttenuation(PINO_POT, ADC_11db);

  dht.begin();

  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_BUZZER, OUTPUT);
  digitalWrite(PINO_BUZZER, LOW);

  for (int pino : PINOS_RGB) {
    if (!ledcAttach(pino, 5000, 8)) {
      Serial.printf("Erro ao configurar PWM no GPIO %d\n", pino);
    }
  }

  escreverRGB({0, 0, 0});
  botaoPressionado = (digitalRead(PINO_BOTAO) == LOW);
  attachInterrupt(digitalPinToInterrupt(PINO_BOTAO), interrupcaoBotao, CHANGE);

  ultimaLeituraDHT = millis() - INTERVALO_DHT_MS;
  ultimaLeituraADC = millis() - INTERVALO_ADC_MS;

  Serial.println("Lampada inteligente - ESP32-WROOM iniciada");
  Serial.println("Comandos Serial: Ligar, Desligar, Automatico, Vermelho, Amarelo, Azul,");
  Serial.println("Ativar Temperatura, Desativar Temperatura, Ativar Detector, Desativar Detector,");
  Serial.println("Ativar Buzzer, Desativar Buzzer.");

#if USAR_ARDUINO_CLOUD
  if (strlen(WIFI_SSID) == 0 || strlen(CLOUD_DEVICE_ID) == 0 || strlen(CLOUD_DEVICE_KEY) == 0) {
    Serial.println("Arduino Cloud: preencha Wi-Fi, Device ID e Secret Key.");
  } else {
    iniciarArduinoCloud();
  }
#endif

#if USAR_API
  iniciarApi();  // Depois do Cloud, que (se ativo) cuida do Wi-Fi.
#endif
}

void loop() {
  uint32_t agora = millis();

  atualizarBotao(agora);
  lerComandosSerial();
  atualizarSensores(agora);
  atualizarSaidas(agora);
  publicarTelemetria();

  if (agora - ultimaImpressaoSerial >= INTERVALO_SERIAL_MS) {
    ultimaImpressaoSerial = agora;
    imprimirEstado();
  }

#if USAR_ARDUINO_CLOUD
  ArduinoCloud.update();
  cloudTemperatura = isfinite(temperaturaC) ? temperaturaC : -99.0f;
  cloudUmidade = isfinite(umidade) ? umidade : -1.0f;
  cloudLuminosidade = valorLDR;
  cloudEstado = String("LED: ") + (ledLigado ? "ON" : "OFF") +
                " | Cor: " + nomeCorAtual +
                " | Botao: " + (botaoPressionado ? "pressionado" : "solto") +
                " | Sistema: " + (sistemaLigado ? "ON" : "OFF") +
  #if USAR_API
                " | " + textoStatusBanco() +
  #endif
                (alertaTemperatura ? " | Perigo! Desligar!" : "");
#endif

  delay(5);
}
