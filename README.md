# Lâmpada inteligente com ESP32-S3

O projeto cobre a montagem, o firmware e o controle remoto da Etapa 1, além da API, do InfluxDB e da integração com o painel da Etapa 2. O Passo 2 está sob um título “Etapa 03” na página 2 do edital. Foi incluído na Etapa 1 conforme a introdução e a sequência dos passos.

## Etapa 1 · Montagem

A pinagem foi definida para uma ESP32-S3 DevKitC-1 com LED RGB externo, DHT11 e buzzer. Use os números GPIO impressos na placa. A configuração inicial usa RGB de cátodo comum e buzzer ativo. Se o seu kit for diferente, confira a pinagem e as opções em `LampadaInteligente.ino`. [Pinagem da DevKitC-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html)

| Componente | GPIO | Ligação |
| --- | --- | --- |
| Fotorresistor (LDR) | 4 | LDR entre 3V3 e GPIO 4. Resistor de 10 kΩ entre GPIO 4 e GND. A leitura sobe com a luz. |
| Potenciômetro de 10 kΩ | 5 | Terminais externos em 3V3 e GND, cursor no GPIO 5. |
| Sensor analógico de nível, opcional | 6 | Saída analógica no GPIO 6 com GND comum. Verifique que a saída fique entre 0 e 3,3 V. |
| DHT11 | 7 | VCC em 3V3, DATA (S/OUT) no GPIO 7 e GND comum. Confira a ordem dos pinos no seu módulo. Se ele não tiver pull-up, coloque um resistor de 10 kΩ entre DATA e 3V3. |
| Botão | 8 | Entre GPIO 8 e GND. O firmware usa pull-up interno, interrupção nas bordas e debounce de 40 ms. |
| LED RGB externo | 9, 10, 11 | R, G e B, respectivamente, cada canal com seu próprio resistor de 330 Ω. Para cátodo comum, ligue o comum ao GND. Para ânodo comum, ligue o comum a 3V3 e ajuste `RGB_ANODO_COMUM = true`. |
| Buzzer ativo | 12 | GPIO 12 ao resistor de base de 1 kΩ de um NPN, emissor ao GND e coletor ao terminal negativo do buzzer. Positivo na tensão indicada pelo buzzer, com GND comum. Para buzzer ativo magnético, use diodo de roda livre em paralelo, com cátodo no positivo. |
| Buzzer passivo | 12 | Configure `BUZZER_PASSIVO = true` para enviar um tom de 2,4 kHz. Use a alimentação e o circuito de acionamento indicados para o modelo do seu buzzer. |

O DHT11 fornece a temperatura e a umidade do painel. O GPIO 6 está reservado para um sensor analógico de nível de líquido. Se vocês não usam esse sensor, deixem o pino livre e `USAR_TANQUE = false`. Nesse caso, o firmware envia `nivel_tanque_pct: null`. Para instalar um sensor de saída analógica proporcional, meça os valores ADC com o tanque vazio e cheio, preencha `ADC_TANQUE_VAZIO` e `ADC_TANQUE_CHEIO` e habilite `USAR_TANQUE` no início de `LampadaInteligente.ino`.

## Etapa 1 · Gravação e teste local

1. Na Arduino IDE 2, instale o pacote **esp32 by Espressif Systems** da série **3.x**. Escolha **ESP32S3 Dev Module** para a DevKitC-1 e selecione a porta da placa.
2. No Gerenciador de Bibliotecas, instale **DHT sensor library** da Adafruit e **Adafruit Unified Sensor**. Abra `firmware/LampadaInteligente/LampadaInteligente.ino`, defina `MODO_TESTE_COMPONENTES` como `1` e grave o sketch. No Monitor Serial em **115200 baud**, observe o LDR, o potenciômetro, o botão e o DHT11. Envie `r`, `g` ou `b` para testar cada canal do RGB, `0` para apagar, `p` para testar o buzzer com nível alto e `t` para testar com um tom PWM. Cada teste do buzzer dura meio segundo. Se estiver usando a porta USB nativa do ESP32-S3, habilite **USB CDC On Boot** antes de gravar.
3. Volte `MODO_TESTE_COMPONENTES` para `0`, compile e grave o mesmo arquivo. Com a nuvem e a API desabilitadas, a lâmpada funciona em modo local.
4. Observe o ADC do LDR com o sensor exposto e coberto. Ajuste `LIMIAR_LUZ` entre esses valores. O valor inicial é 1700 na escala 0–4095, com histerese de 80. O valor representa a tensão relativa medida pelo ADC.

O modo de bancada aciona as saídas sem depender da temperatura, da luz ou do botão. Se os comandos forem confirmados no Serial e as saídas continuarem apagadas, confira o terminal comum do LED, os GPIOs e o GND entre a placa e o circuito do buzzer. No modo normal, a mensagem `Motivo do RGB OFF` explica quando a lógica do projeto mantém o LED apagado.

| Teste | Resultado esperado |
| --- | --- |
| Escuro, temperatura de 0 a 25 °C e sistema ligado | O LED acende. No modo automático, o brilho aumenta à medida que o ambiente escurece. |
| Claro, no modo automático | O LED apaga. |
| Gire o potenciômetro com o LED ligado | Vermelho, laranja, amarelo, branco, turquesa e azul, do menor ADC para o maior. |
| Temperatura fora da faixa com sensor ativo | O LED apaga. O buzzer toca enquanto o sistema e o buzzer estiverem habilitados. O Serial mostra “Perigo! Desligar!”. Em uma sala acima de 25 °C, esse é o comportamento esperado. |
| Botão pressionado uma vez | LED e buzzer apagam. Soltar o botão mantém o sistema desligado. |
| Botão pressionado outra vez | LED e buzzer voltam a obedecer ao modo anterior e às condições dos sensores. |
| DHT11 sem leitura válida, com sensor ativo | O Serial indica “sem leitura” e o LED fica bloqueado até haver uma leitura válida. |

## Arduino Cloud · Thing e painel

1. No [Arduino Cloud](https://app.arduino.cc/), em **Devices**, adicione um dispositivo de terceiros do tipo **ESP32** e selecione o modelo correspondente à sua ESP32-S3. Guarde o **Device ID** e a **Secret Key**. Crie uma **Thing** e associe esse dispositivo. [Cadastro de dispositivos](https://support.arduino.cc/hc/en-us/articles/360016495559-Add-a-device-to-Arduino-Cloud)
2. Na Thing, crie as cinco variáveis abaixo com a política **On change**. Use os nomes exatamente como na tabela. A combinação de cor e botão em `estado` permite usar as cinco variáveis disponíveis por Thing no plano gratuito. [Limites dos planos](https://cloud.arduino.cc/plans/)

   | Variável | Tipo | Permissão | Widget no painel |
   | --- | --- | --- | --- |
   | `temperatura` | Float | Read Only | Chart, em °C |
   | `umidade` | Float | Read Only | Chart, em % |
   | `luminosidade` | Integer | Read Only | Chart, em ADC de 0 a 4095 |
   | `estado` | String | Read Only | Messenger para cor, botão, sistema, buzzer e estado da telemetria |
   | `comando` | String | Read & Write | Messenger para enviar um comando |

3. Em **Dashboards**, crie **Lâmpada inteligente** e vincule cada widget à sua variável. Os widgets se atualizam automaticamente ao receber novos dados. [Configuração de widgets](https://docs.arduino.cc/arduino-cloud/cloud-interface/dashboard-widgets/)
4. Instale **ArduinoIoTCloud 2.10.0** com suas dependências na IDE. Em uma cópia privada de `LampadaInteligente.ino`, defina `USAR_ARDUINO_CLOUD` como `1` e preencha `WIFI_SSID`, `WIFI_SENHA`, `CLOUD_DEVICE_ID` e `CLOUD_DEVICE_KEY` no início do arquivo. Use uma rede Wi-Fi de 2,4 GHz com acesso à internet. Recompile e grave. Se o sketch ultrapassar o espaço da partição padrão em uma placa de 4 MB, selecione **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)** em **Partition Scheme**.
5. Com a Thing online, envie os comandos pelo Messenger vinculado a `comando`. Aguarde a caixa sincronizar vazia antes de repetir o mesmo comando.

| Comando | Ação |
| --- | --- |
| `Ligar` | Força a iluminação mesmo com ambiente claro, desde que o botão e a proteção térmica permitam. |
| `Desligar` | Apaga o LED e mantém o alarme térmico habilitado. |
| `Automático` | Restaura o controle da iluminação pelo LDR. |
| `Vermelho`, `Amarelo`, `Azul` | Troca por 1 s a cor de um LED aceso. Depois, a cor volta à posição atual do potenciômetro. |
| `Ativar Temperatura` / `Desativar Temperatura` | Liga/desliga a leitura do DHT11, incluindo umidade, e a regra térmica associada. |
| `Ativar Detector` / `Desativar Detector` | Liga/desliga a leitura do LDR. Com o detector desabilitado, o modo automático permite iluminação. |
| `Ativar Buzzer` / `Desativar Buzzer` | Habilita/silencia o aviso sonoro. O bloqueio térmico do LED continua ativo enquanto o DHT11 estiver habilitado. |

Antes da primeira leitura válida, os valores de inicialização no Cloud são −99 °C, −1% e −1 ADC. O widget `estado` indica quais leituras estão ausentes. Em caso de indisponibilidade, os gráficos mantêm o último ponto recebido. Cor e botão refletem o circuito em tempo real, sujeitos à conexão com a nuvem.

## Etapa 2 · InfluxDB e API

Na pasta que contém `compose.yaml`, com o Docker Desktop iniciado, crie o arquivo de configuração:

```powershell
if (-not (Test-Path -LiteralPath .env)) { Copy-Item -LiteralPath .env.example -Destination .env }
notepad .env
python -c "import secrets; print(secrets.token_hex(32))"
```

1. Em `.env`, preencha `INFLUX_ADMIN_PASSWORD` (mínimo de 8 caracteres) e gere dois valores diferentes com o comando acima, um para `INFLUX_ADMIN_TOKEN` e outro para `API_TOKEN`. Mantenha `INFLUX_API_TOKEN` em branco neste passo.
2. Rode `docker compose up -d influxdb`. A configuração inicial cria a organização `laboratorio` e o bucket `lampada_iot` com retenção de **30 dias**. Os volumes mantêm as leituras e a configuração entre reinicializações. [Configuração do InfluxDB em Docker](https://docs.influxdata.com/influxdb/v2/install/?t=Docker)
3. Em `http://127.0.0.1:8086`, entre com o usuário e a senha de `.env`. Vá a **Load Data → API Tokens → Generate API Token → Custom API Token**. Marque leitura e escrita apenas para `lampada_iot`. Copie o token na hora e salve em `INFLUX_API_TOKEN`. O ESP32 usa o `API_TOKEN` da API, separado dos tokens do banco. [Permissões de token](https://docs.influxdata.com/influxdb/v2/security/tokens/)
4. Para o ESP32 alcançar a API, rode `ipconfig` e informe em `API_BIND_IP` o IPv4 do computador na rede local. Deixe o ESP32 e o computador na mesma rede e permita TCP 8000 no firewall da rede privada, se necessário. Para teste só no computador, mantenha `127.0.0.1`.
5. Rode `docker compose up -d --build api`. Confira os serviços com `docker compose ps` e a API com `Invoke-RestMethod http://IP_DO_COMPUTADOR:8000/health`.
6. Instale **ArduinoJson 7.x** na IDE. No início de `LampadaInteligente.ino`, mantenha o Wi-Fi configurado, defina `USAR_API` como `1`, ajuste `API_URL` para `http://IP_DO_COMPUTADOR:8000/api/v1/leituras` e copie o `API_TOKEN` de `.env`. Recompile e grave. Use HTTP somente na rede privada do laboratório. Para acesso por rede pública, disponibilize a API por HTTPS e configure `API_CA_PEM` com a CA do servidor.

Se o log mostrar `missing parameter` para `DOCKER_INFLUXDB_INIT_PASSWORD`, preencha `INFLUX_ADMIN_PASSWORD` em `.env` e salve o arquivo na mesma pasta de `compose.yaml`. Confira se o nome é exatamente `.env`. No PowerShell, execute nesta pasta:

```powershell
docker compose --env-file .env up -d --force-recreate influxdb
docker compose --env-file .env ps -a influxdb
docker compose --env-file .env logs --tail 30 influxdb
```

Quando o serviço estiver saudável, abra `http://127.0.0.1:8086`. O comando recria o contêiner com as variáveis atualizadas e reutiliza os volumes do banco.

O ESP32 sincroniza o relógio por NTP e envia leituras a cada 15 s. A API grava a leitura no InfluxDB e devolve a amostra consultada no bucket. Com a API habilitada, os três gráficos no Arduino Cloud recebem os valores confirmados pelo banco. O widget `estado` informa quando a última confirmação tem mais de 45 s. A montagem continua operando localmente durante quedas de rede. O ESP32 tenta reenviar uma leitura uma vez em caso de falha transitória.

### Consultar e conferir

| Método e rota | Resposta |
| --- | --- |
| `GET /health` | `200` se a API estiver rodando. |
| `POST /api/v1/leituras` | `201` com a amostra recuperada do banco. |
| `GET /api/v1/leituras/lampada-s3-01/ultima` | `200` com a amostra mais recente da janela de 24 h. |

Use `Authorization: Bearer <API_TOKEN>` nas rotas de leituras. A medição no InfluxDB é `lampada`, com a tag `dispositivo`. O timestamp enviado é Unix em segundos UTC. Temperatura, umidade, luminosidade e nível são opcionais como valores e retornam `null` quando indisponíveis. O banco também armazena ADC do potenciômetro, brilho, cor, botão, estados do sistema e dos atuadores e habilitação dos sensores.

Para testar a gravação antes de conectar o ESP32, use este exemplo no PowerShell. A amostra fica sob o identificador `teste-lab`.

```powershell
$api = 'http://127.0.0.1:8000' # Use API_BIND_IP se você tiver alterado o endereço.
$cabecalho = @{Authorization = 'Bearer COLE_O_API_TOKEN'}
$dados = @{
  dispositivo = 'teste-lab'
  timestamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  temperatura_c = 23.5
  umidade_pct = 57.2
  luminosidade_adc = 980
  nivel_tanque_pct = $null
  potenciometro_adc = 1900
  brilho_pct = 62
  cor = 'Amarelo'
  botao_pressionado = $false
  sistema_ligado = $true
  led_ligado = $true
  buzzer_ligado = $false
  temperatura_ativa = $true
  detector_ativo = $true
  buzzer_ativo = $true
  alerta_temperatura = $false
}
Invoke-RestMethod "$api/api/v1/leituras" -Method Post -Headers $cabecalho -ContentType 'application/json' -Body ($dados | ConvertTo-Json)
Invoke-RestMethod "$api/api/v1/leituras/teste-lab/ultima" -Headers $cabecalho
```

Após gravar a placa, consulte o identificador `lampada-s3-01` com o mesmo `GET`. No **Data Explorer** do InfluxDB, selecione o bucket e a medição e filtre pela tag do dispositivo para ver o histórico. No Arduino Cloud, confira se os gráficos passam a receber pontos e se o widget de estado mostra `Banco: sincronizado`.
