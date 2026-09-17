#!/usr/bin/env python3
"""Mock OpenAI-compatible chat-completions endpoint for xiaozhi e2e tests.

- Skill-resolver calls (system prompt starting with '# Skill Resolver') get a
  JSON 'no skill' decision.
- Normal calls reply MOCK_REPLY[<last user message>], so the fake device can
  verify the full pipeline (ASR text / direct chat text reaches the LLM and
  comes back).
- User messages starting with 'SLOW:' delay the response, to exercise the
  gateway's abort/cancel path.
"""
import http.server
import json
import os
import sys
import threading
import time


def make_handler(delay_seconds=0.0):
    class Provider(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        delay = delay_seconds

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))
            messages = request.get("messages", [])
            streaming = bool(request.get("stream", False))
            tools = request.get("tools") or []
            log_path = os.environ.get("MOCK_LLM_LOG")
            if log_path:
                with open(log_path, "a", encoding="utf-8") as fp:
                    users = [m.get("content", "")[:60] for m in messages if m.get("role") == "user"]
                    tool_names = [t.get("function", {}).get("name", "")
                                  for t in tools]
                    fp.write(f"{time.time():.3f} stream={streaming} tools={tool_names} users={users}\n")
            resolver = any(
                m.get("role") == "system" and m.get("content", "").startswith("# Skill Resolver")
                for m in messages
            )
            tool_results = [m for m in messages if m.get("role") == "tool"]
            fresh_tool_result = bool(messages) and messages[-1].get("role") == "tool"
            device_tools = [t for t in tools
                            if "otto.action" in t.get("function", {}).get("name", "")]
            user_text = next((m.get("content", "") for m in reversed(messages)
                              if m.get("role") == "user"), "")
            if resolver:
                content = json.dumps(
                    {"action": "none", "skill": "", "confidence": 0.99, "reason_code": "e2e_none"},
                    ensure_ascii=False,
                )
                kind = "text"
            elif user_text.startswith("SLOW:"):
                # Current-turn slow response (based on the latest user message,
                # so tool results from earlier turns in the shared session
                # history do not mask the delay).
                time.sleep(self.delay)
                content = f"MOCK_REPLY[{user_text}]"
                kind = "text"
            elif fresh_tool_result:
                # The kernel re-calls the LLM right after executing a tool:
                # the last message is the fresh tool result.
                content = f"ROBOT_DONE[{messages[-1].get('content', '')}]"
                kind = "text"
            elif device_tools and ("前进" in user_text or "forward" in user_text.lower()):
                kind = "tool_call"
            else:
                content = f"MOCK_REPLY[{user_text}]"
                kind = "text"
            if kind == "tool_call":
                tool_call = {
                    "index": 0,
                    "id": "call_e2e_otto",
                    "type": "function",
                    "function": {
                        "name": "self.otto.action",
                        "arguments": json.dumps({"action": "walk", "steps": 3},
                                                ensure_ascii=False),
                    },
                }
                if streaming:
                    event = json.dumps({"choices": [{"delta": {"tool_calls": [tool_call]}}]},
                                       ensure_ascii=False)
                    body = f"data: {event}\n\ndata: [DONE]\n\n".encode()
                    content_type = "text/event-stream"
                else:
                    body = json.dumps(
                        {"choices": [{"message": {"tool_calls": [tool_call]}}],
                         "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}},
                        ensure_ascii=False,
                    ).encode()
                    content_type = "application/json"
            else:
                if streaming:
                    event = json.dumps({"choices": [{"delta": {"content": content}}]},
                                       ensure_ascii=False)
                    body = f"data: {event}\n\ndata: [DONE]\n\n".encode()
                    content_type = "text/event-stream"
                else:
                    body = json.dumps(
                        {"choices": [{"message": {"content": content}}],
                         "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}},
                        ensure_ascii=False,
                    ).encode()
                    content_type = "application/json"
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    return Provider


def start(port=0, delay_seconds=0.0):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), make_handler(delay_seconds))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    srv, bound = start(port)
    print(f"mock_llm listening on 127.0.0.1:{bound}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        srv.shutdown()
