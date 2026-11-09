"""Contrato compativel com o JSON do transmissor ESP32-S3 fornecido."""
from datetime import datetime, timezone
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, StrictInt, field_validator


class Leitura(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)

    versao: Literal[1]
    sequencia: int = Field(ge=0, le=4294967295, strict=True)
    nivel_tinta: float | None = Field(ge=0, le=100)
    temperatura: float | None = Field(ge=-40, le=80)
    umidade: float | None = Field(ge=0, le=100)
    luminosidade: int = Field(ge=0, le=4095, strict=True)
    presenca: StrictInt | None = Field(default=None, ge=0, le=1)
    timestamp: int = Field(ge=0, le=9223372036854775807, strict=True)
    relogio: Literal["uptime_ms"]
    dispositivo: str = Field(default="esp32-s3-fabrica", pattern=r"^[a-zA-Z0-9_-]{1,48}$")
    # Hora de chegada ao PC: nao e a hora exata da coleta no ESP32.
    recebido_em: datetime

    @field_validator("versao", mode="before")
    @classmethod
    def versao_inteira(cls, value):
        if type(value) is not int:
            raise ValueError("versao precisa ser o inteiro 1.")
        return value

    @field_validator("nivel_tinta", "temperatura", "umidade", mode="before")
    @classmethod
    def numero_real(cls, value):
        if value is not None and (isinstance(value, bool) or not isinstance(value, (int, float))):
            raise ValueError("Use numero JSON ou null.")
        return value

    @field_validator("recebido_em", mode="before")
    @classmethod
    def somente_data_iso(cls, value):
        if not isinstance(value, (str, datetime)):
            raise ValueError("Use data/hora ISO 8601 com fuso horario.")
        return value

    @field_validator("recebido_em")
    @classmethod
    def data_com_fuso(cls, value):
        if value.tzinfo is None or value.utcoffset() is None:
            raise ValueError("recebido_em precisa informar o fuso, por exemplo Z.")
        if value.year < 2000:
            raise ValueError("Data de recepcao invalida; confira o relogio do PC.")
        return value.astimezone(timezone.utc)

def para_line_protocol(leitura: Leitura) -> str:
    """Mesmo dispositivo + instante = mesmo ponto nas tentativas de reenvio."""
    campos = {
        "sequencia": f"{leitura.sequencia}i",
        "uptime_ms": f"{leitura.timestamp}i",
        "luminosidade": f"{leitura.luminosidade}i",
        "presenca_disponivel": "true" if leitura.presenca is not None else "false",
    }
    falhas = 0
    for campo in ("nivel_tinta", "temperatura", "umidade"):
        valor = getattr(leitura, campo)
        campos[campo + "_valido"] = "true" if valor is not None else "false"
        if valor is None:
            falhas += 1
        else:
            campos[campo] = repr(float(valor))
    campos["falhas_sensores"] = f"{falhas}i"
    if leitura.presenca is not None:
        campos["presenca"] = f"{leitura.presenca}i"
    # Microssegundos inteiros, sem arredondamento de ponto flutuante.
    delta = leitura.recebido_em - datetime(1970, 1, 1, tzinfo=timezone.utc)
    instante_us = (delta.days * 86400 + delta.seconds) * 1000000 + delta.microseconds
    tags = f"dispositivo={leitura.dispositivo},relogio=uptime_ms,origem_tempo=recepcao_pc"
    return f"pintura,{tags} " + ",".join(f"{k}={v}" for k, v in campos.items()) + f" {instante_us}"
