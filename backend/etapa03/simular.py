"""Envia dados artificiais identificados como esp32-s3-demo, sem usar a porta Serial."""
import argparse
from datetime import datetime, timezone
import math
import os
import time

import httpx

from ambiente import carregar_env
from ponte_serial import interpretar_linha
import json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quantidade", type=int, default=30)
    parser.add_argument("--intervalo", type=float, default=2)
    parser.add_argument("--com-presenca", action="store_true", help="Simula PIR, ausente no firmware real.")
    args = parser.parse_args()
    carregar_env()
    with httpx.Client(timeout=10, trust_env=False) as cliente:
        for i in range(args.quantidade):
            dados = {"versao": 1, "sequencia": i + 1, "nivel_tinta": round(max(5, 80 - i * 2.5), 1),
                     "temperatura": round(25 + math.sin(i / 5) * 3, 1), "umidade": round(55 + math.cos(i / 5) * 8, 1),
                     "luminosidade": int(1600 + 700 * math.sin(i / 4)), "timestamp": i * 2000, "relogio": "uptime_ms"}
            if args.com_presenca:
                dados["presenca"] = (i // 5) % 2
            pacote = interpretar_linha(json.dumps(dados), dispositivo="esp32-s3-demo",
                                       recebido_em=datetime.now(timezone.utc))
            resposta = cliente.post("http://localhost:8000/leituras", json=pacote,
                                     headers={"X-API-Key": os.environ["API_KEY"]})
            resposta.raise_for_status()
            print(f"DEMO sequencia={i+1}: {resposta.json()['status']}")
            if i + 1 < args.quantidade:
                time.sleep(args.intervalo)


if __name__ == "__main__":
    main()
