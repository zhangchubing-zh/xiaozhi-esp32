#!/usr/bin/env python3
import http.server
import json
import os
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
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        messages = request["messages"]
        if messages and messages[0].get("content", "").startswith("# Skill Resolver"):
            content = json.dumps({"action": "none", "skill": "", "confidence": 0.99,
                                  "reason_code": "restart_none"})
            streaming = request.get("stream", False)
            event = json.dumps({"choices": [{"delta" if streaming else "message": {"content": content}}]})
            body = (f"data: {event}\n\ndata: [DONE]\n\n" if streaming else event).encode()
            content_type = "text/event-stream" if streaming else "application/json"
        else:
            last = messages[-1].get("content")
            if last == "seed-before-restart":
                output = "seeded"
            elif last == "probe-after-restart":
                found = any(item.get("content") == "seed-before-restart" for item in messages)
                output = "restart-restored" if found else "restart-lost"
            else:
                output = "ok"
            event = json.dumps({"choices": [{"delta": {"content": output}}]})
            body = f"data: {event}\n\ndata: [DONE]\n\n".encode()
            content_type = "text/event-stream"
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
            raise RuntimeError("server closed before response")
        data.extend(chunk)
    return data.decode().rstrip("\r\n")


def start_server(binary, workspace, provider_port):
    gateway_port = free_port()
    env = os.environ.copy()
    env.update({
        "LITECRAB_BASE_URL": f"http://127.0.0.1:{provider_port}/v1/chat/completions",
        "LITECRAB_API_KEY": "test-key",
        "LITECRAB_MODEL": "mock-model",
        "LITECRAB_LOG_DIR": os.path.join(workspace, "logs"),
    })
    process = subprocess.Popen([binary, "--workspace", workspace, "--port", str(gateway_port)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               text=True)
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", gateway_port), timeout=0.2):
                return process, gateway_port
        except OSError:
            if process.poll() is not None:
                raise RuntimeError(process.stderr.read())
            time.sleep(0.05)
    process.terminate()
    raise RuntimeError("server did not start")


def stop_server(process):
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def ask(port, user, session, content):
    request = json.dumps({"userId": user, "sessionId": session, "content": content}) + "\n"
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.settimeout(5)
        sock.sendall(request.encode())
        return recv_line(sock)


def main():
    binary = os.path.abspath(sys.argv[1])
    provider_port = free_port()
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", provider_port), Provider)
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="litecrab-session-restart-") as workspace:
            first, first_port = start_server(binary, workspace, provider_port)
            try:
                assert ask(first_port, "owner-a", "stable-session", "seed-before-restart") == "seeded"
            finally:
                stop_server(first)
            snapshot = os.path.join(workspace, ".crab", "sessions", "stable-session.lcs")
            assert os.path.isfile(snapshot) and os.path.getsize(snapshot) > 64

            second, second_port = start_server(binary, workspace, provider_port)
            try:
                assert ask(second_port, "owner-a", "stable-session",
                           "probe-after-restart") == "restart-restored"
                denied = ask(second_port, "owner-b", "stable-session", "probe-after-restart")
                assert denied == "ERROR: session is owned by another user", denied
                invalid = ask(second_port, "owner-a", "../escape", "probe-after-restart")
                assert invalid == "ERROR: invalid sessionId", invalid
            finally:
                stop_server(second)
    finally:
        provider.shutdown()
        provider.server_close()


if __name__ == "__main__":
    main()
