import csv
import hmac
import io
import json
import logging
import os
import re
import socket
import time
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen


IDENTIFICADOR = re.compile(r"[A-Za-z0-9_-]{1,64}")
TOKEN_API = re.compile(r"[A-Za-z0-9_-]{32,128}")
ROTA_LEITURAS = "/api/v1/leituras"
MEDICAO = "lampada"
LIMITE_CORPO = 2048
CORES = {"Apagado", "Vermelho", "Laranja", "Amarelo", "Branco", "Turquesa", "Azul"}
OPCIONAIS = {
    "temperatura_c": (float, -40, 80),
    "umidade_pct": (float, 0, 100),
    "luminosidade_adc": (int, 0, 4095),
    "nivel_tanque_pct": (float, 0, 100),
}
NUMERICOS = {
    "potenciometro_adc": (int, 0, 4095),
    "brilho_pct": (int, 0, 100),
}
BOOLEANOS = (
    "botao_pressionado", "sistema_ligado", "led_ligado", "buzzer_ligado",
    "temperatura_ativa", "detector_ativo", "buzzer_ativo", "alerta_temperatura",
)
CAMPOS = ("dispositivo", "timestamp", "cor", *OPCIONAIS, *NUMERICOS, *BOOLEANOS)


def validar_leitura(dados):
    if not isinstance(dados, dict) or dados.keys() != set(CAMPOS):
        raise ValueError("O JSON deve conter todos os campos da leitura.")
    dispositivo = dados["dispositivo"]
    if not isinstance(dispositivo, str) or not IDENTIFICADOR.fullmatch(dispositivo):
        raise ValueError("Identificador de dispositivo inválido.")
    timestamp = dados["timestamp"]
    agora = int(time.time())
    if type(timestamp) is not int or not agora - 86400 <= timestamp <= agora + 300:
        raise ValueError("O timestamp deve estar em segundos Unix UTC, de 24 h atrás a 5 min à frente.")
    if not isinstance(dados["cor"], str) or dados["cor"] not in CORES:
        raise ValueError("Cor inválida.")
    for campo in BOOLEANOS:
        if type(dados[campo]) is not bool:
            raise ValueError(f"{campo} deve ser booleano.")
    for campo, (tipo, minimo, maximo) in (OPCIONAIS | NUMERICOS).items():
        valor = dados[campo]
        if valor is None and campo in OPCIONAIS:
            continue
        tipos_aceitos = (int,) if tipo is int else (int, float)
        if type(valor) not in tipos_aceitos or not minimo <= valor <= maximo:
            raise ValueError(f"{campo} fora do intervalo ou com tipo inválido.")
        dados[campo] = tipo(valor)
    return dados


def para_line_protocol(dados):
    campos = [f'cor="{dados["cor"]}"']
    campos.extend(f"{campo}={str(dados[campo]).lower()}" for campo in BOOLEANOS)
    for campo, (tipo, _, _) in (OPCIONAIS | NUMERICOS).items():
        valor = dados[campo]
        if valor is not None:
            sufixo = "i" if tipo is int else ""
            campos.append(f"{campo}={valor}{sufixo}")
    return f'{MEDICAO},dispositivo={dados["dispositivo"]} {",".join(campos)} {dados["timestamp"]}'


class InfluxDB:
    def __init__(self, url, org, bucket, token):
        self.url = url.rstrip("/")
        self.org = org
        self.bucket = bucket
        self.token = token

    def requisitar(self, rota, parametros, corpo, tipo, aceite):
        consulta = urlencode({"org": self.org, **parametros})
        req = Request(
            f"{self.url}/api/v2/{rota}?{consulta}",
            data=corpo.encode("utf-8"),
            headers={
                "Authorization": f"Token {self.token}",
                "Content-Type": tipo,
                "Accept": aceite,
            },
        )
        with urlopen(req, timeout=4) as resposta:
            if rota == "write" and resposta.status != 204:
                raise OSError("Resposta de gravação inesperada do InfluxDB")
            return resposta.read()

    def gravar(self, dados):
        self.requisitar(
            "write", {"bucket": self.bucket, "precision": "s"},
            para_line_protocol(dados), "text/plain; charset=utf-8", "application/json",
        )

    def consultar(self, dispositivo, timestamp=None):
        janela = (
            "start: -24h, stop: 5m" if timestamp is None
            else f"start: time(v: {timestamp * 10**9}), stop: time(v: {(timestamp + 1) * 10**9})"
        )
        consulta = (
            f'from(bucket: "{self.bucket}")\n'
            f'  |> range({janela})\n'
            f'  |> filter(fn: (r) => r._measurement == "{MEDICAO}" and r.dispositivo == "{dispositivo}")\n'
            '  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> sort(columns: ["_time"], desc: true)\n'
            '  |> limit(n: 1)'
        )
        texto = self.requisitar("query", {}, consulta, "application/vnd.flux", "application/csv")
        linhas = (linha for linha in io.StringIO(texto.decode("utf-8")) if not linha.startswith("#"))
        registro = next(csv.DictReader(linhas), None)
        if registro is None:
            return None
        leitura = {
            "dispositivo": registro["dispositivo"],
            "timestamp": int(datetime.fromisoformat(registro["_time"]).timestamp()),
            "cor": registro["cor"],
        }
        for campo in BOOLEANOS:
            leitura[campo] = {"true": True, "false": False}[registro[campo]]
        for campo, (tipo, _, _) in (OPCIONAIS | NUMERICOS).items():
            valor = registro.get(campo)
            leitura[campo] = tipo(valor) if valor else None
        return leitura


class Servidor(ThreadingHTTPServer):
    def __init__(self, endereco, influx, token):
        self.influx = influx
        self.token = f"Bearer {token}".encode("ascii")
        super().__init__(endereco, Rotas)

    def get_request(self):
        conexao, endereco = super().get_request()
        conexao.settimeout(8)
        return conexao, endereco


class Rotas(BaseHTTPRequestHandler):
    def responder(self, status, dados):
        corpo = json.dumps(dados, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(corpo)))
        self.send_header("Cache-Control", "no-store")
        if status == HTTPStatus.UNAUTHORIZED:
            self.send_header("WWW-Authenticate", "Bearer")
        self.end_headers()
        self.wfile.write(corpo)

    def autenticar(self):
        cabecalho = self.headers.get("Authorization", "").encode("latin-1")
        if hmac.compare_digest(cabecalho, self.server.token):
            return True
        self.responder(HTTPStatus.UNAUTHORIZED, {"erro": "Token inválido ou ausente."})
        return False

    def erro_banco(self, erro):
        logging.warning("Falha no InfluxDB (%s)", type(erro).__name__)
        self.responder(HTTPStatus.SERVICE_UNAVAILABLE, {"erro": "InfluxDB indisponível."})

    def do_POST(self):
        if urlsplit(self.path).path != ROTA_LEITURAS:
            self.responder(HTTPStatus.NOT_FOUND, {"erro": "Rota inexistente."})
            return
        if not self.autenticar():
            return
        if self.headers.get_content_type() != "application/json":
            self.responder(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {"erro": "Use application/json."})
            return
        try:
            tamanho = int(self.headers.get("Content-Length", "0"))
            if tamanho < 1:
                raise ValueError
        except ValueError:
            self.responder(HTTPStatus.BAD_REQUEST, {"erro": "Content-Length inválido."})
            return
        if tamanho > LIMITE_CORPO:
            self.responder(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"erro": "Corpo excede 2048 bytes."})
            return
        try:
            corpo = self.rfile.read(tamanho)
            if len(corpo) != tamanho:
                raise ValueError
            dados = json.loads(corpo.decode("utf-8"))
        except (ValueError, RecursionError, socket.timeout):
            self.responder(HTTPStatus.BAD_REQUEST, {"erro": "Corpo JSON inválido."})
            return
        try:
            dados = validar_leitura(dados)
        except ValueError as erro:
            self.responder(HTTPStatus.UNPROCESSABLE_ENTITY, {"erro": str(erro)})
            return
        try:
            self.server.influx.gravar(dados)
            leitura = self.server.influx.consultar(dados["dispositivo"], dados["timestamp"])
            if leitura is None:
                raise OSError("Leitura ainda não está disponível para consulta")
        except (OSError, ValueError, KeyError, csv.Error) as erro:
            self.erro_banco(erro)
            return
        self.responder(HTTPStatus.CREATED, leitura)

    def do_GET(self):
        caminho = urlsplit(self.path).path
        if caminho == "/health":
            self.responder(HTTPStatus.OK, {"status": "ok"})
            return
        partes = caminho.split("/")
        if len(partes) != 6 or "/".join(partes[:4]) != ROTA_LEITURAS or partes[5] != "ultima":
            self.responder(HTTPStatus.NOT_FOUND, {"erro": "Rota inexistente."})
            return
        if not self.autenticar():
            return
        dispositivo = partes[4]
        if not IDENTIFICADOR.fullmatch(dispositivo):
            self.responder(HTTPStatus.BAD_REQUEST, {"erro": "Identificador de dispositivo inválido."})
            return
        try:
            leitura = self.server.influx.consultar(dispositivo)
        except (OSError, ValueError, KeyError, csv.Error) as erro:
            self.erro_banco(erro)
            return
        if leitura is None:
            self.responder(HTTPStatus.NOT_FOUND, {"erro": "Nenhuma leitura nas últimas 24 h."})
            return
        self.responder(HTTPStatus.OK, leitura)


def main():
    config = {chave: os.getenv(chave, "") for chave in ("API_TOKEN", "INFLUX_API_TOKEN", "INFLUX_ORG", "INFLUX_BUCKET")}
    if not TOKEN_API.fullmatch(config["API_TOKEN"]):
        raise SystemExit("Configure API_TOKEN com 32 a 128 caracteres alfanuméricos, hífen ou sublinhado.")
    if not config["INFLUX_API_TOKEN"] or not config["INFLUX_ORG"] or not IDENTIFICADOR.fullmatch(config["INFLUX_BUCKET"]):
        raise SystemExit("Configure INFLUX_API_TOKEN, INFLUX_ORG e INFLUX_BUCKET.")
    influx = InfluxDB(
        os.getenv("INFLUX_URL", "http://127.0.0.1:8086"),
        config["INFLUX_ORG"], config["INFLUX_BUCKET"], config["INFLUX_API_TOKEN"],
    )
    host = os.getenv("API_HOST", "0.0.0.0")
    porta = int(os.getenv("API_PORT", "8000"))
    with Servidor((host, porta), influx, config["API_TOKEN"]) as servidor:
        print(f"API de telemetria em {host}:{porta}", flush=True)
        servidor.serve_forever()


if __name__ == "__main__":
    main()
