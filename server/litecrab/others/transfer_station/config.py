from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
from urllib.parse import urlparse


@dataclass(frozen=True)
class TransferConfig:
    listen_host: str = "127.0.0.1"
    tcp_port: int = 10002
    http_port: int = 10001
    board_host: str = "127.0.0.1"
    board_port: int = 10003
    llm_url: str = "http://127.0.0.1:18080/v1/chat/completions"
    api_key: str = ""
    default_model: str = "litecrab"
    connect_timeout: float = 10.0
    request_timeout: float = 200.0
    max_request_bytes: int = 1024 * 1024
    retries: int = 3
    log_dir: str = "logs"
    verbose: bool = False
    user_agent: str = "LiteCrabTransfer/1.0"
    override_model: bool = True

    def validate(self) -> None:
        for name, port in (("tcp_port", self.tcp_port), ("http_port", self.http_port), ("board_port", self.board_port)):
            if not 1 <= port <= 65535:
                raise ValueError(f"{name} must be between 1 and 65535")
        if self.tcp_port == self.http_port:
            raise ValueError("tcp_port and http_port must be different")
        parsed = urlparse(self.llm_url)
        if parsed.scheme not in ("http", "https") or not parsed.hostname:
            raise ValueError("llm_url must be an absolute HTTP(S) URL")
        if not self.default_model:
            raise ValueError("default_model is required")
        if not 1 <= self.max_request_bytes <= 64 * 1024 * 1024:
            raise ValueError("max_request_bytes is outside the safe range")
        if not 1 <= self.retries <= 10:
            raise ValueError("retries must be between 1 and 10")


def _flatten(data: dict) -> dict:
    server = data.get("server", {})
    board = data.get("board", data.get("litecrab", {}))
    llm = data.get("llm", {})
    logging = data.get("log", data.get("logging", {}))
    return {
        "listen_host": server.get("listen_host", data.get("listen_host", "127.0.0.1")),
        "tcp_port": server.get("tcp_port", data.get("tcp_port", 10002)),
        "http_port": server.get("http_port", data.get("http_port", 10001)),
        "board_host": board.get("host", data.get("board_host", "127.0.0.1")),
        "board_port": board.get("port", data.get("board_port", 10003)),
        "llm_url": llm.get("url", llm.get("base_url", data.get("llm_url", "http://127.0.0.1:18080/v1/chat/completions"))),
        "api_key": llm.get("api_key", data.get("api_key", "")),
        "default_model": llm.get("model", data.get("default_model", "litecrab")),
        "connect_timeout": data.get("connect_timeout", 10.0),
        "request_timeout": data.get("request_timeout", 200.0),
        "max_request_bytes": data.get("max_request_bytes", 1024 * 1024),
        "retries": data.get("retries", 3),
        "log_dir": logging.get("dir", data.get("log_dir", "logs")),
        "verbose": data.get("verbose", False),
        "user_agent": data.get("user_agent", "LiteCrabTransfer/1.0"),
        "override_model": data.get("override_model", True),
    }


def load_config(path: str | os.PathLike[str]) -> TransferConfig:
    with Path(path).open("r", encoding="utf-8") as stream:
        raw = json.load(stream)
    if not isinstance(raw, dict):
        raise ValueError("configuration root must be an object")
    values = _flatten(raw)
    overrides = {
        "api_key": os.getenv("LITECRAB_API_KEY"),
        "llm_url": os.getenv("LITECRAB_BASE_URL"),
        "default_model": os.getenv("LITECRAB_MODEL"),
    }
    values.update({key: value for key, value in overrides.items() if value})
    config = TransferConfig(**values)
    config.validate()
    return config
