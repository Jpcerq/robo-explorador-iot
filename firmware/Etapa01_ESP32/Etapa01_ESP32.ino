// =====================================================================
// ESP32-WROOM-32 (DevKit) - MONITORAMENTO
// Recebe por ESP-NOW o JSON enviado pelo ESP32-S3 do chão de fábrica,
// mostra os dados na matriz 8x8 (MAX7219), acende os LEDs de status e
// repassa cada pacote pela USB (Serial) para a ponte Python -> banco.
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Selecione ESP32 Dev Module no menu Ferramentas > Placa."
#endif

// =====================================================================
// PINAGEM (apenas deste ESP32-WROOM)
// =====================================================================
// Matriz: mantidos os pinos que já estão ligados.
//   VCC -> V5 (NÃO no 3V3)   GND -> GND
constexpr uint8_t PIN_MATRIZ_DIN = 25;
constexpr uint8_t PIN_MATRIZ_CS  = 26;
constexpr uint8_t PIN_MATRIZ_CLK = 27;

// LEDs de status: GPIO -> resistor 220 ohm -> perna longa do LED; perna curta no GND.
constexpr uint8_t PIN_LED_VERDE    = 32;
constexpr uint8_t PIN_LED_VERMELHO = 33;

// =====================================================================
// MATRIZ
// =====================================================================
// Se o "OK" do início sair espelhado, de cabeça para baixo ou embaralhado,
// troque esta linha, nesta ordem: GENERIC_HW -> FC16_HW -> PAROLA_HW -> ICSTATION_HW
#define HARDWARE_TYPE MD_MAX72XX::GENERIC_HW
constexpr uint8_t NUM_MATRIZES = 1;           // UMA matriz 8x8
constexpr uint8_t BRILHO_MATRIZ = 3;          // 0 a 15
constexpr uint16_t VELOCIDADE_SCROLL_MS = 45; // ms por coluna; maior = mais lento

// =====================================================================
// ESP-NOW
// =====================================================================
// Mesmo canal do ESP32-S3 (CANAL_ESPNOW = 1 lá).
constexpr uint8_t CANAL_ESPNOW = 1;
// MAC Station do ESP32-S3 (aparece no Serial dele: "MAC local: ...").
// Tudo zero = MODO DESCOBERTA: aceita qualquer emissor e mostra o MAC dele.
// Depois de preenchido, só esse MAC é aceito (validação pedida no edital).
constexpr uint8_t MAC_EMISSOR[6] = {0xE8, 0x31, 0xCD, 0xAD, 0x84, 0xDC};

// =====================================================================
// TEMPOS E LIMITES
// =====================================================================
constexpr uint32_t TEMPO_TELA_MS = 2000;   // cada tela fica no mínimo 2 s
constexpr uint32_t TIMEOUT_RX_MS = 5000;   // sem pacote há 5 s = falha de comunicação
constexpr size_t TAM_MAX_PACOTE = 250;     // limite de dados do ESP-NOW
constexpr uint8_t TAM_FILA = 4;

// =====================================================================
// SAÍDA SERIAL (ponte Python)
// =====================================================================
// Cada pacote válido é impresso UMA vez, sozinho na linha, exatamente como o
// S3 enviou: {"versao":1,"sequencia":...}. Nenhuma outra linha contém '{'.
// Se a ponte Python quebrar com as linhas de log, mude para false:
// aí o Serial passa a ter SOMENTE as linhas JSON.
constexpr bool LOGS_DETALHADOS = true;

// =====================================================================
// ESTRUTURAS DE DADOS
// =====================================================================
// Pacote "cru", copiado no callback e processado depois no loop().
struct PacoteBruto {
  uint8_t mac[6];
  uint16_t tamanho;
  char dados[TAM_MAX_PACOTE + 1];   // +1 para garantir o '\0'
};

// Leitura já validada. NAN = o sensor falhou no emissor (chegou "null").
struct Leitura {
  uint32_t sequencia;
  uint64_t timestamp;     // ms desde o boot do S3 ("relogio":"uptime_ms")
  float nivel;
  float temperatura;
  float umidade;
  uint16_t luminosidade;
  int8_t presenca;        // -1 = campo não enviado, 0 = ausente, 1 = detectada
};

enum Tela : uint8_t { TELA_NVL, TELA_TMP, TELA_UMD, TELA_LUX, TELA_PRS, TOTAL_TELAS };
const char *const NOME_TELA[TOTAL_TELAS] = {"NVL", "TMP", "UMD", "LUX", "PRS"};

// =====================================================================
// ESTADO GLOBAL
// =====================================================================
// Construtor com DIN/CLK/CS explícitos = SPI por software nos pinos 25/27/26.
MD_Parola matriz(HARDWARE_TYPE, PIN_MATRIZ_DIN, PIN_MATRIZ_CLK, PIN_MATRIZ_CS, NUM_MATRIZES);
QueueHandle_t filaRx = nullptr;

// Escrita no callback (tarefa do Wi-Fi), lida no loop(): volatile.
volatile uint32_t pacotesDescartados = 0;
uint32_t descartadosInformados = 0;

Leitura ultima = {};
bool temDados = false;
uint32_t lastRxMillis = 0;
uint32_t pacotesPerdidos = 0;

// A MD_Parola guarda só o PONTEIRO do texto: o buffer precisa ser global.
char textoTela[24] = "";
uint8_t telaAtual = TELA_NVL;
bool modoAlerta = true;          // true = mostrando "SEM DADOS"
bool erroRenderizacao = false;
uint32_t inicioTela = 0;
int8_t estadoLeds = -1;          // -1 = indefinido, 0 = vermelho, 1 = verde

// Log que pode ser silenciado (LOGS_DETALHADOS = false).
#define LOG(...) do { if (LOGS_DETALHADOS) Serial.printf(__VA_ARGS__); } while (0)

// =====================================================================
// CALLBACK DE RECEPÇÃO (roda na tarefa do Wi-Fi: só copia e enfileira)
// =====================================================================
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int tamanho) {
  const uint8_t *mac = info->src_addr;
#else
void aoReceber(const uint8_t *mac, const uint8_t *dados, int tamanho) {
#endif
  if (tamanho <= 0 || tamanho > static_cast<int>(TAM_MAX_PACOTE)) {
    pacotesDescartados++;
    return;
  }
  PacoteBruto pacote;
  memcpy(pacote.mac, mac, 6);
  memcpy(pacote.dados, dados, tamanho);
  pacote.dados[tamanho] = '\0';   // o S3 já manda o '\0', mas não confiamos nisso
  pacote.tamanho = static_cast<uint16_t>(tamanho);
  if (xQueueSend(filaRx, &pacote, 0) != pdTRUE) pacotesDescartados++;
}

// =====================================================================
// UTILITÁRIOS
// =====================================================================
void macParaTexto(const uint8_t *mac, char *destino, size_t tamanho) {
  snprintf(destino, tamanho, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool modoDescoberta() {
  for (uint8_t valor : MAC_EMISSOR) if (valor != 0) return false;
  return true;
}

void valorOuTraco(float valor, char *destino, size_t tamanho) {
  if (isfinite(valor)) snprintf(destino, tamanho, "%.0f", valor);
  else snprintf(destino, tamanho, "--");
}

// =====================================================================
// LEITURA DO JSON (sem ArduinoJson, no mesmo formato do S3)
// {"versao":1,"sequencia":12,"nivel_tinta":75.0,"temperatura":24.0,
//  "umidade":55.0,"luminosidade":680,"timestamp":123456,"relogio":"uptime_ms"}
// =====================================================================
enum class Campo { OK, NULO, AUSENTE, INVALIDO };

Campo lerFloat(const char *json, const char *chave, float &saida);
Campo lerInteiro(const char *json, const char *chave, uint64_t &saida);

const char *acharValor(const char *json, const char *chave) {
  char padrao[32];
  int n = snprintf(padrao, sizeof(padrao), "\"%s\":", chave);
  if (n < 0 || n >= static_cast<int>(sizeof(padrao))) return nullptr;
  const char *p = strstr(json, padrao);
  if (p == nullptr) return nullptr;
  p += n;
  while (*p == ' ') p++;
  return p;
}

Campo lerFloat(const char *json, const char *chave, float &saida) {
  const char *p = acharValor(json, chave);
  if (p == nullptr) return Campo::AUSENTE;
  if (strncmp(p, "null", 4) == 0) { saida = NAN; return Campo::NULO; }
  char *fim = nullptr;
  float valor = strtof(p, &fim);
  if (fim == p || !isfinite(valor)) return Campo::INVALIDO;
  saida = valor;
  return Campo::OK;
}

Campo lerInteiro(const char *json, const char *chave, uint64_t &saida) {
  const char *p = acharValor(json, chave);
  if (p == nullptr) return Campo::AUSENTE;
  if (strncmp(p, "null", 4) == 0) return Campo::NULO;
  if (*p == '-') return Campo::INVALIDO;
  char *fim = nullptr;
  unsigned long long valor = strtoull(p, &fim, 10);
  if (fim == p) return Campo::INVALIDO;
  saida = valor;
  return Campo::OK;
}

// Sensor pode vir null (falha no S3), mas não pode faltar nem sair da faixa física.
bool validarSensor(const char *json, const char *chave, float minimo, float maximo,
                   float &saida) {
  Campo c = lerFloat(json, chave, saida);
  if (c == Campo::NULO) return true;
  return c == Campo::OK && saida >= minimo && saida <= maximo;
}

bool decodificar(const char *json, Leitura &leitura, const char *&motivo) {
  uint64_t inteiro = 0;
  if (lerInteiro(json, "versao", inteiro) != Campo::OK || inteiro != 1) {
    motivo = "versao ausente ou diferente de 1"; return false;
  }
  if (lerInteiro(json, "sequencia", inteiro) != Campo::OK || inteiro > UINT32_MAX) {
    motivo = "sequencia invalida"; return false;
  }
  leitura.sequencia = static_cast<uint32_t>(inteiro);
  if (lerInteiro(json, "timestamp", inteiro) != Campo::OK) {
    motivo = "timestamp invalido"; return false;
  }
  leitura.timestamp = inteiro;
  if (lerInteiro(json, "luminosidade", inteiro) != Campo::OK || inteiro > 4095) {
    motivo = "luminosidade fora de 0-4095"; return false;
  }
  leitura.luminosidade = static_cast<uint16_t>(inteiro);
  if (!validarSensor(json, "nivel_tinta", 0.0f, 100.0f, leitura.nivel)) {
    motivo = "nivel_tinta invalido"; return false;
  }
  if (!validarSensor(json, "temperatura", -40.0f, 80.0f, leitura.temperatura)) {
    motivo = "temperatura invalida"; return false;
  }
  if (!validarSensor(json, "umidade", 0.0f, 100.0f, leitura.umidade)) {
    motivo = "umidade invalida"; return false;
  }
  // Presença é opcional: o S3 atual ainda não envia esse campo.
  Campo c = lerInteiro(json, "presenca", inteiro);
  if (c == Campo::AUSENTE || c == Campo::NULO) leitura.presenca = -1;
  else if (c == Campo::OK && inteiro <= 1) leitura.presenca = static_cast<int8_t>(inteiro);
  else { motivo = "presenca deve ser 0 ou 1"; return false; }
  return true;
}

// =====================================================================
// PROCESSAMENTO DOS PACOTES (no loop, fora do callback)
// =====================================================================
void imprimirResumo(const Leitura &l) {
  char nvl[8], tmp[8], umd[8];
  valorOuTraco(l.nivel, nvl, sizeof(nvl));
  valorOuTraco(l.temperatura, tmp, sizeof(tmp));
  valorOuTraco(l.umidade, umd, sizeof(umd));
  const char *prs = l.presenca < 0 ? "N/D" : (l.presenca ? "1" : "0");
  LOG("RX: nivel=%s%% temp=%sC umd=%s%% lux=%u prs=%s ts=%" PRIu64 "ms seq=%" PRIu32 "\n",
      nvl, tmp, umd, static_cast<unsigned>(l.luminosidade), prs, l.timestamp, l.sequencia);
}

void processarRecepcoes() {
  PacoteBruto pacote;
  while (xQueueReceive(filaRx, &pacote, 0) == pdTRUE) {
    char macTxt[18];
    macParaTexto(pacote.mac, macTxt, sizeof(macTxt));

    // 1) Só aceita o coletor autorizado.
    if (!modoDescoberta() && memcmp(pacote.mac, MAC_EMISSOR, 6) != 0) {
      LOG("Pacote ignorado: MAC %s nao autorizado.\n", macTxt);
      continue;
    }

    // 2) Valida o conteúdo. (O conteúdo inválido NÃO é impresso, para a
    //    ponte Python não gravar lixo no banco.)
    Leitura nova = {};
    const char *motivo = "";
    if (!decodificar(pacote.dados, nova, motivo)) {
      LOG("Pacote invalido de %s descartado: %s\n", macTxt, motivo);
      continue;
    }
    if (modoDescoberta()) {
      LOG("[MODO DESCOBERTA] Emissor: %s -> copie para MAC_EMISSOR.\n", macTxt);
    }

    // 3) Sequência: detecta duplicatas e perdas.
    if (temDados) {
      if (nova.sequencia == ultima.sequencia) {
        LOG("Pacote duplicado ignorado (seq=%" PRIu32 ").\n", nova.sequencia);
        continue;
      }
      if (nova.sequencia > ultima.sequencia + 1) {
        uint32_t perdidos = nova.sequencia - ultima.sequencia - 1;
        pacotesPerdidos += perdidos;
        LOG("Aviso: %" PRIu32 " pacote(s) perdido(s). Total: %" PRIu32 "\n",
            perdidos, pacotesPerdidos);
      } else if (nova.sequencia < ultima.sequencia) {
        LOG("Sequencia reiniciou: o S3 provavelmente foi reiniciado.\n");
      }
    }

    // 4) Guarda e marca o horário da recepção.
    ultima = nova;
    temDados = true;
    lastRxMillis = millis();

    // 5) Linha para a ponte Python: o JSON exato, sozinho na linha, uma única vez.
    Serial.println(pacote.dados);
    imprimirResumo(ultima);
  }
}

// =====================================================================
// MATRIZ DE LEDS
// =====================================================================
bool montarTexto(uint8_t tela, char *destino, size_t tamanho) {
  int n = 0;
  switch (tela) {
    case TELA_NVL:
      n = isfinite(ultima.nivel) ? snprintf(destino, tamanho, "NVL %.0f%%", ultima.nivel)
                                 : snprintf(destino, tamanho, "NVL ERR");
      break;
    case TELA_TMP:
      n = isfinite(ultima.temperatura)
              ? snprintf(destino, tamanho, "TMP %.0fC", ultima.temperatura)
              : snprintf(destino, tamanho, "TMP ERR");
      break;
    case TELA_UMD:
      n = isfinite(ultima.umidade) ? snprintf(destino, tamanho, "UMD %.0f%%", ultima.umidade)
                                   : snprintf(destino, tamanho, "UMD ERR");
      break;
    case TELA_LUX:
      n = snprintf(destino, tamanho, "LUX %u", static_cast<unsigned>(ultima.luminosidade));
      break;
    case TELA_PRS:
      n = snprintf(destino, tamanho, "PRS %s",
                   ultima.presenca < 0 ? "N/D" : (ultima.presenca ? "ON" : "OFF"));
      break;
    default:
      return false;
  }
  return n > 0 && static_cast<size_t>(n) < tamanho;
}

// Rolagem: o texto entra pela direita e sai pela esquerda (8 colunas não
// comportam "NVL 75%" parado).
void iniciarAnimacao(const char *texto) {
  strlcpy(textoTela, texto, sizeof(textoTela));
  matriz.displayClear();
  matriz.displayText(textoTela, PA_LEFT, VELOCIDADE_SCROLL_MS, 0,
                     PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  inicioTela = millis();
}

void mostrarTelaAtual() {
  char texto[sizeof(textoTela)];
  erroRenderizacao = !montarTexto(telaAtual, texto, sizeof(texto));
  if (erroRenderizacao) {
    LOG("Erro de renderizacao na tela %s.\n", NOME_TELA[telaAtual]);
    strlcpy(texto, "ERRO", sizeof(texto));
  }
  iniciarAnimacao(texto);
  LOG("Tela -> %s (\"%s\")\n", NOME_TELA[telaAtual], texto);
}

// displayAnimate() avança no máximo um passo e retorna logo: nunca trava o loop.
void atualizarMatriz(bool comunicacaoOk) {
  bool animacaoTerminou = matriz.displayAnimate();

  if (!comunicacaoOk) {
    if (!modoAlerta) {
      modoAlerta = true;
      LOG("Tela -> SEM DADOS\n");
      iniciarAnimacao("SEM DADOS");
    } else if (animacaoTerminou) {
      matriz.displayReset();   // repete o alerta continuamente
    }
    return;
  }

  if (modoAlerta) {            // voltou a receber: recomeça pela tela 1
    modoAlerta = false;
    telaAtual = TELA_NVL;
    mostrarTelaAtual();
    return;
  }

  // Troca de tela quando a rolagem terminou E já se passaram 2 s.
  if (animacaoTerminou &&
      static_cast<uint32_t>(millis() - inicioTela) >= TEMPO_TELA_MS) {
    telaAtual = (telaAtual + 1) % TOTAL_TELAS;
    mostrarTelaAtual();
  }
}

// =====================================================================
// LEDS DE STATUS
// =====================================================================
void atualizarLedsStatus(bool comunicacaoOk) {
  int8_t novoEstado = (comunicacaoOk && !erroRenderizacao) ? 1 : 0;
  if (novoEstado == estadoLeds) return;   // age só na MUDANÇA de estado
  estadoLeds = novoEstado;
  digitalWrite(PIN_LED_VERDE, novoEstado ? HIGH : LOW);
  digitalWrite(PIN_LED_VERMELHO, novoEstado ? LOW : HIGH);
  if (novoEstado) LOG("LED VERDE ON - dados recebidos\n");
  else if (!comunicacaoOk) LOG("LED VERMELHO ON - timeout de comunicacao\n");
  else LOG("LED VERMELHO ON - erro de renderizacao\n");
}

// =====================================================================
// RÁDIO
// =====================================================================
bool iniciarRadio() {
  WiFi.persistent(false);          // não grava nem usa redes salvas na flash
  if (!WiFi.mode(WIFI_STA)) return false;   // ESP-NOW usa a interface Station
  WiFi.disconnect();               // evita reconectar a um roteador (mudaria o canal)
  WiFi.setSleep(false);
  esp_err_t erro = esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);
  if (erro == ESP_OK) erro = esp_now_init();
  if (erro == ESP_OK) erro = esp_now_register_recv_cb(aoReceber);
  if (erro != ESP_OK) {
    Serial.printf("Falha ao iniciar ESP-NOW: %s\n", esp_err_to_name(erro));
    return false;
  }
  uint8_t macLocal[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, macLocal);
  char macTxt[18];
  macParaTexto(macLocal, macTxt, sizeof(macTxt));
  LOG("ESP-NOW pronto. MAC Station deste receptor: %s | Canal: %u\n", macTxt, CANAL_ESPNOW);
  LOG("Esse MAC deve ser igual ao MAC_RECEPTOR do ESP32-S3 (E8:31:CD:AD:84:DC).\n");
  if (modoDescoberta()) {
    LOG("MAC_EMISSOR vazio: modo descoberta (aceitando qualquer emissor).\n");
  }
  return true;
}

// =====================================================================
// SETUP E LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  LOG("\nESP32-WROOM - monitoramento (matriz + ESP-NOW + ponte serial)\n");

  // Autoteste dos LEDs: verde, depois vermelho.
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  digitalWrite(PIN_LED_VERDE, HIGH);
  digitalWrite(PIN_LED_VERMELHO, LOW);
  delay(350);
  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERMELHO, HIGH);
  delay(350);

  // Teste "Hello/OK" do Passo 5: rola "OK" uma vez.
  matriz.begin();
  matriz.setIntensity(BRILHO_MATRIZ);
  matriz.displayClear();
  iniciarAnimacao("OK");
  while (!matriz.displayAnimate()) delay(1);
  LOG("Teste da matriz concluido (deve ter aparecido OK rolando).\n");

  filaRx = xQueueCreate(TAM_FILA, sizeof(PacoteBruto));
  if (filaRx == nullptr) {
    Serial.println("Erro: memoria insuficiente para a fila de recepcao.");
    while (true) delay(1000);
  }

  if (!iniciarRadio()) {
    iniciarAnimacao("ERRO RADIO");
    while (true) {
      if (matriz.displayAnimate()) matriz.displayReset();
      delay(1);
    }
  }

  modoAlerta = true;
  iniciarAnimacao("SEM DADOS");
  LOG("Tela -> SEM DADOS (aguardando o ESP32-S3)\n");
}

void loop() {
  processarRecepcoes();

  // Subtração sem sinal: continua correta quando millis() dá a volta (~49 dias).
  bool comunicacaoOk = temDados &&
      static_cast<uint32_t>(millis() - lastRxMillis) <= TIMEOUT_RX_MS;

  atualizarMatriz(comunicacaoOk);
  atualizarLedsStatus(comunicacaoOk);

  uint32_t descartados = pacotesDescartados;
  if (descartados != descartadosInformados) {
    LOG("Aviso: %" PRIu32 " pacote(s) descartado(s) (fila cheia ou tamanho invalido).\n",
        descartados);
    descartadosInformados = descartados;
  }

  delay(1);   // cede tempo ao sistema; não atrapalha a rolagem
}
