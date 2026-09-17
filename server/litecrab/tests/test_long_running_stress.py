#!/usr/bin/env python3
"""Bounded stress test for the SD5091 long-running runtime skeleton."""
import concurrent.futures
import http.server
import json
import os
import pathlib
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


class Provider(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(size))
        first = request.get("messages", [{}])[0].get("content", "")
        if first.startswith("# Skill Resolver"):
            content = json.dumps({"action": "none", "skill": "", "confidence": 1.0,
                                  "reason_code": "stress"})
        else:
            content = "stress-ok"
        last = request.get("messages", [{}])[-1].get("content", "")
        if last == "shutdown-hang":
            self.server.hang_started.set()
            time.sleep(10)
            return
        if request.get("stream"):
            event = json.dumps({"choices": [{"delta": {"content": content}}]})
            body = f"data: {event}\n\ndata: [DONE]\n\n".encode()
            content_type = "text/event-stream"
        else:
            body = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
            content_type = "application/json"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
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
            break
        data.extend(chunk)
    return data.decode(errors="replace").strip()


def wait_ready(process, port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"server exited early: {process.stderr.read()}")
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=.2)
            sock.close()
            time.sleep(.15)
            return
        except OSError:
            time.sleep(.03)
    raise AssertionError("gateway startup timeout")


def proc_value(pid, key):
    for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith(key + ":"):
            return int(line.split()[1])
    raise AssertionError(f"missing {key}")


def one_request(port, index):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(15)
        body = json.dumps({"userId": "stress", "sessionId": f"stress-{index % 4}",
                           "content": f"request-{index}"}) + "\n"
        sock.sendall(body.encode())
        return recv_line(sock)


def ephemeral_request(port):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(15)
        sock.sendall(b'{"userId":"stress","content":"ephemeral"}\n')
        return recv_line(sock)


def hanging_request(port):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(15)
        sock.sendall(b'{"userId":"stress","content":"shutdown-hang"}\n')
        return recv_line(sock)


def stop_bounded(process):
    started = time.monotonic()
    process.terminate()
    process.wait(timeout=3)
    assert process.returncode == 0, process.returncode
    assert time.monotonic() - started < 2.5


def main():
    binary, root = sys.argv[1:3]
    provider_port = free_port()
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", provider_port), Provider)
    provider.daemon_threads = True
    provider.hang_started = threading.Event()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    environment = os.environ.copy()
    environment.update({
        "LITECRAB_BASE_URL": f"http://127.0.0.1:{provider_port}/v1/chat/completions",
        "LITECRAB_API_KEY": "stress-key",
        "LITECRAB_MODEL": "stress-model",
        "LITECRAB_ALARM_ENABLED": "0",
    })
    with tempfile.TemporaryDirectory(prefix="litecrab-stress-") as workspace:
        port = free_port()
        process = subprocess.Popen([binary, "--workspace", workspace, "--port", str(port)],
                                   cwd=root, env=environment, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, text=True)
        try:
            wait_ready(process, port)
            idle = []
            busy = 0
            for _ in range(28):
                sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                sock.settimeout(.15)
                try:
                    if recv_line(sock) == "ERROR: server busy":
                        busy += 1
                        sock.close()
                    else:
                        idle.append(sock)
                except socket.timeout:
                    idle.append(sock)
            assert len(idle) <= 16, len(idle)
            assert busy >= 12, busy
            assert proc_value(process.pid, "Threads") <= 10
            assert len(list(pathlib.Path(f"/proc/{process.pid}/fd").iterdir())) <= 40
            for sock in idle:
                sock.close()
            time.sleep(.3)

            churn_count = int(os.environ.get("LITECRAB_STRESS_CONNECTION_CHURN", "1000"))
            for index in range(churn_count):
                churn = socket.create_connection(("127.0.0.1", port), timeout=2)
                churn.close()
                if index % 100 == 0:
                    time.sleep(.001)
            time.sleep(.3)
            assert len(list(pathlib.Path(f"/proc/{process.pid}/fd").iterdir())) <= 24
            assert ephemeral_request(port) == "stress-ok"
            time.sleep(.1)
            session_dir = pathlib.Path(workspace) / ".crab/sessions"
            assert not list(session_dir.glob("tmp:sess-*.lcs")), \
                "connection-generated sessions must not write flash"

            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                results = list(pool.map(lambda i: one_request(port, i), range(60)))
            allowed = {"stress-ok", "ERROR: request rejected", "ERROR: response timeout"}
            assert results and all(result in allowed for result in results), set(results)
            assert "stress-ok" in results
            assert proc_value(process.pid, "VmRSS") < 128 * 1024
            assert proc_value(process.pid, "Threads") <= 10
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                hanging = pool.submit(hanging_request, port)
                assert provider.hang_started.wait(timeout=3), "LLM hang was not reached"
                stop_bounded(process)
                try:
                    hanging.result(timeout=3)
                except (ConnectionError, OSError):
                    pass
            process = None

            # Repeated start/stop catches stale listener state, leaked ports and
            # shutdown paths that only fail after the first lifecycle.
            for _ in range(8):
                cycle_port = free_port()
                cycle = subprocess.Popen(
                    [binary, "--workspace", workspace, "--port", str(cycle_port)],
                    cwd=root, env=environment, stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, text=True)
                wait_ready(cycle, cycle_port)
                stop_bounded(cycle)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            provider.shutdown()
            provider.server_close()


if __name__ == "__main__":
    main()
