import os
from pathlib import Path

RAIZ = Path(__file__).resolve().parent


def carregar_env():
    arquivo = RAIZ / ".env"
    if not arquivo.exists():
        raise SystemExit("Execute primeiro: python configurar.py")
    for linha in arquivo.read_text(encoding="utf-8").splitlines():
        if linha.strip() and not linha.lstrip().startswith("#"):
            nome, valor = linha.split("=", 1)
            os.environ.setdefault(nome.strip(), valor.strip())
