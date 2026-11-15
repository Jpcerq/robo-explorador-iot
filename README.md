# Lâmpada inteligente com ESP32-WROOM

Lâmpada inteligente IoT da disciplina **Práticas Integradas: Camada de Aplicação**. Um ESP32-WROOM lê temperatura, umidade, luminosidade, potenciômetro e botão e controla um LED RGB e um buzzer. As leituras são gravadas em um banco **InfluxDB** através de uma API, e a lâmpada pode ser monitorada e controlada remotamente por um painel no **Arduino IoT Cloud**.

```
ESP32-WROOM ──HTTP (a cada 15 s)──► API (servidor.py, porta 8000) ──► InfluxDB (bucket lampada_iot)
     │
     └──MQTT──► Arduino IoT Cloud (Thing + Dashboard) ◄── comandos do usuário
```

## Estrutura do repositório

| Caminho | Conteúdo |
| --- | --- |
| `firmware/LampadaInteligenteWROOM/LampadaInteligenteWROOM.ino` | Firmware do ESP32-WROOM (Arduino-ESP32 3.x). |
| `backend/servidor.py` | API HTTP que recebe as leituras, valida e grava no InfluxDB. |
| `backend/iniciar_banco.bat` e `backend/iniciar_banco.py` | Sobe o InfluxDB e a API no Windows **sem Docker**. |
| `compose.yaml`, `backend/Dockerfile`, `.env.example` | Alternativa com **Docker** para subir o InfluxDB e a API. |

## 1. Como montar o projeto

### Componentes

ESP32-WROOM, sensor DHT22 (temperatura e umidade), fotorresistor (LDR) com resistor de 10 kΩ, potenciômetro de 10 kΩ, botão, buzzer ativo e LED RGB de cátodo comum com três resistores de 330 Ω.

### Ligações

| Componente | GPIO | Ligação |
| --- | --- | --- |
| LDR | 34 | LDR entre 3V3 e GPIO 34; resistor de 10 kΩ entre GPIO 34 e GND. Leitura menor = ambiente mais escuro. |
| Potenciômetro | 36 | Extremidades em 3V3 e GND, cursor no GPIO 36. |
| DHT22 | 32 | VCC em 3V3, DATA no GPIO 32, GND comum. Resistor de 10 kΩ entre DATA e 3V3 se o módulo não tiver pull-up. |
| Botão | 17 | Entre GPIO 17 e GND (pull-up interno, interrupção e debounce de 50 ms). |
| Buzzer | 14 | GPIO 14 no buzzer ativo (use um transistor se o buzzer consumir mais que o GPIO fornece). |
| LED RGB | 25, 26, 27 | Canais R, G e B, cada um com resistor de 330 Ω. Comum no GND. Para ânodo comum, ligue o comum em 3V3 e use `LED_ANODO_COMUM = true`. |

Os GPIOs 34 e 36 são do ADC1, que continua funcionando com o Wi-Fi ligado.

### Funcionamento

| Situação | Comportamento |
| --- | --- |
| Ambiente escuro e temperatura na faixa | LED RGB acende. |
| Ambiente claro (modo automático) | LED RGB apaga. |
| Temperatura fora da faixa (`TEMPERATURA_MIN`/`TEMPERATURA_MAX`) | LED apaga, buzzer toca e o Serial mostra "Perigo! Desligar!". |
| Potenciômetro | Muda a cor: vermelho → laranja → amarelo → branco → turquesa → azul. |
| Botão | Desliga LED e buzzer; pressionar de novo reativa. |

O Monitor Serial (115200 baud) mostra o estado do LED, a temperatura, o LDR, o potenciômetro, o buzzer e o sistema a cada 2 s.

## 2. Firmware

1. Na Arduino IDE 2, instale a placa **esp32 by Espressif Systems** (série 3.x) e selecione **ESP32-WROOM-DA Module** (ou ESP32 Dev Module).
2. Instale as bibliotecas **DHT sensor library** (Adafruit), **Adafruit Unified Sensor**, **ArduinoJson 7.x** e **ArduinoIoTCloud** (aceite instalar as dependências).
3. Em **Tools → Partition Scheme**, escolha **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**. Com o Cloud e a API, o programa não cabe na partição padrão.
4. Preencha a configuração no início do `.ino` e grave:

| Constante | Valor |
| --- | --- |
| `USAR_ARDUINO_CLOUD` / `USAR_API` | `1` para ligar o Cloud e o envio ao banco; `0` para rodar só localmente. |
| `WIFI_SSID` / `WIFI_SENHA` | Rede **2,4 GHz** (o ESP32 não conecta em 5 GHz). |
| `CLOUD_DEVICE_ID` / `CLOUD_DEVICE_KEY` | Device ID e Secret Key do dispositivo no Arduino Cloud. |
| `API_URL` | `http://IP_DO_PC:8000/api/v1/leituras` (IPv4 do computador que roda a API). |
| `API_TOKEN` | Token da API gerado pelo backend. |
| `LIMIAR_LUZ` | Valor do LDR entre "claro" e "escuro" (escala 0–4095). Calibre observando o Serial. |

> **Não envie senha do Wi-Fi, token da API nem Secret Key para o GitHub.** O arquivo do repositório fica com esses campos vazios.

### Comandos

Pelo Monitor Serial ou pelo Messenger `comandoCloud` do painel:

`Ligar`, `Desligar`, `Automatico`, `Vermelho`, `Amarelo`, `Azul` (cor por 1 s), `Ativar Temperatura`, `Desativar Temperatura`, `Ativar Detector`, `Desativar Detector`, `Ativar Buzzer`, `Desativar Buzzer`.

## 3. Como rodar o backend (API + InfluxDB)

### Opção A — Windows, sem Docker (recomendada)

Requer Python 3.9+ instalado.

1. Dê dois cliques em `backend/iniciar_banco.bat`.
2. Na primeira execução, o script:
   - baixa o InfluxDB 2.9.1 oficial (≈ 51 MB) e confere o SHA-256;
   - cria a organização `laboratorio`, o bucket `lampada_iot` com retenção de **30 dias**, um token com leitura/escrita apenas nesse bucket e o token da API;
   - inicia o InfluxDB (`127.0.0.1:8086`) e a API (porta 8000) e testa uma gravação.
3. O login do painel do InfluxDB, a `API_URL` e o `API_TOKEN` para o sketch ficam em `backend/ACESSO_BANCO.txt` (arquivo local, ignorado pelo Git). O andamento fica em `backend/iniciar_banco.log`.
4. Se o Windows perguntar, permita o Python nas **redes privadas** (o ESP32 precisa acessar a porta 8000).

Mantenha abertas as janelas do `influxd` e do `servidor.py`. Depois de reiniciar o PC, rode o `iniciar_banco.bat` de novo: ele reaproveita a configuração e os dados.

### Opção B — Docker

1. Copie `.env.example` para `.env`, preencha `INFLUX_ADMIN_PASSWORD` e gere `INFLUX_ADMIN_TOKEN` e `API_TOKEN` com `python -c "import secrets; print(secrets.token_hex(32))"`.
2. `docker compose up -d influxdb`
3. Em `http://127.0.0.1:8086`, crie um token de leitura/escrita para `lampada_iot` e coloque em `INFLUX_API_TOKEN`. Ponha o IPv4 do PC em `API_BIND_IP`.
4. `docker compose up -d --build api`

### Rotas da API

| Rota | Resposta |
| --- | --- |
| `GET /health` | `200` se a API estiver no ar. |
| `POST /api/v1/leituras` | `201` com a leitura gravada e lida de volta do InfluxDB. |
| `GET /api/v1/leituras/<dispositivo>/ultima` | `200` com a leitura mais recente das últimas 24 h. |

As rotas de leituras exigem o cabeçalho `Authorization: Bearer <API_TOKEN>`.

## 4. Banco de Dados

O banco é o **InfluxDB 2**, um banco de séries temporais.

| Item | Valor |
| --- | --- |
| Organização | `laboratorio` |
| Bucket | `lampada_iot` (retenção de 30 dias) |
| Medição | `lampada` |
| Tag | `dispositivo` (o ESP32 usa `lampada-wroom-01`) |
| Timestamp | Unix em segundos UTC, sincronizado por NTP no ESP32 |
| Autenticação | Token só do bucket para a API; o ESP32 usa um token separado da API |

| Campo | Tipo | Descrição |
| --- | --- | --- |
| `temperatura_c` | float | Temperatura do DHT22 (°C). |
| `umidade_pct` | float | Umidade do DHT22 (%). |
| `luminosidade_adc` | int | Leitura do LDR (0–4095). |
| `potenciometro_adc` | int | Leitura do potenciômetro (0–4095). |
| `brilho_pct` | int | 100 com o LED aceso, 0 apagado. |
| `cor` | string | Apagado, Vermelho, Laranja, Amarelo, Branco, Turquesa ou Azul. |
| `botao_pressionado`, `sistema_ligado`, `led_ligado`, `buzzer_ligado` | bool | Estado do botão e dos atuadores. |
| `temperatura_ativa`, `detector_ativo`, `buzzer_ativo` | bool | Sensores e buzzer habilitados ou não. |
| `alerta_temperatura` | bool | Temperatura fora da faixa. |

Campos sem leitura válida (sensor desativado ou com falha) não são gravados.

## 5. Como consultar os dados salvos

**Painel do InfluxDB:** abra `http://127.0.0.1:8086`, entre com o login do `ACESSO_BANCO.txt` e vá em **Data Explorer**. Escolha o bucket `lampada_iot`, a medição `lampada` e o dispositivo `lampada-wroom-01`, ou cole no **Script Editor**:

```flux
from(bucket: "lampada_iot")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "lampada" and r.dispositivo == "lampada-wroom-01")
  |> filter(fn: (r) => r._field == "temperatura_c" or r._field == "umidade_pct" or r._field == "luminosidade_adc")
```

Ligue **View Raw Data** para ver a tabela com uma linha por leitura.

**Pela API (PowerShell):**

```powershell
$h = @{Authorization = "Bearer SEU_API_TOKEN"}
Invoke-RestMethod "http://127.0.0.1:8000/api/v1/leituras/lampada-wroom-01/ultima" -Headers $h
```

**Grafana (opcional):** adicione uma fonte de dados InfluxDB com linguagem **Flux**, URL `http://localhost:8086`, organização `laboratorio`, bucket padrão `lampada_iot` e um token de leitura do bucket. A mesma consulta Flux acima pode ser usada nos painéis.

## 6. Arduino IoT Cloud

1. Em **Devices → Add Device → Compatible device**, escolha **ESP32 → ESP32-WROOM-DA Module** e guarde o Device ID e a Secret Key.
2. Crie uma **Thing**, associe o dispositivo e crie as variáveis com estes nomes exatos (política **On change**):

| Variável | Tipo | Permissão | Widget |
| --- | --- | --- | --- |
| `cloudTemperatura` | Float | Read Only | Chart |
| `cloudUmidade` | Float | Read Only | Chart |
| `cloudLuminosidade` | Integer | Read Only | Chart |
| `cloudEstado` | Character String | Read Only | Messenger (LED, cor, botão, sistema e status do banco) |
| `comandoCloud` | Character String | Read & Write | Messenger (envio de comandos) |

3. Em **Dashboards**, crie um painel e use **Add → Things** para gerar os widgets das variáveis. Os widgets se atualizam automaticamente.
4. Preencha `CLOUD_DEVICE_ID` e `CLOUD_DEVICE_KEY` no sketch, com `USAR_ARDUINO_CLOUD 1`, e grave.

O widget `cloudEstado` mostra `Banco: OK` quando a última leitura foi confirmada pelo InfluxDB nos últimos 45 s.
