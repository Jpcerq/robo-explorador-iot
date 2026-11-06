# Robô explorador — controle Velxio, ESP32 físico e telemetria

Exploration robot built for an IoT course at SENAI CIMATEC. A simulated ESP32-S3 controller publishes commands over MQTT to a physical ESP32-S3.
The physical board drives a servo, reads temperature, humidity, light and distance sensors, and sends telemetry over HTTP to a Flask API backed by SQLite.
Servo control and telemetry delivery were validated on hardware. The rest of this document is in Portuguese.

**Team:** João Pedro Cerqueira Guimarães and Pedro Medeiros Prietto

```mermaid
flowchart LR
    subgraph Sim[Velxio simulator]
        C[ESP32-S3 controller<br/>joystick and button]
    end
    B[(MQTT broker<br/>HiveMQ)]
    subgraph Robot[Physical robot]
        E[ESP32-S3]
        SV[Servo]
        SN[DHT11, ultrasonic, LDR]
    end
    subgraph Backend[Computer on the local network]
        F[Flask API]
        D[(SQLite)]
    end

    C -- commands --> B
    B -- commands --> E
    E -- status --> B
    E -- PWM --> SV
    SN -- readings --> E
    E -- HTTP POST /leituras --> F
    F --> D
```

Projeto das etapas 01, 02 e 03: o controle simulado no Velxio publica comandos MQTT; o ESP32-S3 físico aciona o servo, lê os sensores e envia as leituras para uma API Flask com SQLite.

## Arquivos que devem ser usados

| Arquivo | Onde usar |
| --- | --- |
| `sketch.ino` | Editor do ESP32-S3 virtual no Velxio. É uma cópia do controle em `firmware/controle_velxio/controle_velxio.ino`. |
| `diagram.json` | Diagrama do controle simulado já existente no repositório. |
| `libraries.txt` | Biblioteca MQTT do controle simulado. |
| `firmware/controle_velxio/controle_velxio.ino` | Sketch completo do controle virtual, organizado em pasta Arduino. |
| `firmware/robo_fisico/robo_fisico.ino` | Sketch completo do robô físico enviado e confirmado pelo responsável pelo projeto. |
| `backend/app.py` | API HTTP que grava e consulta as leituras. |
| `backend/schema.sql` | Estrutura da tabela SQLite. |
| `backend/resetar_leituras.py` | Limpeza manual dos registros para novos testes. |

Use um sketch completo em cada ESP32. Estas versões configuram os dados diretamente no código e não precisam de `secrets.h`. A cópia local do firmware físico mantém as configurações enviadas pelo responsável pelo projeto. A versão pública no GitHub usa valores de exemplo para Wi-Fi, URL da API e CallMeBot: preencha esses campos antes de carregar uma cópia baixada do repositório.

## Comunicação

```text
Controle Velxio -> MQTT broker.hivemq.com:1883 -> ESP32 físico -> servo GPIO 9
                                                    |
                                                    +-> HTTP POST /leituras -> Flask -> SQLite
```

Ambos os ESP32 usam o tópico `cimatec/iot/monitorargado/robo/cmd`. O físico publica seu estado em `cimatec/iot/monitorargado/robo/status`. O controle desta versão não assina o estado do físico: seus LEDs indicam o estado ligado/desligado selecionado no próprio controle.

O controle reenvia movimentos a cada 500 ms. O físico possui um failsafe de 1,5 s: na ausência de novos comandos de movimento, solicita a posição central do servo.

## Iniciar o backend no Windows

Abra um terminal na pasta `backend`:

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe app.py
```

A API escuta em `0.0.0.0:5000` e cria `backend/leituras.db` automaticamente. Mantenha o terminal aberto durante o uso.

No computador do backend, abra:

- `http://127.0.0.1:5000/health`: estado do serviço.
- `http://127.0.0.1:5000/leituras`: até 100 registros mais recentes.
- `http://127.0.0.1:5000/leituras?limit=10`: até 10 registros mais recentes.

O limite de 100 restringe a quantidade retornada pela consulta; os registros anteriores continuam armazenados no banco. A variável de ambiente `LEITURAS_DB` permite alterar o caminho do banco.

## Carregar o ESP32 físico

Abra `firmware/robo_fisico/robo_fisico.ino` na Arduino IDE. Use ESP32-S3 com o pacote Arduino-ESP32 3.x, compatível com `ledcAttach` e `ledcWrite` por GPIO. Instale as bibliotecas `PubSubClient`, `DHT sensor library` (Adafruit, com suas dependências) e `UrlEncode`.

As configurações `WIFI_SSID`, `WIFI_PASS`, `API_URL`, `CALLMEBOT_PHONE` e `CALLMEBOT_API_KEY` ficam no início do sketch. Configure esses campos na versão pública; a cópia local já configurada mantém os valores do arquivo enviado. Se mudar de rede ou o IPv4 do computador mudar, atualize `API_URL`, por exemplo:

```cpp
#define API_URL "http://IP_DO_COMPUTADOR:5000/leituras"
```

Use o IPv4 do computador consultado com `ipconfig`. O ESP32 físico precisa alcançar esse computador pela rede local. Confira as permissões do Firewall do Windows para a porta 5000 e evite isolamento entre clientes Wi-Fi. No ESP32 físico, `localhost` aponta para a própria placa.

| Componente físico | GPIO |
| --- | --- |
| LED verde | 15 |
| LED vermelho | 16 |
| DHT11 | 17 |
| Ultrassônico TRIG / ECHO | 18 / 8 |
| LDR | 7 |
| Sinal do servo | 9 |
| Eixo X do joystick local | 4 |
| Botão de emergência local | 5 |

O acionamento do servo pelo eixo do joystick físico está desativado com `HABILITAR_JOYSTICK_LOCAL_SERVO = false`, para evitar que ele sobrescreva os comandos MQTT do Velxio. O botão local continua funcionando por interrupção.

O servo usa PWM a 50 Hz. O alvo é 0° para `Esquerda`, 180° para `Direita` e 90° para `Frente`, `Tras` e `PARADO`. `DESLIGAR` desativa o PWM; `LIGAR` solicita o centro. A posição exibida no Serial é o alvo solicitado, sem medição da posição mecânica do servo.

O firmware lê sensores a cada 2 s. Uma distância ultrassônica válida de até 50 cm determina `presenca = 1`. Não há PIR nessa versão. Leituras inválidas do DHT são ignoradas; as válidas seguem por HTTP para a API. A fila tem 60 posições em RAM, além da amostra em envio, e tenta novamente em caso de falha. Reiniciar a placa perde a fila; quando ela enche, a amostra mais antiga ainda na fila é descartada.

CallMeBot é opcional e permanece desativado se telefone ou chave estiverem vazios.

## Executar o controle Velxio

No circuito virtual ESP32-S3, copie o conteúdo inteiro de `sketch.ino`, use o `diagram.json` do controle e instale `PubSubClient` se necessário. O Wi-Fi do controle está configurado como `Velxio-GUEST`, sem senha. Inicie a simulação com o físico conectado ao broker.

Projeto do controle indicado no repositório original: https://velxio.dev/project/2fd814c2-42d4-47c3-8b46-2e90d503c5b9

| Componente virtual | GPIO |
| --- | --- |
| Joystick X / Y / SEL | 4 / 5 / 6 |
| Botão ligar/desligar | 14 |
| LED verde / vermelho | 15 / 16 |

Os comandos são `Frente`, `Tras`, `Esquerda`, `Direita`, `PARADO`, `LIGAR` e `DESLIGAR`. O joystick envia direções discretas; o eixo vertical tem prioridade sobre o horizontal nas diagonais. Não há transmissão proporcional de ângulo nessa versão. O pino SEL está configurado, mas não dispara uma ação no controle virtual.

## Conferir o funcionamento

Abra os monitores seriais a 115200 baud. No Velxio, `MQTT enviado: Direita` se repete enquanto o joystick permanece nessa direção. No físico, `[MQTT] Recebido=Direita` confirma a chegada; `[SERVO] Alvo=180 graus` informa a aplicação de uma nova posição solicitada. Um erro de configuração ou de escrita de PWM é exibido explicitamente.

`Telemetria salva (HTTP 201)` confirma a gravação de uma amostra. Atualize a página `/leituras` para consultar os registros. Pelo celular ou outro computador, substitua `127.0.0.1` pelo IPv4 do computador do backend.

## API

`POST /leituras` recebe `Content-Type: application/json`:

```json
{
  "temperatura_c": 24.3,
  "umidade_pct": 55,
  "luminosidade": 723,
  "presenca": 1,
  "probabilidade_vida": 78
}
```

O campo `timestamp` é opcional e aceita um horário ISO 8601. Sem ele, a API usa o horário UTC de recebimento. Uma gravação retorna HTTP 201; dados inválidos retornam HTTP 400. `GET /leituras?limit=N` aceita de 1 a 100 registros e os devolve em ordem cronológica dentro do lote mais recente.

## Zerar leituras para um novo teste

Para começar sem amostras antigas da fila, desligue o ESP32 físico e pare o Flask com `Ctrl+C`. Na pasta `backend`, execute:

```powershell
.\.venv\Scripts\python.exe resetar_leituras.py --confirmar
```

O script apaga os registros do banco existente e mantém a tabela. Os IDs continuam a sequência anterior. Inicie o Flask novamente e religue o ESP32. O arquivo de banco, ambientes virtuais e caches ficam fora do Git.

## Validação

O responsável pelo projeto confirmou em hardware o controle do servo com o firmware físico enviado e os envios HTTP 201 a cada 2 s. A cópia local mantém esse arquivo físico integralmente; a cópia pública substitui apenas os valores de configuração por exemplos. O diagrama do controle foi preservado do repositório. Nenhuma nova compilação ou execução de firmware foi realizada durante a organização dos arquivos.
