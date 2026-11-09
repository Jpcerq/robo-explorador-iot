"""Cria tokens restritos ao bucket; somente este job recebe token administrador."""
import json
import os
from pathlib import Path
from urllib.parse import urlencode
from urllib.request import Request, urlopen


def main():
    base = os.getenv("INFLUXDB_URL", "http://influxdb:8086")
    admin = os.environ["INFLUXDB_ADMIN_TOKEN"]
    org = os.getenv("INFLUXDB_ORG", "fabrica")
    bucket = os.getenv("INFLUXDB_BUCKET", "pintura")
    pasta = Path(os.getenv("CREDENTIALS_DIR", "/credentials"))
    pasta.mkdir(parents=True, exist_ok=True)

    def api(path, payload=None):
        req = Request(base + path, data=None if payload is None else json.dumps(payload).encode(),
                      headers={"Authorization": "Token " + admin, "Content-Type": "application/json"})
        with urlopen(req, timeout=10) as response:
            return json.load(response)

    buckets = api("/api/v2/buckets?" + urlencode({"org": org, "name": bucket}))["buckets"]
    if len(buckets) != 1:
        raise RuntimeError("Bucket nao encontrado ou ambiguo.")
    item = buckets[0]
    autorizacoes = {a["id"]: a for a in api("/api/v2/authorizations")["authorizations"]}
    for acao, nome in (("write", "write"), ("read", "read")):
        arquivo = pasta / f"{nome}.json"
        token_arquivo = pasta / f"{nome}.token"
        anterior = json.loads(arquivo.read_text()) if arquivo.exists() else {}
        autorizacao = autorizacoes.get(anterior.get("id"), {})
        if (anterior.get("bucketID") == item["id"] and autorizacao.get("status") == "active"
                and token_arquivo.exists()):
            continue
        novo = api("/api/v2/authorizations", {
            "orgID": item["orgID"], "description": f"etapa03-{nome}-{bucket}",
            "permissions": [{"action": acao, "resource": {"type": "buckets", "id": item["id"],
                                                         "orgID": item["orgID"]}}],
        })
        token_arquivo.write_text(novo["token"], encoding="utf-8")
        # O Grafana roda com UID proprio e precisa ler seu token no volume compartilhado.
        token_arquivo.chmod(0o644)
        arquivo.write_text(json.dumps({"id": novo["id"], "bucketID": item["id"]}), encoding="utf-8")
    print("Tokens de escrita (API) e leitura (Grafana) prontos.")


if __name__ == "__main__":
    main()
