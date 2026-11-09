"""Le o receptor original, guarda em SQLite e encaminha para a API.

SQLite e somente a fila de transporte local. O banco do projeto e o InfluxDB.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import logging
import os
from pathlib import Path
import sqlite3
import threading
import time

import httpx
import serial

from ambiente import RAIZ, carregar_env
from backend.models import Leitura

log = logging.getLogger("ponte")


def interpretar_linha(linha: str, dispositivo="esp32-s3-fabrica", recebido_em=None):
    texto = linha.strip().rstrip("\x00")
    if texto.startswith("Conteudo:"):
        texto = texto[len("Conteudo:"):].strip().rstrip("\x00")
    elif not texto.startswith("{"):
        return None  # Ignora cabecalhos, MAC, contadores e separadores do receptor.
    dados = json.loads(texto)
    if not isinstance(dados, dict):
        raise ValueError("Pacote precisa ser um objeto JSON.")
    # Estes campos pertencem a ponte, nunca ao relogio uptime do ESP32.
    dados["recebido_em"] = recebido_em or datetime.now(timezone.utc)
    dados["dispositivo"] = dispositivo
    return Leitura.model_validate(dados).model_dump(mode="json")


class Fila:
    def __init__(self, arquivo):
        self.arquivo = str(arquivo)
        Path(arquivo).parent.mkdir(parents=True, exist_ok=True)
        with self.conectar() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("""CREATE TABLE IF NOT EXISTS fila (
                id INTEGER PRIMARY KEY, payload TEXT NOT NULL,
                estado TEXT NOT NULL DEFAULT 'pendente', motivo TEXT)""")

    @contextmanager
    def conectar(self):
        db = sqlite3.connect(self.arquivo, timeout=10)
        try:
            with db:
                yield db
        finally:
            db.close()

    def adicionar(self, payload):
        with self.conectar() as db:
            db.execute("INSERT INTO fila(payload) VALUES (?)", (json.dumps(payload, allow_nan=False),))

    def proximo(self):
        with self.conectar() as db:
            return db.execute("SELECT id, payload FROM fila WHERE estado='pendente' ORDER BY id LIMIT 1").fetchone()

    def remover(self, identificador):
        with self.conectar() as db:
            db.execute("DELETE FROM fila WHERE id=?", (identificador,))

    def rejeitar(self, identificador, motivo):
        with self.conectar() as db:
            db.execute("UPDATE fila SET estado='quarentena', motivo=? WHERE id=?", (motivo, identificador))

    def invalidar_linha(self, linha):
        with self.conectar() as db:
            db.execute("INSERT INTO fila(payload, estado, motivo) VALUES (?, 'quarentena', ?)",
                       (linha, "JSON ou campos invalidos; conferir pacote original."))

    def contagens(self):
        with self.conectar() as db:
            return dict(db.execute("SELECT estado, count(*) FROM fila GROUP BY estado"))


def enviar_um(fila, cliente, url, api_key):
    item = fila.proximo()
    if item is None:
        return False
    identificador, payload = item
    try:
        resposta = cliente.post(url + "/leituras", content=payload,
                                 headers={"Content-Type": "application/json", "X-API-Key": api_key})
    except httpx.HTTPError:
        log.warning("API inacessivel; leitura %s preservada na fila.", identificador)
        return False
    if resposta.status_code == 201 and resposta.json().get("status") == "persistido":
        fila.remover(identificador)
        log.info("Leitura %s confirmada no InfluxDB.", identificador)
        return True
    if resposta.status_code == 422:
        fila.rejeitar(identificador, "API rejeitou os campos (HTTP 422).")
        log.error("Leitura %s em quarentena; confira os campos.", identificador)
        return True
    log.warning("API retornou HTTP %s; leitura %s preservada.", resposta.status_code, identificador)
    return False


def ler_serial(porta, baud, fila, parar, dispositivo):
    while not parar.is_set():
        try:
            with serial.Serial(port=porta, baudrate=baud, timeout=1) as conexao:
                log.info("Serial %s aberta a %s baud.", porta, baud)
                pendente = bytearray()
                while not parar.is_set():
                    pedaco = conexao.read_until(b"\n", size=2048)
                    if not pedaco:
                        continue
                    pendente.extend(pedaco)
                    if len(pendente) > 4096:
                        fila.invalidar_linha(pendente.decode("utf-8", errors="replace"))
                        pendente.clear()
                        continue
                    if not pendente.endswith(b"\n"):
                        continue
                    linha = pendente.decode("utf-8", errors="replace")
                    pendente.clear()
                    try:
                        payload = interpretar_linha(linha, dispositivo)
                        if payload is not None:
                            fila.adicionar(payload)
                            log.info("RX sequencia=%s registrada na fila.", payload["sequencia"])
                    except (ValueError, TypeError):
                        fila.invalidar_linha(linha)
                        log.error("Pacote invalido preservado em quarentena.")
        except (serial.SerialException, OSError):
            log.warning("Nao foi possivel ler %s. Feche o Monitor Serial e confira o cabo.", porta)
            parar.wait(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--porta", default="COM6")
    parser.add_argument("--baud", default=115200, type=int)
    parser.add_argument("--api", default="http://localhost:8000")
    parser.add_argument("--dispositivo", default="esp32-s3-fabrica")
    parser.add_argument("--fila", type=Path, default=RAIZ / "data" / "fila.sqlite3")
    parser.add_argument("--status", action="store_true", help="Mostra contagens da fila e sai.")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    fila = Fila(args.fila)
    if args.status:
        print(fila.contagens())
        return
    carregar_env()
    parar = threading.Event()
    leitor = threading.Thread(target=ler_serial, args=(args.porta, args.baud, fila, parar, args.dispositivo), daemon=True)
    leitor.start()
    try:
        with httpx.Client(timeout=5, trust_env=False) as cliente:
            while not parar.is_set():
                if not enviar_um(fila, cliente, args.api.rstrip("/"), os.environ["API_KEY"]):
                    parar.wait(2)
    except KeyboardInterrupt:
        log.info("Encerrando. Leituras pendentes continuam no disco.")
    finally:
        parar.set()
        leitor.join(timeout=3)


if __name__ == "__main__":
    main()
