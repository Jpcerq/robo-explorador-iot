# Etapa 03 - Banco de Dados e Grafana

Implementação da Etapa 03 da atividade de pintura de blocos de madeira (páginas 5 e 6 do edital). Integra o **receptor e o transmissor fornecidos**, com comunicação USB/Serial escolhida por você.

```text
ESP32-S3 / sensores
       | ESP-NOW, canal 1, a cada 2 s
ESP32-WROOM-32 / receptor existente
       | USB / Serial a 115200 baud: Conteudo: {JSON}
ponte_serial.py no computador
       | POST /leituras com X-API-Key
API Python (FastAPI)
       | gravação confirmada, token de escrita
InfluxDB 2 / bucket pintura / retenção de 30 dias
       | consultas Flux, token de leitura
Grafana / indicadores e gráficos / atualização a cada 2 s
```

## O que já está implementado

- API autenticada para receber e validar as leituras, preservando `null` nas falhas dos sensores.
- Bucket `pintura`, organização `fabrica`, retenção de 30 dias e volumes persistentes.
- Inicialização automática com tokens distintos: escrita no bucket para API e leitura para Grafana. O token administrador fica somente no InfluxDB e no job de configuração.
- Ponte compatível com o texto `Conteudo: {...}` do receptor atual. Os demais logs são ignorados.
- Fila local SQLite: a ponte só apaga uma leitura após a API confirmar a escrita no InfluxDB. Falhas de rede mantêm a leitura para nova tentativa. Pacotes inválidos ficam em quarentena.
- Dashboard provisionado automaticamente, com 15 painéis e atualização a cada 2 segundos: nível, temperatura, umidade, luminosidade, presença, tempo desde a última recepção, falhas de sensores, sequência e uptime.
- Simulador separado dos dados reais, testes automatizados e verificação da instalação completa.

SQLite é apenas a fila de transporte no PC. **As leituras do projeto são armazenadas no InfluxDB**, como pede a atividade. Nenhuma parte das etapas 01, 02, 04 ou 05 foi implementada aqui; a integração usa os firmwares existentes.

## Limites identificados nos códigos fornecidos

**Presença:** o transmissor não tem leitura de PIR nem o campo `presenca`. A integração aceita opcionalmente `presenca: 0` ou `presenca: 1`, e o Grafana já tem indicador e histórico. Com o firmware atual, mostra **Sem leitura**, nunca ausência fictícia. Para demonstrar presença física, esse dado precisa vir do firmware do coletor; isso permanece uma pendência da coleta na Etapa 01. O simulador permite demonstrar o painel com dados artificiais identificados.

**Timestamp:** o campo `timestamp` enviado é `esp_timer_get_time()/1000`: tempo desde o boot, **não** data/hora Unix ou ISO. A ponte preserva esse valor em `uptime_ms`, adiciona `recebido_em` em ISO 8601 UTC e o utiliza como tempo do ponto no InfluxDB. Os gráficos representam **a hora da recepção no PC**, não a hora exata da coleta. O relógio do PC deve estar correto. Para obter a hora ISO original da coleta exigida no exemplo do edital, o transmissor precisa de um relógio sincronizado; a ponte não consegue recuperá-la apenas do uptime.

**Luminosidade:** 0 a 4095 é ADC bruto; não é uma medida calibrada em lux.

**Recepção:** o receptor original mantém apenas o último pacote em um buffer. A fila protege os dados **já lidos pelo computador**. Não recupera perda de rádio, pacote sobrescrito no receptor ou dados enviados enquanto a ponte estava fechada.

## 1. Preparar no Windows

Instale **Python 3.12** e **Docker Desktop com containers Linux**. Deixe o Docker Desktop aberto. Extraia este projeto para uma pasta local e abra o PowerShell nela, onde estão `compose.yaml` e este README.

**Se o ambiente `.venv` já foi criado neste computador, não precisa criá-lo de novo.** Confira com `.\.venv\Scripts\python.exe --version` e siga para a instalação das dependências. O comando `py -3.12` exige especificamente Python 3.12 registrado no launcher; ter outra versão do Python não satisfaz esse comando. Se a criação do ambiente falhar, corrija esse primeiro erro antes de executar os próximos comandos.

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe configurar.py
docker compose up -d --build
docker compose ps -a
```

O script gera `.env` com senhas e chave aleatórias. Não sobrescreve um `.env` existente. O primeiro build precisa de internet para baixar as imagens e dependências.

Aguarde `influxdb` ficar saudável, `preparar-tokens` terminar com código 0 e `api`/`grafana` iniciarem. O job `preparar-tokens` aparecer encerrado: esse é seu estado esperado.

Os serviços são publicados somente em `127.0.0.1`, pois a ponte roda no mesmo PC. Não precisa abrir porta no roteador nem mudar o canal ESP-NOW.

| Serviço | Endereço | Acesso |
|---|---|---|
| API e documentação interativa | http://localhost:8000/docs | Use `Authorize` quando disponível ou preencha `x-api-key` no endpoint com `API_KEY` do `.env` |
| InfluxDB | http://localhost:8086 | `INFLUXDB_USERNAME` e `INFLUXDB_PASSWORD` do `.env` |
| Grafana | http://localhost:3001 | `GRAFANA_USERNAME` e `GRAFANA_PASSWORD` do `.env` |
| Dashboard | http://localhost:3001/d/pintura-etapa03 | Após login no Grafana |

O bucket, os tokens, a fonte de dados e o dashboard são criados automaticamente. **Não use InfluxDB 3 ou troque a imagem por `latest` neste projeto:** as consultas entregues usam InfluxDB 2 com Flux.

A imagem está fixada em `influxdb:2.9.1`; a 2.9.0 apresenta uma falha no setup inicial. O Grafana usa a porta local **3001** para evitar conflito com outros programas na 3000. Se precisar de outra porta, defina `GRAFANA_PORT` no `.env` e execute novamente `docker compose up -d`.

## 2. Conectar ao seu receptor

1. Mantenha seus dois firmwares atuais. Não precisa alterar a linha `Serial.print("Conteudo: ")` do receptor.
2. Conecte o **ESP32-WROOM-32 receptor** ao PC por USB.
3. **Feche o Monitor Serial da Arduino IDE/VS Code** para liberar a porta. Apenas um programa pode usá-la por vez.
4. Confirme a porta no Gerenciador de Dispositivos ou com o comando abaixo. Na captura fornecida ela era COM6.

```powershell
.\.venv\Scripts\python.exe -m serial.tools.list_ports
.\.venv\Scripts\python.exe ponte_serial.py --porta COM6
```

Mantenha esse terminal aberto. A cada pacote devem aparecer mensagens de `RX sequencia=... registrada na fila` e `confirmada no InfluxDB`.

A abertura da porta pode reiniciar algumas placas; aguarde o rádio iniciar. A ponte reconecta automaticamente se o cabo for desconectado e tenta reenviar a fila mesmo enquanto a Serial está indisponível. Encerre com Ctrl+C.

## 3. Ver o Grafana

Abra o dashboard e selecione **ESP32 real** no filtro **Dispositivo**. O intervalo inicial é dos últimos 15 minutos e a atualização é de 2 segundos.

- O nível de tinta fica vermelho abaixo de 20% e verde a partir de 20%.
- Os indicadores atuais usam o último pacote recebido nos últimos 10 segundos. Se o último pacote traz um sensor `null`, seu indicador fica sem leitura em vez de reutilizar um valor antigo.
- `Última recepção há` mostra o atraso, em segundos, e fica vermelho a partir de 5 segundos.
- `Falhas no último pacote` conta os `null` de nível, temperatura e umidade.
- Os históricos permitem comparar a evolução do tanque, temperatura/umidade, luminosidade e presença.
- O firmware atual não envia presença: **Sem leitura / Não enviado** é o resultado esperado.

O painel de histórico usa agregação adequada à largura da tela; presença usa o último valor de cada intervalo, nunca média. O horário é apresentado no fuso do navegador. Dados continuam salvos em UTC no banco.

O dashboard é gerenciado pelo arquivo `grafana/dashboards/pintura.json`. Para mudanças permanentes, altere esse arquivo; o provisionamento pode sobrescrever mudanças feitas apenas na interface.

## 4. Demonstrar antes de ligar os sensores

Com os containers iniciados:

```powershell
.\.venv\Scripts\python.exe simular.py --quantidade 60 --com-presenca
```

Abra o Grafana e selecione **Demonstração** no filtro. São dados artificiais gravados como `esp32-s3-demo`, incluindo presença alternada para exercitar seu painel. Eles não se misturam aos dados do filtro **ESP32 real**.

Sem `--com-presenca`, o simulador reproduz a ausência desse campo no firmware original.

## 5. Verificar a instalação completa

```powershell
.\.venv\Scripts\python.exe verificar_instalacao.py
```

Esse script precisa dos containers funcionando. Ele insere **um ponto de demonstração**, confirma a persistência consultando o InfluxDB, confere a retenção, verifica a fonte de dados e o dashboard no Grafana e executa as consultas dos painéis. Caso execute os simuladores simultaneamente, pare-os para que outro ponto de demonstração não substitua o último pacote durante essa checagem.

Para comprovar persistência após reiniciar os serviços, pare a geração de dados, execute `docker compose restart` e consulte novamente o histórico. Reiniciar os containers não remove os volumes.

## 6. Consultar as leituras no InfluxDB

No InfluxDB, abra o **Data Explorer**, use o editor Flux e execute:

```flux
from(bucket: "pintura")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "pintura")
  |> filter(fn: (r) => r.dispositivo == "esp32-s3-fabrica")
  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
  |> sort(columns: ["_time"], desc: true)
  |> limit(n: 20)
```

| Elemento | Valor / significado |
|---|---|
| Organização / bucket / measurement | `fabrica` / `pintura` / `pintura` |
| Retenção | 30 dias, configurada na primeira inicialização do banco |
| `_time` | Hora UTC de chegada ao computador; precisão de microssegundos |
| Tags | `dispositivo`, `relogio=uptime_ms`, `origem_tempo=recepcao_pc` |
| Campos dos sensores | `nivel_tinta`, `temperatura`, `umidade`, `luminosidade`, `presenca` quando disponível |
| Rastreabilidade | `sequencia`, `uptime_ms`, flags `*_valido`, `falhas_sensores`, `presenca_disponivel` |

Valores `null` não são gravados como zero no InfluxDB: o campo do sensor é omitido no ponto, e sua flag fica falsa. As demais informações do pacote continuam registradas.

Reenvios da fila usam o mesmo dispositivo e o mesmo `recebido_em`; desse modo, uma resposta perdida após a escrita não produz um segundo ponto no InfluxDB. A sequência pode reiniciar após reboot do ESP32 sem sobrescrever dados anteriores, pois o horário de recepção muda. A Serial não possui identificador de boot, portanto uma retransmissão real do mesmo pacote pelo rádio pode ser registrada novamente em outro horário.

## Contrato da API

`POST /leituras`, cabeçalho `X-API-Key: <API_KEY>`, corpo JSON:

```json
{
  "versao": 1,
  "sequencia": 1,
  "nivel_tinta": 75.0,
  "temperatura": 24.0,
  "umidade": 55.0,
  "luminosidade": 680,
  "timestamp": 2000,
  "relogio": "uptime_ms",
  "recebido_em": "2026-11-06T12:00:00.123456Z",
  "dispositivo": "esp32-s3-fabrica"
}
```

O JSON anterior é um exemplo do contrato, não um comando para gravar um timestamp antigo. A ponte preenche `recebido_em` com a hora atual. `presenca` é opcional e aceita somente o inteiro 0, o inteiro 1 ou `null`. Nível, temperatura e umidade aceitam `null`; luminosidade precisa ser inteiro entre 0 e 4095. Campos extras são rejeitados para detectar mudança de protocolo.

Respostas: `201` confirma persistência; `401` indica chave inválida; `422` indica campos inválidos; `503` indica ausência de confirmação no banco. `GET /health` verifica a API; `GET /ready`, autenticado, verifica também a disponibilidade do InfluxDB. A disponibilidade não substitui uma gravação de teste: o script de verificação faz essa confirmação.

## Fila e problemas comuns

```powershell
.\.venv\Scripts\python.exe ponte_serial.py --status
docker compose logs --tail 100 api preparar-tokens influxdb grafana
```

`data/fila.sqlite3` guarda pendências e quarentena. Não apague esse arquivo enquanto houver leituras pendentes. A quarentena pode ser inspecionada com uma ferramenta SQLite e mantém o pacote para análise. Ela não é reenviada automaticamente; corrija o formato antes de reapresentar os dados. Monitore o espaço em disco durante indisponibilidades longas.

| Sintoma | Verificação |
|---|---|
| `No suitable Python runtime found` | O launcher não encontrou a versão pedida. Se `.venv` já existe, use diretamente `.\.venv\Scripts\python.exe`; senão, prepare o Python antes de continuar. |
| `.venv\\Scripts\\python.exe` não encontrado | A criação do ambiente virtual não terminou. Os passos seguintes dependem dela. |
| `required variable ... is missing a value` no Compose | Execute `configurar.py` com um Python funcional, na pasta do projeto, para criar `.env`. Não é necessário preencher manualmente as variáveis uma a uma. |
| Porta ocupada | Feche Monitor Serial e outros programas que usam COM6. |
| Sem pacotes | Confirme porta, baud, alimentação, ESP-NOW e o MAC destino no transmissor. |
| HTTP 401 na ponte | A ponte e o container da API devem usar a mesma `API_KEY` de `.env`. |
| HTTP 503 | Veja logs do InfluxDB e do job de tokens. A fila mantém os dados. |
| Grafana sem dados | Selecione o dispositivo correto e ajuste o intervalo. Confirme mensagens de gravação da ponte. |
| Presença sem leitura | Esperado: seu transmissor ainda não envia PIR. |
| Sensores atuais vazios | Veja `null` nos pacotes, falhas e o tempo desde a última recepção. |
| Login não muda após editar `.env` | Senhas iniciais do banco e do Grafana só são aplicadas ao criar os respectivos volumes. Altere pela interface administrativa, preservando os dados. |

`docker compose down` para os serviços preservando os volumes. Não use remoção de volumes se quiser manter o histórico. Alterar a retenção no Compose depois da inicialização não modifica automaticamente um bucket existente; ajuste-o na interface do InfluxDB.

## Testes e validação

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements-dev.txt
.\.venv\Scripts\python.exe -m pytest -q
```

Os testes cobrem o JSON do transmissor, logs do receptor, dados inválidos, `null`, presença opcional, UTC versus uptime, autenticação, erros do banco e retomada da fila. Incluem um servidor HTTP local que simula a interface de escrita do InfluxDB para exercitar a passagem ponte → API → protocolo de escrita.

**Esse teste não equivale a executar o InfluxDB ou o Grafana reais.** O teste com os produtos reais é `verificar_instalacao.py`, após iniciar os containers.

## Referências técnicas

- [InfluxDB 2 com Docker Compose](https://docs.influxdata.com/influxdb/v2/install/use-docker-compose/)
- [Escrita no InfluxDB 2 pela API](https://docs.influxdata.com/influxdb/v2/get-started/write/)
- [Criação de tokens com permissões](https://docs.influxdata.com/influxdb/v2/admin/tokens/create-token/)
- [Configuração da fonte InfluxDB no Grafana](https://grafana.com/docs/grafana/latest/datasources/influxdb/configure/)
- [Provisionamento de fontes de dados e dashboards](https://grafana.com/docs/grafana/latest/administration/provisioning/)
