#!/usr/bin/env python3
"""Gateway security-regression baseline using only local sockets and stdlib."""

import os
import socket
import subprocess
import sys
import tempfile
import time


MAX_REQUEST_BYTES = 16_384


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def recv_line(sock):
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("gateway closed before a complete response")
        data.extend(chunk)
    return data.decode().rstrip("\r\n")


def wait_for_gateway(process, port):
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            if process.poll() is not None:
                raise RuntimeError(f"gateway exited early: {process.stderr.read()}")
            time.sleep(0.05)
    raise RuntimeError("gateway did not start")


def request(port, payload):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.settimeout(2)
        connection.sendall(payload)
        return recv_line(connection)


def assert_invalid_session_rejected(port, workspace):
    response = request(
        port,
        b'{"userId":"attacker","sessionId":"../escape","content":"ignored"}\n',
    )
    assert response == "ERROR: invalid sessionId", repr(response)
    possible_escape_targets = (
        os.path.join(workspace, "escape.lcs"),
        os.path.join(workspace, ".crab", "escape.lcs"),
        os.path.join(workspace, ".crab", "sessions", "escape.lcs"),
    )
    assert all(not os.path.exists(path) for path in possible_escape_targets)


def assert_oversized_request_closes_connection(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.settimeout(2)
        connection.sendall(b"x" * MAX_REQUEST_BYTES + b"\n")
        try:
            received = connection.recv(1)
        except ConnectionResetError:
            received = b""
        assert received == b"", f"oversized request unexpectedly received: {received!r}"

    response = request(
        port,
        b'{"sessionId":"still/invalid","content":"ignored"}\n',
    )
    assert response == "ERROR: invalid sessionId", repr(response)


def write_base_config(path, listen_ip, allow_remote=None):
    body = {"server": {"listen_ip": listen_ip, "listen_port": 10003}}
    if allow_remote is not None:
        body["server"]["allow_unauthenticated_remote"] = allow_remote
    import json
    with open(path, "w", encoding="utf-8") as f:
        json.dump(body, f)


def run_unsafe_remote_rejected(binary):
    with tempfile.TemporaryDirectory(prefix="litecrab-unsafe-") as workspace:
        config_path = os.path.join(workspace, "base.json")
        write_base_config(config_path, "0.0.0.0", allow_remote=False)
        port = free_port()
        env = os.environ.copy()
        env.update({
            "LITECRAB_BASE_URL": "http://127.0.0.1:1/v1/chat/completions",
            "LITECRAB_API_KEY": "unsafe-test-key",
            "LITECRAB_MODEL": "unsafe-test-model",
            "LITECRAB_LOG_DIR": os.path.join(workspace, "logs"),
        })
        process = subprocess.Popen(
            [binary, "--config", config_path,
             "--workspace", workspace, "--port", str(port)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
            raise RuntimeError("unsafe remote config unexpectedly started the gateway")
        assert process.returncode == 2, f"expected exit code 2, got {process.returncode}"
        stderr = process.stderr.read()
        assert "unauthenticated remote TCP" in stderr, repr(stderr)
        assert "unsafe-test-key" not in stderr, "API key leaked into stderr"

        with socket.socket() as probe:
            probe.settimeout(0.2)
            try:
                probe.connect(("127.0.0.1", port))
                connected = True
            except OSError:
                connected = False
            assert not connected, "gateway should not be listening after rejection"


def run_explicit_remote_compat(binary):
    import json
    port = free_port()
    provider_port = free_port()
    import http.server
    import threading

    class StubProvider(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            self.rfile.read(length)
            body = json.dumps({"choices": [{"message": {"content": "compat-ok"}}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    provider = http.server.ThreadingHTTPServer(("127.0.0.1", provider_port), StubProvider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()

    with tempfile.TemporaryDirectory(prefix="litecrab-compat-") as workspace:
        config_path = os.path.join(workspace, "base.json")
        llm_config_path = os.path.join(workspace, "llm.json")
        write_base_config(config_path, "0.0.0.0", allow_remote=True)
        with open(llm_config_path, "w", encoding="utf-8") as f:
            json.dump({"llm": {
                "default": "compat",
                "compat": {
                    "baseUrl": f"http://127.0.0.1:{provider_port}/v1/chat/completions",
                    "apiKey": "compat-secret-key",
                    "modelName": "compat-model",
                },
                "max_tokens": 16,
                "stream": False,
            }}, f)
        env = os.environ.copy()
        env["LITECRAB_LOG_DIR"] = os.path.join(workspace, "logs")
        process = subprocess.Popen(
            [binary, "--config", config_path, "--llm-config", llm_config_path,
             "--workspace", workspace, "--port", str(port)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        log_path = os.path.join(workspace, "logs")
        try:
            wait_for_gateway(process, port)
            response = request(
                port,
                b'{"userId":"compat-user","sessionId":"compat/session","content":"ignored"}\n',
            )
            assert response == "ERROR: invalid sessionId", repr(response)

            log_contents = ""
            if os.path.isdir(log_path):
                for name in os.listdir(log_path):
                    with open(os.path.join(log_path, name), "r", encoding="utf-8", errors="replace") as f:
                        log_contents += f.read()
            assert "unauthenticated remote TCP is explicitly enabled" in log_contents, \
                "missing high-risk WARNING in logs"
            assert "compat-secret-key" not in log_contents, "API key leaked into logs"
            assert "compat-user" not in log_contents, "request userId leaked into logs"
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
            provider.shutdown()
            provider.server_close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_security_e2e.py LITECRAB_SERVER")
    binary = sys.argv[1]
    port = free_port()

    with tempfile.TemporaryDirectory(prefix="litecrab-security-e2e-") as workspace:
        env = os.environ.copy()
        env.update(
            {
                "LITECRAB_BASE_URL": "http://127.0.0.1:1/v1/chat/completions",
                "LITECRAB_API_KEY": "security-test-key",
                "LITECRAB_MODEL": "security-test-model",
                "LITECRAB_LOG_DIR": os.path.join(workspace, "logs"),
            }
        )
        process = subprocess.Popen(
            [binary, "--workspace", workspace, "--port", str(port)],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_gateway(process, port)
            assert_invalid_session_rejected(port, workspace)
            assert_oversized_request_closes_connection(port)
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)

        if process.returncode not in (-15, 0):
            raise RuntimeError(f"gateway exited unexpectedly: {process.stderr.read()}")

    run_unsafe_remote_rejected(binary)
    run_explicit_remote_compat(binary)


if __name__ == "__main__":
    main()
