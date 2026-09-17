"""Verify resolver and Agent requests follow stream configuration end to end."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from test_agent_matrix import Provider, ask, free_port, wait_for_server, write_config


class ModeProvider(Provider):
    def respond(self, request, reply):
        if not request.get("stream"):
            return super().respond(request, reply)
        # Split content across SSE events so resolver JSON must be reassembled.
        content = reply["content"]
        split = len(content) // 2
        events = [json.dumps({"choices": [{"delta": {"content": part}}]})
                  for part in (content[:split], content[split:])]
        body = ("".join("data: " + event + "\n\n" for event in events)
                + "data: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


def run(binary, root, stream):
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ModeProvider)
    provider.requests, provider.authorizations = [], []
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="litecrab-resolver-") as temporary:
            config = Path(temporary) / "llm.json"
            write_config(config, provider.server_port, stream)
            env = {k: v for k, v in os.environ.items() if not k.startswith("LITECRAB_")}
            env["MATRIX_API_KEY"] = "matrix-secret"
            port = free_port()
            process = subprocess.Popen([
                binary, "--llm-config", str(config), "--workspace", root, "--port", str(port)
            ], cwd=temporary, env=env, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, text=True)
            try:
                with wait_for_server(process, port) as client:
                    client.settimeout(5)
                    for query, expected in (
                        ("route-plc", "PLC_ROUTE_OK"),
                        ("route-markdown", "MARKDOWN_ROUTE_OK"),
                        ("route-none", "BASE_FALLBACK_OK"),
                        ("route-invalid", "BASE_FALLBACK_OK"),
                    ):
                        start = len(provider.requests)
                        actual = ask(client, query)
                        assert actual == expected, (stream, query, actual)
                        requests = provider.requests[start:]
                        resolver = [r for r in requests if r["messages"][0]["content"].startswith("# Skill Resolver")]
                        ordinary = [r for r in requests if r not in resolver]
                        assert len(resolver) == 1 and ordinary, (query, requests)
                        assert "tools" not in resolver[0]
                        assert "tool_choice" not in resolver[0]
                        assert all(r.get("tools") for r in ordinary), (query, ordinary)
                        assert all(r.get("tool_choice") == "auto" for r in ordinary), (query, ordinary)
                        assert all(r.get("stream") is stream for r in requests), (query, requests)
            finally:
                process.terminate()
                try:
                    process.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join()
    print(f"resolver stream={stream}: selection, fenced JSON, no match, invalid JSON passed")


if __name__ == "__main__":
    for mode in (True, False):
        run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()), mode)
