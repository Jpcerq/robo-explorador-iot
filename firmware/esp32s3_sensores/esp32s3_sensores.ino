#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>
#include <inttypes.h>
#include <string.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Selecione ESP32S3 Dev Module no menu Ferramentas > Placa."
#endif

// ================= PINAGEM =================

constexpr uint8_t PIN_TRIG = 5;
constexpr uint8_t PIN_ECHO = 18;
constexpr uint8_t PIN_DHT = 4;
constexpr uint8_t PIN_LDR = 6;
constexpr uint8_t PIN_LED_VERDE = 15;
constexpr uint8_t PIN_LED_VERMELHO = 16;

constexpr uint8_t TIPO_DHT = DHT11;

// ================= ESP-NOW =================

constexpr bool ATIVAR_ESPNOW = true;

// MAC Station do ESP32-WROOM-32 receptor. E8:31:CD:AD:84:DC
constexpr uint8_t MAC_RECEPTOR[6] = {
  0xE8, 0x31, 0xCD, 0xAD, 0x84, 0xDC
};

constexpr uint8_t CANAL_ESPNOW = 1;

// ================= CONFIGURAÇÕES =================

constexpr float DISTANCIA_VAZIO_CM = 30.0f;
constexpr float DISTANCIA_CHEIO_CM = 5.0f;
constexpr float LIMITE_TINTA_BAIXA = 20.0f;

constexpr uint32_t INTERVALO_LEITURA_MS = 2000;
constexpr uint32_t TIMEOUT_ECHO_US = 30000;
constexpr uint32_t PULSO_LED_MS = 120;

static_assert(
  DISTANCIA_VAZIO_CM > DISTANCIA_CHEIO_CM,
  "Distancia do tanque vazio deve ser maior que a do cheio."
);

static_assert(
  DISTANCIA_CHEIO_CM >= 2.0f && DISTANCIA_VAZIO_CM <= 400.0f,
  "Calibracao fora da faixa nominal do HC-SR04."
);

static_assert(
  INTERVALO_LEITURA_MS >= 2000,
  "Respeite o intervalo do DHT."
);

static_assert(
  CANAL_ESPNOW >= 1 && CANAL_ESPNOW <= 11,
  "Canal invalido."
);

// ================= DADOS =================

struct Leitura {
  uint64_t timestamp;
  uint32_t sequencia;
  float distanciaCm;
  float nivel;
  float temperatura;
  float umidade;
  uint16_t luminosidade;
};

// Declarações explícitas para funções que utilizam Leitura.
inline bool haAlertaSensores(const Leitura &leitura);
inline int criarPacote(
  const Leitura &leitura,
  char *destino,
  size_t tamanho
);
Leitura lerSensores();
void exibirLeitura(const Leitura &leitura);
void enviarLeitura(const Leitura &leitura);

// ================= VARIÁVEIS =================

DHT dht(PIN_DHT, TIPO_DHT);

QueueHandle_t filaEnvio = nullptr;

bool radioPronto = false;
bool envioPendente = false;
bool falhaComunicacao = ATIVAR_ESPNOW;
bool alertaSensores = true;

uint32_t ultimoCiclo = 0;
uint32_t sequencia = 0;

char pacoteEmEnvio[250] = {};

// 0 = sem animação; 1 = sucesso; 2 = falha.
uint8_t eventoLed = 0;
uint32_t inicioEventoLed = 0;

// ================= CÁLCULO DO NÍVEL =================

inline float calcularNivel(float distanciaCm) {
  if (!isfinite(distanciaCm) ||
      distanciaCm < 2.0f ||
      distanciaCm > 400.0f) {
    return NAN;
  }

  float nivel =
    100.0f * (DISTANCIA_VAZIO_CM - distanciaCm) /
    (DISTANCIA_VAZIO_CM - DISTANCIA_CHEIO_CM);

  return fmaxf(0.0f, fminf(100.0f, nivel));
}

inline bool haAlertaSensores(const Leitura &leitura) {
  return !isfinite(leitura.nivel) ||
         !isfinite(leitura.temperatura) ||
         !isfinite(leitura.umidade) ||
         leitura.nivel < LIMITE_TINTA_BAIXA;
}

// ================= JSON =================

inline void numeroJson(
  float numero,
  char *destino,
  size_t tamanho
) {
  if (isfinite(numero)) {
    snprintf(destino, tamanho, "%.1f", numero);
  } else {
    snprintf(destino, tamanho, "null");
  }
}

// Timestamp em milissegundos desde o boot.
// Mantém o formato esperado pela ponte Python.
inline int criarPacote(
  const Leitura &leitura,
  char *destino,
  size_t tamanho
) {
  char nivel[16];
  char temperatura[16];
  char umidade[16];

  numeroJson(leitura.nivel, nivel, sizeof(nivel));
  numeroJson(leitura.temperatura, temperatura, sizeof(temperatura));
  numeroJson(leitura.umidade, umidade, sizeof(umidade));

  return snprintf(
    destino,
    tamanho,
    "{\"versao\":1,\"sequencia\":%" PRIu32
    ",\"nivel_tinta\":%s,"
    "\"temperatura\":%s,"
    "\"umidade\":%s,"
    "\"luminosidade\":%u,"
    "\"timestamp\":%" PRIu64
    ",\"relogio\":\"uptime_ms\"}",
    leitura.sequencia,
    nivel,
    temperatura,
    umidade,
    static_cast<unsigned>(leitura.luminosidade),
    leitura.timestamp
  );
}

// ================= CALLBACK DE ENVIO =================

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void aoEnviar(
  const esp_now_send_info_t *,
  esp_now_send_status_t status
) {
#else
void aoEnviar(
  const uint8_t *,
  esp_now_send_status_t status
) {
#endif
  xQueueSend(filaEnvio, &status, 0);
}

// ================= LEDs =================

void sinalizarEvento(uint8_t evento) {
  eventoLed = evento;
  inicioEventoLed = millis();
}

void atualizarLeds() {
  bool vermelho =
    alertaSensores ||
    (ATIVAR_ESPNOW && falhaComunicacao);

  bool verde = !vermelho;

  uint32_t decorrido = millis() - inicioEventoLed;

  if (eventoLed != 0 && decorrido < 2 * PULSO_LED_MS) {
    bool primeiraFase = decorrido < PULSO_LED_MS;

    if (eventoLed == 1) {
      verde = primeiraFase ? !verde : verde;
    } else {
      verde = false;
      vermelho = !primeiraFase;
    }
  } else {
    eventoLed = 0;
  }

  digitalWrite(PIN_LED_VERDE, verde ? HIGH : LOW);
  digitalWrite(PIN_LED_VERMELHO, vermelho ? HIGH : LOW);
}

// ================= INICIALIZAÇÃO DO RÁDIO =================

bool macValido() {
  bool algumByte = false;

  for (uint8_t valor : MAC_RECEPTOR) {
    algumByte |= valor != 0;
  }

  return algumByte && (MAC_RECEPTOR[0] & 0x01) == 0;
}

bool iniciarRadio() {
  if (!macValido()) {
    Serial.println("Configure um MAC_RECEPTOR unicast valido.");
    return false;
  }

  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("Falha ao ativar Wi-Fi Station.");
    return false;
  }

  WiFi.setSleep(false);

  esp_err_t erro = esp_wifi_set_channel(
    CANAL_ESPNOW,
    WIFI_SECOND_CHAN_NONE
  );

  if (erro == ESP_OK) {
    erro = esp_now_init();
  }

  if (erro != ESP_OK) {
    Serial.printf(
      "Falha ao iniciar radio: %s\n",
      esp_err_to_name(erro)
    );
    return false;
  }

  erro = esp_now_register_send_cb(aoEnviar);

  if (erro == ESP_OK) {
    esp_now_peer_info_t receptor = {};

    memcpy(
      receptor.peer_addr,
      MAC_RECEPTOR,
      sizeof(MAC_RECEPTOR)
    );

    receptor.channel = CANAL_ESPNOW;
    receptor.ifidx = WIFI_IF_STA;
    receptor.encrypt = false;

    erro = esp_now_add_peer(&receptor);
  }

  if (erro != ESP_OK) {
    Serial.printf(
      "Falha ao configurar ESP-NOW: %s\n",
      esp_err_to_name(erro)
    );

    esp_now_deinit();
    return false;
  }

  uint8_t macLocal[6] = {};

  erro = esp_wifi_get_mac(WIFI_IF_STA, macLocal);

  if (erro != ESP_OK) {
    Serial.printf(
      "Falha ao ler MAC Station: %s\n",
      esp_err_to_name(erro)
    );

    esp_now_deinit();
    return false;
  }

  Serial.printf(
    "ESP-NOW pronto. MAC local: "
    "%02X:%02X:%02X:%02X:%02X:%02X | Canal: %u\n",
    macLocal[0],
    macLocal[1],
    macLocal[2],
    macLocal[3],
    macLocal[4],
    macLocal[5],
    CANAL_ESPNOW
  );

  Serial.printf(
    "Destino: %02X:%02X:%02X:%02X:%02X:%02X\n",
    MAC_RECEPTOR[0],
    MAC_RECEPTOR[1],
    MAC_RECEPTOR[2],
    MAC_RECEPTOR[3],
    MAC_RECEPTOR[4],
    MAC_RECEPTOR[5]
  );

  return true;
}

// ================= LEITURA DOS SENSORES =================

float lerDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);

  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracao = pulseIn(
    PIN_ECHO,
    HIGH,
    TIMEOUT_ECHO_US
  );

  if (duracao == 0) {
    return NAN;
  }

  return duracao * 0.0343f / 2.0f;
}

Leitura lerSensores() {
  Leitura leitura = {};

  leitura.timestamp =
    static_cast<uint64_t>(esp_timer_get_time() / 1000);

  leitura.sequencia = ++sequencia;

  leitura.distanciaCm = lerDistanciaCm();
  leitura.nivel = calcularNivel(leitura.distanciaCm);

  leitura.umidade = dht.readHumidity();
  leitura.temperatura = dht.readTemperature();

  float minima = TIPO_DHT == DHT11 ? 0.0f : -40.0f;
  float maxima = TIPO_DHT == DHT11 ? 50.0f : 80.0f;

  if (!isfinite(leitura.temperatura) ||
      leitura.temperatura < minima ||
      leitura.temperatura > maxima) {
    leitura.temperatura = NAN;
  }

  if (!isfinite(leitura.umidade) ||
      leitura.umidade < 0.0f ||
      leitura.umidade > 100.0f) {
    leitura.umidade = NAN;
  }

  leitura.luminosidade = analogRead(PIN_LDR);

  return leitura;
}

// ================= MONITOR SERIAL =================

void exibirLeitura(const Leitura &leitura) {
  Serial.println();
  Serial.println("---------------- NOVA LEITURA ----------------");

  if (isfinite(leitura.distanciaCm)) {
    Serial.printf(
      "Distancia ate a superficie: %.1f cm\n",
      leitura.distanciaCm
    );
  }

  if (isfinite(leitura.nivel)) {
    Serial.printf(
      "Nivel do tanque: %.1f%%\n",
      leitura.nivel
    );

    if (leitura.nivel < LIMITE_TINTA_BAIXA) {
      Serial.println("Alerta! Nivel de tinta baixo.");
    }
  } else {
    Serial.println(
      "Falha no ultrassonico: sem eco ou distancia invalida."
    );
  }

  if (isfinite(leitura.temperatura) &&
      isfinite(leitura.umidade)) {
    Serial.printf(
      "Temperatura: %.1f C | Umidade: %.1f%%\n",
      leitura.temperatura,
      leitura.umidade
    );
  } else {
    Serial.println(
      "Falha no DHT. Confira alimentacao, modelo e DATA."
    );
  }

  Serial.printf(
    "Luminosidade: %u (ADC de 0 a 4095)\n",
    leitura.luminosidade
  );

  Serial.printf(
    "Timestamp: %" PRIu64 " ms desde a inicializacao\n",
    leitura.timestamp
  );

  if (alertaSensores) {
    Serial.println(
      "LED VERMELHO: verificar tanque de tinta e sensores."
    );

    if (!isfinite(leitura.nivel)) {
      Serial.println(
        "Motivo: ultrassonico sem leitura valida."
      );
    } else if (leitura.nivel < LIMITE_TINTA_BAIXA) {
      Serial.println(
        "Motivo: nivel de tinta abaixo de 20%."
      );
    }

    if (!isfinite(leitura.temperatura) ||
        !isfinite(leitura.umidade)) {
      Serial.println(
        "Motivo: DHT sem leitura valida."
      );
    }
  } else if (ATIVAR_ESPNOW && falhaComunicacao) {
    Serial.println(
      "LED VERMELHO: verificar comunicacao ESP-NOW."
    );
  } else {
    Serial.println("LED VERDE: operacao normal.");
  }
}

// ================= RESULTADO DO ENVIO =================

void tratarResultadoEnvio() {
  esp_now_send_status_t status;

  if (xQueueReceive(filaEnvio, &status, 0) != pdTRUE) {
    return;
  }

  envioPendente = false;
  falhaComunicacao = status != ESP_NOW_SEND_SUCCESS;

  if (!falhaComunicacao) {
    Serial.printf(
      "Pacote enviado com sucesso: %s\n",
      pacoteEmEnvio
    );

    if (!alertaSensores) {
      Serial.println("Operacao normal.");
    }

    sinalizarEvento(1);
  } else {
    Serial.println(
      "Falha no envio ESP-NOW. Nova tentativa no proximo ciclo."
    );

    sinalizarEvento(2);
  }
}

// ================= ENVIO DOS DADOS =================

void enviarLeitura(const Leitura &leitura) {
  if (!ATIVAR_ESPNOW) {
    return;
  }

  if (envioPendente) {
    falhaComunicacao = true;

    Serial.println(
      "Falha no envio: aguardando conclusao do envio anterior."
    );

    sinalizarEvento(2);
    return;
  }

  if (!radioPronto) {
    radioPronto = iniciarRadio();
  }

  if (!radioPronto) {
    falhaComunicacao = true;

    Serial.println(
      "Radio indisponivel. Nova tentativa no proximo ciclo."
    );

    sinalizarEvento(2);
    return;
  }

  int tamanho = criarPacote(
    leitura,
    pacoteEmEnvio,
    sizeof(pacoteEmEnvio)
  );

  if (tamanho < 0 ||
      static_cast<size_t>(tamanho) >= sizeof(pacoteEmEnvio)) {
    falhaComunicacao = true;

    Serial.println(
      "Falha no envio: pacote excedeu o limite de 250 bytes."
    );

    sinalizarEvento(2);
    return;
  }

  envioPendente = true;

  esp_err_t erro = esp_now_send(
    MAC_RECEPTOR,
    reinterpret_cast<const uint8_t *>(pacoteEmEnvio),
    tamanho + 1
  );

  if (erro != ESP_OK) {
    envioPendente = false;
    falhaComunicacao = true;

    Serial.printf(
      "Falha no envio ESP-NOW: %s\n",
      esp_err_to_name(erro)
    );

    sinalizarEvento(2);

    if (erro == ESP_ERR_ESPNOW_NOT_INIT ||
        erro == ESP_ERR_ESPNOW_NOT_FOUND) {
      esp_now_deinit();
      radioPronto = false;
    }
  }
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);

  digitalWrite(PIN_TRIG, LOW);

  // Autoteste dos LEDs.
  digitalWrite(PIN_LED_VERDE, HIGH);
  digitalWrite(PIN_LED_VERMELHO, LOW);
  delay(350);

  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERMELHO, HIGH);
  delay(350);

  atualizarLeds();

  analogReadResolution(12);
  dht.begin();

  filaEnvio = xQueueCreate(
    1,
    sizeof(esp_now_send_status_t)
  );

  if (filaEnvio == nullptr) {
    Serial.println(
      "Erro: memoria insuficiente para a fila de envio."
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println();
  Serial.println(
    "ESP32-S3 - sensores e transmissao ESP-NOW"
  );

  Serial.println(
    "Aguardando 2 segundos para estabilizacao do DHT."
  );

  if (ATIVAR_ESPNOW) {
    radioPronto = iniciarRadio();
  } else {
    Serial.println("Modo local: ESP-NOW desativado.");
  }

  ultimoCiclo = millis();
}

// ================= LOOP =================

void loop() {
  tratarResultadoEnvio();
  atualizarLeds();

  uint32_t agora = millis();

  if (static_cast<uint32_t>(agora - ultimoCiclo) >=
      INTERVALO_LEITURA_MS) {
    ultimoCiclo = agora;

    Leitura leitura = lerSensores();

    alertaSensores = haAlertaSensores(leitura);

    exibirLeitura(leitura);
    enviarLeitura(leitura);
    atualizarLeds();
  }

  delay(1);
}
