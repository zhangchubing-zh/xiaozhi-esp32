from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
from logging.handlers import RotatingFileHandler
import socket
import socketserver
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from .config import TransferConfig


class _ThreadingTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class TransferStation:
    def __init__(self, config: TransferConfig):
        config.validate()
        self.config = config
        self.started_at = time.monotonic()
        self._tcp: _ThreadingTCPServer | None = None
        self._http: ThreadingHTTPServer | None = None
        self._threads: list[threading.Thread] = []
        self.logger = logging.getLogger(f"litecrab.transfer.{id(self)}")
        self.logger.setLevel(logging.INFO)
        self.logger.propagate = False

    def configure_logging(self) -> None:
        from pathlib import Path
        directory = Path(self.config.log_dir)
        directory.mkdir(parents=True, exist_ok=True)
        fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
        file_handler = RotatingFileHandler(directory / "transfer_station.log", maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
        file_handler.setFormatter(fmt)
        self.logger.addHandler(file_handler)
        console = logging.StreamHandler()
        console.setFormatter(fmt)
        self.logger.addHandler(console)
        self.logger.setLevel(logging.DEBUG if self.config.verbose else logging.INFO)

    def start(self) -> None:
        station = self

        class TCPHandler(socketserver.StreamRequestHandler):
            def handle(self) -> None:
                while True:
                    line = self.rfile.readline(station.config.max_request_bytes + 1)
                    if not line:
                        return
                    if len(line) > station.config.max_request_bytes:
                        self.wfile.write(b"ERROR: request too large\n")
                        return
                    try:
                        with socket.create_connection((station.config.board_host, station.config.board_port), station.config.connect_timeout) as board:
                            board.settimeout(station.config.request_timeout)
                            board.sendall(line if line.endswith(b"\n") else line + b"\n")
                            reply = bytearray()
                            while len(reply) <= station.config.max_request_bytes:
                                chunk = board.recv(min(65536, station.config.max_request_bytes + 1 - len(reply)))
                                if not chunk:
                                    break
                                reply.extend(chunk)
                                if b"\n" in chunk:
                                    break
                            if len(reply) > station.config.max_request_bytes:
                                raise ValueError("board response too large")
                            self.wfile.write(bytes(reply) if reply.endswith(b"\n") else bytes(reply) + b"\n")
                    except (OSError, ValueError) as exc:
                        station.logger.warning("board proxy failed: %s", exc)
                        self.wfile.write(b"ERROR: board unavailable\n")

        class HTTPHandler(BaseHTTPRequestHandler):
            server_version = "LiteCrabTransfer/1.0"

            def log_message(self, fmt: str, *args: object) -> None:
                station.logger.info("http " + fmt, *args)

            def _headers(self, status: int, content_type: str = "application/json") -> None:
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
                self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
                self.end_headers()

            def _json(self, status: int, value: object) -> None:
                body = json.dumps(value, ensure_ascii=False).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)

            def do_OPTIONS(self) -> None:
                self._headers(204)

            def do_GET(self) -> None:
                if self.path == "/health":
                    self._json(200, {"status": "ok"})
                elif self.path == "/status":
                    self._json(200, {"status": "running", "uptime_seconds": round(time.monotonic() - station.started_at, 3), "board": f"{station.config.board_host}:{station.config.board_port}"})
                else:
                    self._json(404, {"error": "not found"})

            def do_POST(self) -> None:
                if self.path != "/v1/chat/completions":
                    self._json(404, {"error": "not found"})
                    return
                raw_length = self.headers.get("Content-Length")
                if raw_length is None or not raw_length.isdigit():
                    self._json(400, {"error": "valid Content-Length is required"})
                    return
                length = int(raw_length)
                if length < 2 or length > station.config.max_request_bytes:
                    self._json(400, {"error": "invalid request size"})
                    return
                try:
                    payload = json.loads(self.rfile.read(length))
                    if not isinstance(payload, dict) or not isinstance(payload.get("messages"), list):
                        raise ValueError("messages must be an array")
                    if station.config.override_model:
                        payload["model"] = station.config.default_model
                    else:
                        payload.setdefault("model", station.config.default_model)
                except (json.JSONDecodeError, UnicodeDecodeError, ValueError) as exc:
                    self._json(400, {"error": str(exc)})
                    return
                data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
                headers = {"Content-Type": "application/json", "User-Agent": station.config.user_agent}
                if station.config.api_key:
                    headers["Authorization"] = f"Bearer {station.config.api_key}"
                for attempt in range(station.config.retries):
                    try:
                        with urlopen(Request(station.config.llm_url, data=data, headers=headers, method="POST"), timeout=station.config.request_timeout) as upstream:
                            response = upstream.read(station.config.max_request_bytes + 1)
                            if len(response) > station.config.max_request_bytes:
                                raise ValueError("upstream response too large")
                            self.send_response(upstream.status)
                            self.send_header("Content-Type", upstream.headers.get_content_type())
                            self.send_header("Content-Length", str(len(response)))
                            self.send_header("Access-Control-Allow-Origin", "*")
                            self.end_headers()
                            self.wfile.write(response)
                            return
                    except HTTPError as exc:
                        if exc.code < 500 or attempt + 1 == station.config.retries:
                            body = exc.read(station.config.max_request_bytes)
                            self.send_response(exc.code)
                            self.send_header("Content-Type", "application/json")
                            self.send_header("Content-Length", str(len(body)))
                            self.end_headers()
                            self.wfile.write(body)
                            return
                    except (URLError, OSError, ValueError) as exc:
                        station.logger.warning("LLM proxy attempt %d failed: %s", attempt + 1, exc)
                        if attempt + 1 == station.config.retries:
                            self._json(502, {"error": "LLM upstream unavailable"})
                            return
                    time.sleep(0.05 * (attempt + 1))

        self._tcp = _ThreadingTCPServer((self.config.listen_host, self.config.tcp_port), TCPHandler)
        self._http = ThreadingHTTPServer((self.config.listen_host, self.config.http_port), HTTPHandler)
        for server, name in ((self._tcp, "tcp"), (self._http, "http")):
            thread = threading.Thread(target=server.serve_forever, name=f"litecrab-transfer-{name}", daemon=True)
            thread.start()
            self._threads.append(thread)
        self.logger.info("transfer station started tcp=%d http=%d board=%s:%d llm=%s",
                         self.config.tcp_port, self.config.http_port,
                         self.config.board_host, self.config.board_port,
                         self.config.llm_url)

    def stop(self) -> None:
        for server in (self._tcp, self._http):
            if server:
                server.shutdown()
                server.server_close()
        for thread in self._threads:
            thread.join(timeout=5)
        self._threads.clear()

    def healthy(self) -> bool:
        return len(self._threads) == 2 and all(thread.is_alive() for thread in self._threads)
