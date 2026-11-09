"""Verificacao real apos subir os containers. Insere um ponto DEMO identificado."""
from datetime import datetime, timezone
import json
import os
import time

import httpx

from ambiente import RAIZ, carregar_env
from ponte_serial import interpretar_linha


def main():
    carregar_env()
    auth = (os.environ["GRAFANA_USERNAME"], os.environ["GRAFANA_PASSWORD"])
    grafana_url = "http://localhost:" + os.getenv("GRAFANA_PORT", "3001")
    with httpx.Client(timeout=30, trust_env=False) as cliente:
        print("Aguardando o Grafana ficar pronto...")
        limite = time.monotonic() + 45
        while True:
            try:
                saude = cliente.get(grafana_url + "/api/health", timeout=2)
                saude.raise_for_status()
                if saude.json().get("database") == "ok":
                    break
            except (httpx.HTTPError, ValueError):
                pass
            if time.monotonic() >= limite:
                raise SystemExit("Grafana ainda nao esta pronto. Confira docker compose logs grafana e repita a verificacao.")
            time.sleep(1)
        pronto = cliente.get("http://localhost:8000/ready", headers={"X-API-Key": os.environ["API_KEY"]})
        pronto.raise_for_status()
        print("OK: API e InfluxDB acessiveis.")

        pacote = json.loads((RAIZ / "exemplo_pacote.json").read_text())
        pacote["sequencia"] = 424242
        pacote["presenca"] = 1  # Apenas no dispositivo de DEMO.
        payload = interpretar_linha(json.dumps(pacote), "esp32-s3-demo", datetime.now(timezone.utc))
        gravacao = cliente.post("http://localhost:8000/leituras", json=payload,
                                headers={"X-API-Key": os.environ["API_KEY"]})
        gravacao.raise_for_status()
        assert gravacao.json()["status"] == "persistido"

        token = {"Authorization": "Token " + os.environ["INFLUXDB_ADMIN_TOKEN"]}
        consulta = cliente.post("http://localhost:8086/api/v2/query", params={"org": "fabrica"},
            headers={**token, "Content-Type": "application/vnd.flux", "Accept": "application/csv"},
            content='from(bucket:"pintura") |> range(start:-1m) |> filter(fn:(r) => r._measurement == "pintura" and r.dispositivo == "esp32-s3-demo" and r._field == "sequencia" and r._value == 424242)')
        consulta.raise_for_status()
        assert "424242" in consulta.text, "Ponto nao encontrado na consulta ao InfluxDB."
        print("OK: leitura confirmada por consulta ao InfluxDB real.")

        buckets = cliente.get("http://localhost:8086/api/v2/buckets", params={"org":"fabrica", "name":"pintura"}, headers=token)
        buckets.raise_for_status()
        assert buckets.json()["buckets"][0]["retentionRules"][0]["everySeconds"] == 30 * 86400
        print("OK: bucket pintura com retencao de 30 dias.")

        health = cliente.get(grafana_url + "/api/datasources/uid/influx-pintura/health", auth=auth)
        health.raise_for_status()
        assert health.json().get("status") == "OK", health.text
        painel = cliente.get(grafana_url + "/api/dashboards/uid/pintura-etapa03", auth=auth)
        painel.raise_for_status()
        dashboard = painel.json()["dashboard"]
        assert dashboard["refresh"] == "2s"
        print("OK: datasource Flux e dashboard provisionados no Grafana.")

        # Executa as consultas reais dos paineis contra o plugin do Grafana.
        consultas = []
        for p in dashboard["panels"]:
            for t in p.get("targets", []):
                consultas.append({**t, "refId": f"P{p['id']}{t['refId']}",
                    "query": t["query"].replace("${dispositivo}", "esp32-s3-demo"),
                    "intervalMs": 2000, "maxDataPoints": 500})
        agora = int(time.time() * 1000)
        resultado = cliente.post(grafana_url + "/api/ds/query", auth=auth,
            json={"from": str(agora - 900000), "to": str(agora), "queries": consultas})
        resultado.raise_for_status()
        for ref, item in resultado.json()["results"].items():
            assert not item.get("error"), f"Consulta {ref}: {item.get('error')}"
        print(f"OK: {len(consultas)} consultas do dashboard executadas sem erro.")
        print("Verificacao concluida. O ponto inserido e de DEMONSTRACAO.")


if __name__ == "__main__":
    main()
