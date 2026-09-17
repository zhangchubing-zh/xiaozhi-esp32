#!/usr/bin/env python3
import http.server
import json
import os
import socket
import subprocess
import sys
import threading
import time

MARKDOWN = '**📋 诊断总结报告**\n\n- **已执行操作**：\n\n  1. `login`\n  2. `get_history_alarm`\n'


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
            content = json.dumps({
                "action": "none", "skill": "", "confidence": 0.99,
                "reason_code": "e2e_none",
            })
            streaming = request.get("stream", False)
            event = json.dumps({"choices": [{"delta" if streaming else "message": {"content": content}}]})
            body = (f"data: {event}\n\ndata: [DONE]\n\n" if streaming else event).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream" if streaming else "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
        last = messages[-1]
        if last.get("content") == "oversize response":
            body = b"x" * (2 * 1024 * 1024 + 1)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if last.get("content") == "truncated stream":
            body = b'data: {"choices":[{"delta":{"content":"partial"}}]}\n\n'
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
        if last.get("role") == "tool":
            delta = {"content": "tool-result-ok"}
        elif last.get("content") == "markdown report":
            delta = {"content": MARKDOWN}
        elif last.get("content") == "seed durable":
            delta = {"content": "seed-ok"}
        elif last.get("content") == "check durable":
            restored = any(m.get("content") == "seed durable" for m in messages)
            delta = {"content": "restored-ok" if restored else "restored-missing"}
        elif last.get("content") == "use tool":
            delta = {"tool_calls": [{
                "index": 0,
                "id": "call_e2e",
                "function": {"name": "read", "arguments": json.dumps({"path": "docs/hub/hub_dispatch.md"})},
            }]}
        else:
            delta = {"content": "hello-ok"}
        event = json.dumps({"choices": [{"delta": delta}]}, ensure_ascii=False)
        body = f"data: {event}\n\ndata: [DONE]\n\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
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
            raise RuntimeError("server closed before newline")
        data.extend(chunk)
    return data.decode().rstrip("\r\n")


def main():
    binary, root = sys.argv[1:3]
    provider_port, gateway_port = free_port(), free_port()
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", provider_port), Provider)
    provider_thread = threading.Thread(target=provider.serve_forever, daemon=True)
    provider_thread.start()
    env = os.environ.copy()
    env.update({
        "LITECRAB_BASE_URL": f"http://127.0.0.1:{provider_port}/v1/chat/completions",
        "LITECRAB_API_KEY": "test-key",
        "LITECRAB_MODEL": "mock-model",
    })
    server = subprocess.Popen([binary, root, str(gateway_port)], cwd=root, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.time() + 5
        while True:
            try:
                conn = socket.create_connection(("127.0.0.1", gateway_port), timeout=1)
                break
            except OSError:
                if server.poll() is not None:
                    raise RuntimeError(server.stderr.read())
                if time.time() >= deadline:
                    raise RuntimeError("gateway did not start")
                time.sleep(0.05)
        with conn:
            conn.settimeout(5)
            conn.sendall(b'{"userId":"e2e","content":"hello"}\n')
            response = recv_line(conn)
            assert response == "hello-ok", repr(response)
            conn.sendall(b'plain text request\n')
            assert recv_line(conn) == "hello-ok"
            conn.sendall(b'{"userId":"e2e","sessionId":"tmp:claimed","content":"hello"}\n')
            assert recv_line(conn) == "ERROR: invalid sessionId"
            conn.sendall(b'{"userId":"e2e","content":"oversize response"}\n')
            oversized = recv_line(conn)
            assert oversized.startswith("ERROR: LLM request failed (-206)"), repr(oversized)
            conn.sendall(b'{"userId":"e2e","content":"truncated stream"}\n')
            truncated = recv_line(conn)
            assert truncated.startswith("ERROR: LLM request failed (-203)"), repr(truncated)
            conn.sendall(b'{"userId":"e2e","content":"use tool"}\n')
            assert recv_line(conn) == "tool-result-ok"
            conn.sendall(b'{"userId":"e2e","content":"markdown report","responseFormat":"json"}\n')
            envelope = json.loads(recv_line(conn))
            # The LLM adapter trims outer whitespace, but preserves Markdown structure.
            assert envelope == {"responseFormat": "json", "content": MARKDOWN.strip()}, repr(envelope)
        stable_session = f"e2e-stable-{gateway_port}"
        with socket.create_connection(("127.0.0.1", gateway_port), timeout=2) as first:
            first.settimeout(5)
            request = json.dumps({"userId": "e2e", "sessionId": stable_session,
                                  "content": "seed durable"}) + "\n"
            first.sendall(request.encode())
            assert recv_line(first) == "seed-ok"
        with socket.create_connection(("127.0.0.1", gateway_port), timeout=2) as second:
            second.settimeout(5)
            request = json.dumps({"userId": "e2e", "sessionId": stable_session,
                                  "content": "check durable"}) + "\n"
            second.sendall(request.encode())
            assert recv_line(second) == "restored-ok"
    finally:
        server.terminate()
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
        provider.shutdown()
        provider.server_close()


if __name__ == "__main__":
    main()
