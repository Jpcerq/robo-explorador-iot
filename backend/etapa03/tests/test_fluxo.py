from datetime import datetime, timezone
import json
from pathlib import Path
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import httpx
import pytest
import yaml
from fastapi.testclient import TestClient
from pydantic import ValidationError

from backend.main import app
from backend.models import Leitura, para_line_protocol
from ponte_serial import Fila, enviar_um, interpretar_linha

RAIZ = Path(__file__).resolve().parents[1]
HORA = "2026-11-06T12:00:00.123456Z"


@pytest.fixture
def pacote():
    return json.loads((RAIZ / "exemplo_pacote.json").read_text())


@pytest.fixture
def normalizado(pacote):
    return interpretar_linha("Conteudo: " + json.dumps(pacote), recebido_em=HORA)


def test_saida_exata_receptor(normalizado):
    assert normalizado["timestamp"] == 2000
    assert normalizado["recebido_em"] == HORA
    assert normalizado["presenca"] is None
    assert normalizado["dispositivo"] == "esp32-s3-fabrica"


@pytest.mark.parametrize("linha", ["", "MAC do receptor: E8:31:CD:AD:84:DC", "===== PACOTE RECEBIDO =====", "Total recebido: 1", "==========================="])
def test_logs_ignorados(linha):
    assert interpretar_linha(linha) is None


def test_null_nao_vira_zero(pacote):
    pacote.update(nivel_tinta=None, temperatura=None, umidade=None)
    leitura = Leitura(**interpretar_linha(json.dumps(pacote), recebido_em=HORA))
    linha = para_line_protocol(leitura)
    assert "nivel_tinta=" not in linha
    assert "temperatura=" not in linha
    assert "umidade=" not in linha
    assert "falhas_sensores=3i" in linha
    assert "nivel_tinta_valido=false" in linha
    assert "presenca=" not in linha
    assert "presenca_disponivel=false" in linha


def test_presenca_opcional_e_distinta_de_ausencia(pacote):
    pacote["presenca"] = 0
    leitura = Leitura(**interpretar_linha(json.dumps(pacote), recebido_em=HORA))
    assert "presenca=0i" in para_line_protocol(leitura)
    assert "presenca_disponivel=true" in para_line_protocol(leitura)


@pytest.mark.parametrize("campo,valor", [
    ("nivel_tinta", 101), ("temperatura", -41), ("umidade", -1),
    ("luminosidade", 4096), ("luminosidade", True), ("presenca", 2),
    ("presenca", False), ("timestamp", -1), ("timestamp", "2000"),
    ("relogio", "unix"), ("nivel_tinta", "75"), ("nivel_tinta", True),
    ("temperatura", float("nan")), ("umidade", float("inf")),
    ("versao", 2), ("versao", True), ("sequencia", 1.2),
])
def test_pacotes_invalidos_rejeitados(pacote, campo, valor):
    pacote[campo] = valor
    with pytest.raises((ValidationError, ValueError)):
        interpretar_linha(json.dumps(pacote), recebido_em=HORA)


def test_timestamp_sem_fuso_rejeitado(pacote):
    with pytest.raises(ValidationError):
        interpretar_linha(json.dumps(pacote), recebido_em="2026-11-06T12:00:00")


def test_uptime_nao_interpretado_como_unix(normalizado):
    linha = para_line_protocol(Leitura(**normalizado))
    data = datetime.fromisoformat(HORA.replace("Z", "+00:00"))
    segundos = (data - datetime(1970, 1, 1, tzinfo=timezone.utc)).days * 86400 + 12 * 3600
    assert linha.endswith(f" {segundos * 1000000 + 123456}")
    assert "uptime_ms=2000i" in linha
    assert "origem_tempo=recepcao_pc" in linha


def test_reiniciar_esp_nao_joga_dado_para_1970(pacote):
    pacote["timestamp"] = 0
    pacote["sequencia"] = 1
    ponto = Leitura(**interpretar_linha(json.dumps(pacote), recebido_em=HORA))
    assert para_line_protocol(ponto).split()[-1] != "0"


@pytest.fixture
def cliente(monkeypatch):
    monkeypatch.setenv("API_KEY", "chave-de-teste")
    monkeypatch.setenv("INFLUXDB_TOKEN", "token-de-teste")
    monkeypatch.delenv("API_KEY_FILE", raising=False)
    monkeypatch.delenv("INFLUXDB_TOKEN_FILE", raising=False)
    original = httpx.AsyncClient
    recebidos = []
    estado = {"status": 204}

    def tratar(req):
        recebidos.append(req)
        if req.url.path == "/health":
            return httpx.Response(200, json={"status": "pass"})
        return httpx.Response(estado["status"])

    monkeypatch.setattr("backend.main.httpx.AsyncClient", lambda **kwargs: original(transport=httpx.MockTransport(tratar), **kwargs))
    with TestClient(app) as cli:
        yield cli, recebidos, estado


def test_api_exige_autenticacao(cliente, normalizado):
    cli, recebidos, _ = cliente
    assert cli.post("/leituras", json=normalizado).status_code == 401
    assert cli.post("/leituras", json=normalizado, headers={"X-API-Key":"errada"}).status_code == 401
    assert not recebidos


def test_api_grava_antes_de_confirmar(cliente, normalizado):
    cli, recebidos, _ = cliente
    resposta = cli.post("/leituras", json=normalizado, headers={"X-API-Key":"chave-de-teste"})
    assert resposta.status_code == 201
    assert resposta.json()["status"] == "persistido"
    req = recebidos[0]
    assert req.url.path == "/api/v2/write"
    assert req.url.params["precision"] == "us"
    assert req.headers["authorization"] == "Token token-de-teste"
    assert "nivel_tinta=75.0" in req.content.decode()


@pytest.mark.parametrize("status", [401, 422, 429, 500, 202])
def test_api_nao_confirma_falha_banco(cliente, normalizado, status):
    cli, _, estado = cliente
    estado["status"] = status
    resposta = cli.post("/leituras", json=normalizado, headers={"X-API-Key":"chave-de-teste"})
    assert resposta.status_code == 503
    assert "token-de-teste" not in resposta.text


def test_api_valida_json(cliente, normalizado):
    cli, recebidos, _ = cliente
    normalizado["luminosidade"] = -1
    assert cli.post("/leituras", json=normalizado, headers={"X-API-Key":"chave-de-teste"}).status_code == 422
    assert not recebidos


def test_fila_sobrevive_reinicio_e_tenta_mesmo_timestamp(tmp_path, normalizado):
    caminho = tmp_path / "fila.sqlite3"
    Fila(caminho).adicionar(normalizado)
    fila = Fila(caminho)
    corpos = []
    estado = {"status": 503}

    def tratar(req):
        corpos.append(req.content)
        return httpx.Response(estado["status"], json={"status": "persistido"})

    with httpx.Client(transport=httpx.MockTransport(tratar)) as cli:
        assert not enviar_um(fila, cli, "http://api", "chave")
        assert fila.contagens() == {"pendente": 1}
        estado["status"] = 201
        assert enviar_um(fila, cli, "http://api", "chave")
        assert fila.contagens() == {}
        assert corpos[0] == corpos[1]


def test_fila_pacote_rejeitado_nao_bloqueia_proximo(tmp_path, normalizado):
    fila = Fila(tmp_path / "fila.sqlite3")
    fila.adicionar(normalizado)
    fila.adicionar(normalizado)
    with httpx.Client(transport=httpx.MockTransport(lambda req: httpx.Response(422))) as cli:
        assert enviar_um(fila, cli, "http://api", "chave")
    assert fila.contagens() == {"pendente": 1, "quarentena": 1}


def test_fila_preservada_sem_rede(tmp_path, normalizado):
    fila = Fila(tmp_path / "fila.sqlite3")
    fila.adicionar(normalizado)
    def timeout(req):
        raise httpx.ConnectError("Indisponivel", request=req)
    with httpx.Client(transport=httpx.MockTransport(timeout)) as cli:
        assert not enviar_um(fila, cli, "http://api", "chave")
    assert fila.contagens() == {"pendente": 1}


def test_http_local_real_ponte_api_e_protocolo_influx(monkeypatch, tmp_path, normalizado):
    # Servidor HTTP real simula apenas o endpoint de escrita do InfluxDB.
    recebidos = []
    class BancoHTTP(BaseHTTPRequestHandler):
        def do_POST(self):
            recebidos.append((self.path, self.rfile.read(int(self.headers["Content-Length"]))))
            self.send_response(204)
            self.end_headers()
        def log_message(self, *args):
            pass
    servidor = ThreadingHTTPServer(("127.0.0.1", 0), BancoHTTP)
    thread = threading.Thread(target=servidor.serve_forever, daemon=True)
    thread.start()
    monkeypatch.setenv("API_KEY", "chave")
    monkeypatch.setenv("INFLUXDB_TOKEN", "token")
    monkeypatch.setenv("INFLUXDB_URL", f"http://127.0.0.1:{servidor.server_port}")
    monkeypatch.delenv("INFLUXDB_TOKEN_FILE", raising=False)
    try:
        with TestClient(app) as cli:
            fila = Fila(tmp_path / "fila.sqlite3")
            fila.adicionar(normalizado)
            assert enviar_um(fila, cli, "", "chave")
            assert fila.contagens() == {}
        assert "precision=us" in recebidos[0][0]
        assert "uptime_ms=2000i" in recebidos[0][1].decode()
    finally:
        servidor.shutdown()
        servidor.server_close()
        thread.join(timeout=2)


def test_provisionamento_completo():
    compose = yaml.safe_load((RAIZ / "compose.yaml").read_text())
    assert compose["services"]["influxdb"]["environment"]["DOCKER_INFLUXDB_INIT_RETENTION"] == "30d"
    assert "INFLUXDB_ADMIN_TOKEN" not in compose["services"]["api"]["environment"]
    datasource = yaml.safe_load((RAIZ / "grafana/provisioning/datasources/influxdb.yaml").read_text())
    assert datasource["datasources"][0]["jsonData"]["version"] == "Flux"
    dashboard = json.loads((RAIZ / "grafana/dashboards/pintura.json").read_text(encoding="utf-8"))
    assert dashboard["refresh"] == "2s"
    assert len({p["id"] for p in dashboard["panels"]}) == len(dashboard["panels"])
    queries = "\n".join(t["query"] for p in dashboard["panels"] for t in p.get("targets", []))
    for campo in ("nivel_tinta", "temperatura", "umidade", "luminosidade", "presenca"):
        assert campo in queries
    assert "range(start: -10s)" in queries
