"""Sobe o InfluxDB 2 nativo do Windows (sem Docker) e a API da lampada.

Primeira execucao: baixa o InfluxDB oficial (com verificacao SHA-256), cria a
organizacao "laboratorio", o bucket "lampada_iot" com retencao de 30 dias, um
token so de leitura/escrita nesse bucket e o token da API. Depois inicia o
influxd e o servidor.py e testa a gravacao. A URL e o token que o sketch usa
ficam em ACESSO_BANCO.txt (arquivo local, fora do Git).

Execucoes seguintes: so reinicia o InfluxDB e a API com a configuracao salva.
"""
import hashlib
import json
import os
import secrets
import socket
import subprocess
import sys
import time
import zipfile
from pathlib import Path
from urllib.request import Request, urlopen

VERSAO = "2.9.1"
URLS_ZIP = [
    f"https://download.influxdata.com/influxdb/releases/influxdb2-{VERSAO}-windows_amd64.zip",
    f"https://download.influxdata.com/influxdb/releases/v{VERSAO}/influxdb2-{VERSAO}-windows_amd64.zip",
    f"https://dl.influxdata.com/influxdb/releases/influxdb2-{VERSAO}-windows_amd64.zip",
]
SHA256_ZIP = "d8b93916a5e21fe348dfdb8de7ea04fb7425b38939486e874cd8096463b91043"

PASTA = Path(__file__).resolve().parent
PASTA_INFLUX = PASTA / "influxdb"
PASTA_DADOS = PASTA / "dados-influx"
CONFIG = PASTA / "config_local.json"
ACESSO = PASTA / "ACESSO_BANCO.txt"

ORG = "laboratorio"
BUCKET = "lampada_iot"
USUARIO = "admin_lampada"
INFLUX = "http://127.0.0.1:8086"
API = "http://127.0.0.1:8000"
NOVO_CONSOLE = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)


def passo(texto):
    print(f"\n==> {texto}", flush=True)


def http(metodo, url, token=None, corpo=None, esquema="Token"):
    dados = json.dumps(corpo).encode("utf-8") if corpo is not None else None
    req = Request(url, data=dados, method=metodo)
    if dados is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"{esquema} {token}")
    with urlopen(req, timeout=10) as resposta:
        texto = resposta.read()
        return json.loads(texto) if texto else {}


def responde(url):
    try:
        with urlopen(url, timeout=2) as resposta:
            return resposta.status == 200
    except Exception:
        return False


def esperar(url, nome, segundos=60):
    for _ in range(segundos):
        if responde(url):
            return
        time.sleep(1)
    raise SystemExit(f"ERRO: {nome} nao respondeu em {segundos} s. Veja a janela dele para o motivo.")


def baixar_influx():
    existentes = list(PASTA_INFLUX.rglob("influxd.exe"))
    if existentes:
        return existentes[0]
    passo(f"Baixando InfluxDB {VERSAO} oficial de download.influxdata.com")
    destino = PASTA / f"influxdb2-{VERSAO}-windows_amd64.zip"
    hash_zip = hashlib.sha256()
    resposta = None
    for url in URLS_ZIP:
        try:
            resposta = urlopen(Request(url, headers={"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}), timeout=60)
            print(f"   Fonte: {url}")
            break
        except Exception as erro:
            print(f"   {url} -> {erro}")
    if resposta is None:
        raise SystemExit("ERRO: nao foi possivel baixar o InfluxDB de nenhuma URL oficial.")
    with resposta, open(destino, "wb") as arquivo:
        total = int(resposta.headers.get("Content-Length", 0))
        baixado = 0
        while bloco := resposta.read(1 << 20):
            arquivo.write(bloco)
            hash_zip.update(bloco)
            baixado += len(bloco)
            if total:
                print(f"\r   {baixado >> 20} de {total >> 20} MB", end="", flush=True)
    print()
    if hash_zip.hexdigest() != SHA256_ZIP:
        destino.unlink()
        raise SystemExit("ERRO: o arquivo baixado nao confere com o SHA-256 oficial. Nada foi executado.")
    print("   SHA-256 conferido.")
    with zipfile.ZipFile(destino) as arquivo_zip:
        arquivo_zip.extractall(PASTA_INFLUX)
    destino.unlink()
    return next(PASTA_INFLUX.rglob("influxd.exe"))


def iniciar_influx(executavel):
    if responde(f"{INFLUX}/health"):
        print("   InfluxDB ja estava rodando.")
        return
    PASTA_DADOS.mkdir(exist_ok=True)
    subprocess.Popen(
        [str(executavel),
         f"--bolt-path={PASTA_DADOS / 'influxd.bolt'}",
         f"--engine-path={PASTA_DADOS / 'engine'}",
         "--http-bind-address=127.0.0.1:8086",
         "--reporting-disabled"],
        cwd=PASTA, creationflags=NOVO_CONSOLE,
    )
    esperar(f"{INFLUX}/health", "InfluxDB")
    print("   InfluxDB no ar em http://127.0.0.1:8086")


def configurar_influx(cfg):
    if http("GET", f"{INFLUX}/api/v2/setup").get("allowed"):
        passo("Configuracao inicial: org laboratorio, bucket lampada_iot, retencao 30 dias")
        cfg["influx_admin_senha"] = secrets.token_urlsafe(12)
        cfg["influx_admin_token"] = secrets.token_hex(32)
        cfg.pop("influx_api_token", None)
        http("POST", f"{INFLUX}/api/v2/setup", corpo={
            "username": USUARIO, "password": cfg["influx_admin_senha"],
            "org": ORG, "bucket": BUCKET,
            "retentionPeriodSeconds": 30 * 24 * 3600,
            "token": cfg["influx_admin_token"],
        })
        salvar(cfg)
    elif "influx_admin_token" not in cfg:
        raise SystemExit("ERRO: este InfluxDB ja foi configurado por outro meio e nao ha token salvo aqui.")

    if not cfg.get("influx_api_token"):
        passo("Criando token so de leitura e escrita no bucket lampada_iot")
        admin = cfg["influx_admin_token"]
        org_id = http("GET", f"{INFLUX}/api/v2/orgs?org={ORG}", admin)["orgs"][0]["id"]
        bucket_id = http("GET", f"{INFLUX}/api/v2/buckets?name={BUCKET}&orgID={org_id}", admin)["buckets"][0]["id"]
        permissoes = [{"action": acao, "resource": {"type": "buckets", "id": bucket_id, "orgID": org_id}}
                      for acao in ("read", "write")]
        resposta = http("POST", f"{INFLUX}/api/v2/authorizations", admin, {
            "orgID": org_id, "description": "API lampada (lampada_iot)", "permissions": permissoes,
        })
        cfg["influx_api_token"] = resposta["token"]
    cfg.setdefault("api_token", secrets.token_hex(32))
    salvar(cfg)


def iniciar_api(cfg):
    if responde(f"{API}/health"):
        print("   API ja estava rodando.")
        return
    ambiente = {
        **os.environ,
        "INFLUX_URL": INFLUX, "INFLUX_ORG": ORG, "INFLUX_BUCKET": BUCKET,
        "INFLUX_API_TOKEN": cfg["influx_api_token"], "API_TOKEN": cfg["api_token"],
        "API_HOST": "0.0.0.0", "API_PORT": "8000",
    }
    subprocess.Popen([sys.executable, "-u", str(PASTA / "servidor.py")],
                     cwd=PASTA, env=ambiente, creationflags=NOVO_CONSOLE)
    esperar(f"{API}/health", "API")
    print("   API no ar na porta 8000")


def testar_gravacao(cfg):
    passo("Teste: gravando e lendo uma amostra 'teste-lab' pela API")
    amostra = {
        "dispositivo": "teste-lab", "timestamp": int(time.time()),
        "temperatura_c": 23.5, "umidade_pct": 57.2, "luminosidade_adc": 980,
        "nivel_tanque_pct": None, "potenciometro_adc": 1900, "brilho_pct": 100,
        "cor": "Amarelo", "botao_pressionado": False, "sistema_ligado": True,
        "led_ligado": True, "buzzer_ligado": False, "temperatura_ativa": True,
        "detector_ativo": True, "buzzer_ativo": True, "alerta_temperatura": False,
    }
    resposta = http("POST", f"{API}/api/v1/leituras", cfg["api_token"], amostra, esquema="Bearer")
    print(f"   OK: gravada e consultada no InfluxDB (timestamp {resposta['timestamp']}).")


def ip_da_rede():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]


def salvar(cfg):
    CONFIG.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def main():
    cfg = json.loads(CONFIG.read_text(encoding="utf-8")) if CONFIG.exists() else {}
    executavel = baixar_influx()
    passo("Iniciando InfluxDB")
    iniciar_influx(executavel)
    configurar_influx(cfg)
    passo("Iniciando API (servidor.py)")
    iniciar_api(cfg)
    testar_gravacao(cfg)
    ip = ip_da_rede()
    passo(f"IP deste PC na rede: {ip}")
    ACESSO.write_text(
        "Painel do InfluxDB: http://127.0.0.1:8086\n"
        f"Usuario: {USUARIO}\nSenha: {cfg['influx_admin_senha']}\n"
        f"Org: {ORG} | Bucket: {BUCKET} | Medicao: lampada | Dispositivo: lampada-wroom-01\n"
        f"No sketch: API_URL = http://{ip}:8000/api/v1/leituras\n"
        f"No sketch: API_TOKEN = {cfg['api_token']}\n"
        "Nao envie este arquivo nem config_local.json para o GitHub.\n",
        encoding="utf-8",
    )
    passo("PRONTO. Deixe as janelas do InfluxDB e da API abertas.")
    print(f"   Login do painel salvo em {ACESSO}")
    print("   Para religar depois, execute iniciar_banco.bat de novo.")


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as erro:
        raise SystemExit(f"ERRO: {type(erro).__name__}: {erro}")
