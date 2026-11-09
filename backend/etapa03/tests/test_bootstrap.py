import io
import json
from urllib.parse import urlparse

from backend import bootstrap


def test_cria_tokens_restritos_e_reutiliza_no_reinicio(monkeypatch, tmp_path):
    monkeypatch.setenv("INFLUXDB_ADMIN_TOKEN", "admin-teste")
    monkeypatch.setenv("CREDENTIALS_DIR", str(tmp_path))
    autorizacoes = []
    criacoes = []

    def servidor(req, timeout):
        assert req.get_header("Authorization") == "Token admin-teste"
        path = urlparse(req.full_url).path
        if path == "/api/v2/buckets":
            resposta = {"buckets": [{"id": "bucket1", "orgID": "org1"}]}
        elif req.data is None:
            resposta = {"authorizations": autorizacoes}
        else:
            corpo = json.loads(req.data)
            criacoes.append(corpo)
            resposta = {**corpo, "id": "auth" + str(len(criacoes)),
                        "token": "token-" + str(len(criacoes)), "status": "active"}
            autorizacoes.append(resposta)
        return io.BytesIO(json.dumps(resposta).encode())

    monkeypatch.setattr(bootstrap, "urlopen", servidor)
    bootstrap.main()
    assert [c["permissions"][0]["action"] for c in criacoes] == ["write", "read"]
    assert all(c["permissions"][0]["resource"]["id"] == "bucket1" for c in criacoes)
    assert (tmp_path / "write.token").read_text() == "token-1"
    assert (tmp_path / "read.token").read_text() == "token-2"
    bootstrap.main()
    assert len(criacoes) == 2
