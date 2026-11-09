import logging
import os
import secrets
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Annotated

import httpx
from fastapi import Depends, FastAPI, Header, HTTPException

from .models import Leitura, para_line_protocol

log = logging.getLogger("etapa03")


def segredo(nome: str) -> str:
    caminho = os.getenv(nome + "_FILE")
    valor = Path(caminho).read_text(encoding="utf-8").strip() if caminho else os.getenv(nome, "")
    if not valor:
        raise RuntimeError(f"Configure {nome} ou {nome}_FILE.")
    return valor


@asynccontextmanager
async def lifespan(app):
    app.state.api_key = segredo("API_KEY")
    app.state.token = segredo("INFLUXDB_TOKEN")
    app.state.influx = httpx.AsyncClient(
        base_url=os.getenv("INFLUXDB_URL", "http://influxdb:8086"),
        timeout=5.0,
    )
    yield
    await app.state.influx.aclose()


app = FastAPI(title="Pintura de blocos - Etapa 03", version="1.0.0", lifespan=lifespan)


def autenticar(x_api_key: Annotated[str | None, Header()] = None):
    if not x_api_key or not secrets.compare_digest(x_api_key, app.state.api_key):
        raise HTTPException(401, "Chave de API ausente ou invalida.")


@app.get("/health")
async def health():
    """Disponibilidade da API. /ready tambem verifica o banco."""
    return {"status": "ok"}


@app.get("/ready", dependencies=[Depends(autenticar)])
async def ready():
    try:
        resposta = await app.state.influx.get("/health")
        resposta.raise_for_status()
        if resposta.json().get("status") != "pass":
            raise ValueError("Banco nao esta pronto.")
    except (httpx.HTTPError, ValueError):
        raise HTTPException(503, "InfluxDB indisponivel.") from None
    return {"status": "ok", "influxdb": "ok"}


@app.post("/leituras", status_code=201, dependencies=[Depends(autenticar)])
async def receber(leitura: Leitura):
    try:
        resposta = await app.state.influx.post(
            "/api/v2/write",
            params={"org": os.getenv("INFLUXDB_ORG", "fabrica"),
                    "bucket": os.getenv("INFLUXDB_BUCKET", "pintura"), "precision": "us"},
            headers={"Authorization": f"Token {app.state.token}", "Content-Type": "text/plain; charset=utf-8"},
            content=para_line_protocol(leitura),
        )
        resposta.raise_for_status()
        if resposta.status_code != 204:
            raise HTTPException(503, "InfluxDB nao confirmou a gravacao.")
    except httpx.HTTPError:
        # Nunca imprimir tokens nem o corpo de erros internos no cliente.
        log.warning("Gravacao no InfluxDB falhou; leitura deve permanecer na fila da ponte.")
        raise HTTPException(503, "Leitura nao confirmada no InfluxDB. Tente novamente.") from None
    return {"status": "persistido", "sequencia": leitura.sequencia,
            "recebido_em": leitura.recebido_em,
            "origem_tempo": "recepcao_pc", "presenca_disponivel": leitura.presenca is not None}
