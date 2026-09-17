#!/usr/bin/env python3
"""Verify the formal SD5091 artifact is one binary using board-owned data."""
import hashlib
import http.server
import json
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_listening(process, port, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"server exited early: {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                return
        except OSError:
            time.sleep(.03)
    raise AssertionError("installed server did not listen")


def files_below(root):
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())


class Provider(http.server.BaseHTTPRequestHandler):
    requests = []

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        self.requests.append(request)
        messages = request.get("messages", [])
        if messages and messages[0].get("content", "").startswith("# Skill Resolver"):
            content = json.dumps({"action": "none", "skill": "", "confidence": .99,
                                  "reason_code": "deployment_contract"})
        else:
            content = "external-config-and-skill-ok"
        body = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def recv_line(sock):
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            raise AssertionError("gateway closed before response")
        data.extend(chunk)
    return data.decode().rstrip("\r\n")


def main():
    cmake, build_dir, built_binary = sys.argv[1:4]
    with tempfile.TemporaryDirectory(prefix="litecrab-deployment-contract-") as temp:
        root = pathlib.Path(temp)
        install_root = root / "artifact"
        subprocess.run([cmake, "--install", build_dir, "--prefix", str(install_root)],
                       check=True, capture_output=True, text=True)
        assert files_below(install_root) == ["bin/litecrab_server"], files_below(install_root)

        installed = install_root / "bin/litecrab_server"
        assert installed.stat().st_mode & 0o111
        assert hashlib.sha256(installed.read_bytes()).digest() == \
               hashlib.sha256(pathlib.Path(built_binary).read_bytes()).digest()

        # These paths model files already provisioned and owned by the board;
        # none of them may be copied into the formal artifact by installation
        # or by server startup.
        board = root / "board"
        config_dir = board / "config"
        skill_dir = board / "skills/example"
        log_dir = board / "logs"
        config_dir.mkdir(parents=True)
        skill_dir.mkdir(parents=True)
        skill_text = ("---\nname: external_board_skill\n"
                      "description: deployment contract marker\n---\n"
                      "EXTERNAL_BOARD_SKILL_CONTENT\n")
        (skill_dir / "SKILL.md").write_text(skill_text, encoding="utf-8")
        port = free_port()
        provider_port = free_port()
        Provider.requests = []
        provider = http.server.ThreadingHTTPServer(("127.0.0.1", provider_port), Provider)
        provider_thread = threading.Thread(target=provider.serve_forever, daemon=True)
        provider_thread.start()
        base_config = config_dir / "base.json"
        llm_config = config_dir / "llm.json"
        base_config.write_text(json.dumps({
            "server": {"listen_ip": "127.0.0.1", "listen_port": port,
                       "recv_timeout_ms": 1000, "send_timeout_ms": 1000,
                       "max_req_bytes": 16384, "worker_threads": 2,
                       "max_connections": 4},
            "log": {"dir": str(log_dir), "level": "INFO"},
            "workspace_root": str(board),
        }), encoding="utf-8")
        llm_config.write_text(json.dumps({"llm": {
            "default": "test",
            "test": {"baseUrl": f"http://127.0.0.1:{provider_port}/v1/chat/completions",
                     "apiKey": "deployment-contract-only", "modelName": "mock"},
            "timeout_ms": 1000, "stream": False,
        }}), encoding="utf-8")

        process = subprocess.Popen([
            str(installed), "--config", str(base_config), "--llm-config", str(llm_config),
            "--workspace", str(board), "--no-alarm",
        ], cwd=board, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            wait_listening(process, port)
            with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
                connection.settimeout(5)
                connection.sendall(b'{"userId":"deployment-test","content":"hello"}\n')
                assert recv_line(connection) == "external-config-and-skill-ok"
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=5)
            assert process.returncode == 0, (stdout, stderr)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            provider.shutdown()
            provider.server_close()

        assert (skill_dir / "SKILL.md").read_text(encoding="utf-8") == skill_text
        request_text = json.dumps(Provider.requests, ensure_ascii=False)
        assert "external_board_skill" in request_text, request_text
        assert files_below(install_root) == ["bin/litecrab_server"], files_below(install_root)


if __name__ == "__main__":
    main()
