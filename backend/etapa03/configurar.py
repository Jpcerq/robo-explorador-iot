"""Gera segredos locais uma unica vez; nao altera volumes ou credenciais existentes."""
import secrets
from ambiente import RAIZ


def main():
    arquivo = RAIZ / ".env"
    if arquivo.exists():
        print(".env ja existe; configuracao preservada.")
        return
    valores = {
        "INFLUXDB_USERNAME": "admin",
        "INFLUXDB_PASSWORD": secrets.token_hex(16),
        "INFLUXDB_ADMIN_TOKEN": secrets.token_hex(32),
        "GRAFANA_USERNAME": "admin",
        "GRAFANA_PORT": "3001",
        "GRAFANA_PASSWORD": secrets.token_hex(16),
        "API_KEY": secrets.token_hex(32),
    }
    with arquivo.open("x", encoding="utf-8") as f:
        f.write("# Credenciais locais. Nao publicar este arquivo.\n")
        f.writelines(f"{nome}={valor}\n" for nome, valor in valores.items())
    print(".env criado. Senhas do InfluxDB e Grafana estao nesse arquivo.")
    print("Inicie: docker compose up -d --build")


if __name__ == "__main__":
    main()
