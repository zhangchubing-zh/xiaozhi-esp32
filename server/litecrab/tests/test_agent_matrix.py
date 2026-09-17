#!/usr/bin/env python3
"""Black-box Agent matrix: config, messages, tools, skills, and error handling."""
from __future__ import annotations

import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

CHECKS = 0

def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(message)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def last_user(messages):
    for message in reversed(messages):
        if message.get("role") == "user":
            return message.get("content", "")
    return ""


SKILL_INVENTORY_QUERIES = (
    "当前有哪些skill",
    "有哪些 Skill",
    "请列出available skills",
    "what skills are available?",
    "skill列表",
    "支持哪些 skill",
)

BASE_ROUTE_QUERIES = SKILL_INVENTORY_QUERIES + (
    "当前可以执行哪些指令",
    "你能做什么",
    "有哪些能力",
    "available commands",
    "what can you do",
)

EVIDENCE_NORMALIZATION_CASES = {
    "我需要使用PLC_Diagnosis  这个skill查询历史告警":
        "我需要使用PLC_Diagnosis 这个skill查询历史告警",
    "我需要使用PLC_Diagnosis\t这个skill查询历史告警":
        "我需要使用PLC_Diagnosis 这个skill查询历史告警",
    "我需要使用PLC_Diagnosis\n这个skill查询历史告警":
        "我需要使用PLC_Diagnosis 这个skill查询历史告警",
    "我需要使用PLC_Diagnosis\u3000这个skill查询历史告警":
        "我需要使用PLC_Diagnosis 这个skill查询历史告警",
    "我需要使用PLC_DIAGNOSIS这个skill查询历史告警":
        "我需要使用plc_diagnosis这个skill查询历史告警",
}


class Provider(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        self.server.requests.append(request)
        self.server.authorizations.append(self.headers.get("Authorization"))
        messages = request["messages"]
        prompt = last_user(messages)
        if messages and messages[0].get("role") == "system" and messages[0].get("content", "").startswith("# Skill Resolver"):
            route_prompt = prompt.rsplit("User: ", 1)[-1]
            if route_prompt == "route-invalid":
                self.respond(request, {"content": "not-json"})
                return
            selected = ("workspace_summary" if route_prompt in
                         ("skill", "skill-suspend", "route-low-confidence", "route-markdown",
                          "总结当前项目和 README", "route-empty-evidence", "route-bad-evidence") else
                        "PLC_Diagnosis" if route_prompt in
                        ("route-plc", "PLC 通信失败，帮我诊断") or
                        route_prompt in EVIDENCE_NORMALIZATION_CASES else
                        "not_eligible" if route_prompt == "route-unknown" else "")
            confidence = (0.2 if route_prompt == "route-low-confidence" else
                          1.1 if route_prompt == "route-confidence-high" else
                          -0.1 if route_prompt == "route-confidence-negative" else 0.99)
            payload = json.dumps({
                "selected_skill": ("" if route_prompt == "route-empty-skill" else
                                   selected or None),
                "confidence": confidence,
                "reason_code": "mock_semantic_match" if selected else "mock_no_match",
                "evidence": ("" if route_prompt == "route-empty-evidence" else
                             "not present in user text" if route_prompt == "route-bad-evidence" else
                             EVIDENCE_NORMALIZATION_CASES[route_prompt]
                             if route_prompt in EVIDENCE_NORMALIZATION_CASES else
                             route_prompt if selected else ""),
            })
            if route_prompt == "route-missing-selection":
                payload = json.dumps({
                    "confidence": 0.99, "reason_code": "missing_selection", "evidence": ""
                })
            self.respond(request, {"content": f"```json\n{payload}\n```" if route_prompt == "route-markdown" else payload})
            return
        current_turn = []
        for message in reversed(messages):
            if message.get("role") == "user":
                break
            current_turn.append(message)
        tool_messages = [m for m in reversed(current_turn) if m.get("role") == "tool"]
        if prompt == "tool-read" and not tool_messages:
            reply = {"tool_calls": [self.call("read-1", "read", {"path": "CMakeLists.txt"})]}
        elif prompt == "tool-read":
            ok = "project(LiteCrab" in tool_messages[-1].get("content", "")
            reply = {"content": "TOOL_READ_OK" if ok else "TOOL_READ_BAD"}
        elif prompt == "multi-tool" and not tool_messages:
            reply = {"tool_calls": [
                self.call("grep-1", "grep", {"pattern": "project(LiteCrab", "path": "."}, 0),
                self.call("glob-1", "glob", {"pattern": "*.md", "path": "docs"}, 1),
                self.call("read-1", "read", {"path": "README.md"}, 2),
            ]}
        elif prompt == "multi-tool":
            contents = "\n".join(m.get("content", "") for m in tool_messages[-3:])
            ok = all(marker in contents for marker in ("CMakeLists.txt", "runtime", "LiteCrab"))
            reply = {"content": "MULTI_TOOL_OK" if ok else "MULTI_TOOL_BAD"}
        elif prompt in BASE_ROUTE_QUERIES:
            system = messages[0].get("content", "")
            tool_names = {(t.get("function") or {}).get("name", "") for t in request.get("tools", [])}
            ok = ("no specialized skill is required" in system
                  and "Available runtime skills" in system
                  and "usage example:" in system
                  and "workspace_summary" in system and "PLC_Diagnosis" in system
                  and "skill_read" not in tool_names and "skill_complete" not in tool_names)
            reply = {"content": "BASE_ROUTE_OK" if ok else "BASE_ROUTE_BAD"}
        elif prompt in ("route-none", "route-invalid", "route-unknown", "route-low-confidence",
                        "route-confidence-high", "route-confidence-negative",
                        "route-empty-skill", "route-missing-selection", "route-empty-evidence",
                        "route-bad-evidence", "解释一下 PLC 是什么",
                        "不要执行 PLC 诊断，只解释通信原理", "workspace_summary 是什么"):
            system = messages[0].get("content", "")
            tool_names = {(t.get("function") or {}).get("name", "") for t in request.get("tools", [])}
            ok = ("no specialized skill is required" in system
                  and "skill_read" not in tool_names and "skill_complete" not in tool_names)
            reply = {"content": "BASE_FALLBACK_OK" if ok else "BASE_FALLBACK_BAD"}
        elif prompt == "route-none-bypass" and not tool_messages:
            reply = {"tool_calls": [self.call(
                "bypass-1", "skill_read", {"skillPath": "PLC_Diagnosis"}
            )]}
        elif prompt == "route-none-bypass":
            blocked = "skill was not selected by router" in tool_messages[-1].get("content", "")
            reply = {"content": "BYPASS_BLOCKED" if blocked else "BYPASS_ALLOWED"}
        elif prompt == "skill" and not tool_messages:
            routed = "Skill Router decision: select `workspace_summary`" in messages[0].get("content", "")
            reply = ({"tool_calls": [self.call(
                "skill-1", "skill_read", {"skillPath": "workspace_summary"}
            )]} if routed else {"content": "SKILL_ROUTE_NOT_APPLIED"})
        elif prompt == "skill":
            if len(tool_messages) == 1:
                reply = {"tool_calls": [self.call(
                    "skill-readme", "read", {"path": "README.md"}
                )]}
            elif len(tool_messages) == 2:
                ok = "LiteCrab" in tool_messages[-1].get("content", "")
                reply = {"tool_calls": [self.call(
                    "skill-complete", "skill_complete", {"summary": "matrix complete"}
                )]} if ok else {"content": "SKILL_BAD"}
            else:
                completed = "matrix complete" in tool_messages[-1].get("content", "")
                reply = {"content": "SKILL_OK" if completed else "SKILL_BAD"}
        elif prompt == "skill-suspend" and not tool_messages:
            routed = "Skill Router decision: select `workspace_summary`" in messages[0].get("content", "")
            reply = ({"tool_calls": [self.call(
                "suspend-read", "skill_read", {"skillPath": "workspace_summary"}
            )]} if routed else {"content": "SUSPEND_ROUTE_BAD"})
        elif prompt == "skill-suspend":
            reply = {"content": "SKILL_SUSPENDED"}
        elif prompt == "new-unrelated-task" and not tool_messages:
            reply = {"tool_calls": [self.call(
                "new-task-complete", "skill_complete", {"summary": "must not be active"}
            )]}
        elif prompt == "new-unrelated-task":
            inactive = "no active skill run" in tool_messages[-1].get("content", "")
            reply = {"content": "NEW_TASK_OK" if inactive else "AUTO_RESUME_BAD"}
        elif prompt in ("continue", "resume-exact") and not tool_messages:
            reply = {"tool_calls": [self.call(
                "resume-complete", "skill_complete", {"summary": "resume complete"}
            )]}
        elif prompt in ("continue", "resume-exact"):
            resumed = "resume complete" in tool_messages[-1].get("content", "")
            label = "RESUME_INTENT_OK" if prompt == "continue" else "EXACT_RESUME_OK"
            reply = {"content": label if resumed else "RESUME_BAD"}
        elif prompt == "isolation-a-seed":
            reply = {"content": "ISOLATION_A_SEEDED"}
        elif prompt == "isolation-b-probe":
            leaked = any(m.get("content") in ("isolation-a-seed", "ISOLATION_A_SEEDED")
                         for m in messages[:-1])
            reply = {"content": "ISOLATION_LEAK" if leaked else "ISOLATION_B_OK"}
        elif prompt == "isolation-a-check":
            has_a = any(m.get("content") == "isolation-a-seed" for m in messages)
            has_b = any(m.get("content") == "isolation-b-probe" for m in messages)
            reply = {"content": "ISOLATION_A_OK" if has_a and not has_b else "ISOLATION_A_BAD"}
        elif prompt == "isolation-b-check":
            has_b = any(m.get("content") == "isolation-b-probe" for m in messages)
            has_a = any(m.get("content") == "isolation-a-seed" for m in messages)
            reply = {"content": "ISOLATION_B_STILL_OK" if has_b and not has_a else "ISOLATION_B_BAD"}
        elif prompt == "multiline-response":
            reply = {"content": "\n\nLINE_ONE\nLINE_TWO\r\n"}
        elif prompt == "route-markdown":
            routed = "Skill Router decision: select `workspace_summary`" in messages[0].get("content", "")
            reply = {"content": "MARKDOWN_ROUTE_OK" if routed else "MARKDOWN_ROUTE_BAD"}
        elif prompt == "route-plc":
            routed = "Skill Router decision: select `PLC_Diagnosis`" in messages[0].get("content", "")
            reply = {"content": "PLC_ROUTE_OK" if routed else "PLC_ROUTE_BAD"}
        elif prompt in ("总结当前项目和 README", "PLC 通信失败，帮我诊断"):
            wanted = "workspace_summary" if prompt.startswith("总结") else "PLC_Diagnosis"
            system = messages[0].get("content", "")
            tool_names = {(t.get("function") or {}).get("name", "") for t in request.get("tools", [])}
            routed = (f"Skill Router decision: select `{wanted}`" in system
                      and "skill_read" in tool_names and "skill_complete" in tool_names)
            reply = {"content": "POSITIVE_ROUTE_OK" if routed else "POSITIVE_ROUTE_BAD"}
        elif prompt in EVIDENCE_NORMALIZATION_CASES:
            system = messages[0].get("content", "")
            routed = "Skill Router decision: select `PLC_Diagnosis`" in system
            reply = {"content": "NORMALIZED_EVIDENCE_OK" if routed else
                                "NORMALIZED_EVIDENCE_BAD"}
        elif prompt == "bad-tool-args" and not tool_messages:
            reply = {"tool_calls": [self.call("bad-1", "read", {"unknown": True})]}
        elif prompt == "bad-tool-args":
            ok = "tool parse error" in tool_messages[-1].get("content", "")
            reply = {"content": "BAD_TOOL_HANDLED" if ok else "BAD_TOOL_UNHANDLED"}
        elif prompt == "disabled-tool" and not tool_messages:
            reply = {"tool_calls": [self.call(
                "disabled-1", "shell", {"script": "printf disabled-tool-ran"}
            )]}
        elif prompt == "disabled-tool":
            last = tool_messages[-1].get("content", "") if tool_messages else ""
            ok = "success: false" in last and "tool is disabled" in last
            reply = {"content": "SHELL_DISABLED_OK" if ok else "SHELL_DISABLED_BAD"}
        elif prompt == "exec-plc-help" and not tool_messages:
            reply = {"tool_calls": [self.call("plc-help-1", "exec_program", {
                "command": "main.wsl-test",
                "path": "skills/PLC_Diagnosis/scripts",
                "args": ["--help"],
            })]}
        elif prompt == "exec-plc-help":
            last = tool_messages[-1].get("content", "") if tool_messages else ""
            ok = "success: true" in last and "Usage:" in last
            reply = {"content": "EXEC_PLC_OK" if ok else "EXEC_PLC_BAD"}
        elif prompt == "history-two":
            has_user = any(m.get("content") == "history-one" for m in messages)
            has_answer = any(m.get("content") == "HISTORY_ONE_OK" for m in messages)
            reply = {"content": "HISTORY_OK" if has_user and has_answer else "HISTORY_BAD"}
        elif prompt == "history-one":
            reply = {"content": "HISTORY_ONE_OK"}
        elif prompt == "unicode-你好-🌍":
            reply = {"content": "UNICODE_OK_你好_🌍"}
        elif prompt == "request-shape":
            tool_names = [
                (t.get("function") or {}).get("name", "")
                for t in request.get("tools", [])
            ]
            valid = (
                request.get("model") == self.server.expected_model
                and request.get("max_tokens") == 777
                and request.get("temperature") == 0.15
                and request.get("tool_choice") == "auto"
                and len(request.get("tools", [])) == 9
                and "shell" not in tool_names
                and set(tool_names) == {
                    "read", "csv_read", "write", "edit", "grep", "glob", "ls", "pwd",
                    "exec_program",
                }
                and messages[0].get("role") == "system"
            )
            reply = {"content": "REQUEST_SHAPE_OK" if valid else "REQUEST_SHAPE_BAD"}
        else:
            reply = {"content": "ECHO:" + prompt}
        self.respond(request, reply)

    @staticmethod
    def call(call_id, name, arguments, index=0):
        return {
            "index": index,
            "id": call_id,
            "type": "function",
            "function": {"name": name, "arguments": json.dumps(arguments)},
        }

    def respond(self, request, reply):
        if request.get("stream"):
            event = {"choices": [{"delta": reply}], "usage": {
                "prompt_tokens": 3, "completion_tokens": 2, "total_tokens": 5
            }}
            body = ("data: " + json.dumps(event, ensure_ascii=False) + "\n\ndata: [DONE]\n\n").encode()
            content_type = "text/event-stream"
        else:
            body = json.dumps({"choices": [{"message": reply}], "usage": {
                "prompt_tokens": 3, "completion_tokens": 2, "total_tokens": 5
            }}, ensure_ascii=False).encode()
            content_type = "application/json"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


def recv_line(client):
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = client.recv(4096)
        if not chunk:
            raise RuntimeError("agent closed the connection")
        data.extend(chunk)
    return data.decode(errors="replace").rstrip("\r\n")


def ask(client, content, user="matrix-user", **route_fields):
    payload = json.dumps({"userId": user, "content": content, **route_fields}, ensure_ascii=False)
    client.sendall(payload.encode() + b"\n")
    return recv_line(client)


def wait_for_server(process, port):
    deadline = time.time() + 7
    while time.time() < deadline:
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read())
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("agent did not start")


def write_config(path, provider_port, stream, default="mock"):
    path.write_text(json.dumps({"llm": {
        "default": default,
        "mock": {
            "baseUrl": f"http://127.0.0.1:{provider_port}/v1/chat/completions",
            "apiKeyEnv": "MATRIX_API_KEY",
            "modelName": "matrix-stream" if stream else "matrix-json",
        },
        "alternate": {
            "baseUrl": f"http://127.0.0.1:{provider_port}/v1/chat/completions",
            "apiKey": "alternate-key",
            "modelName": "matrix-alternate",
        },
        "max_tokens": 777,
        "temperature": 0.15,
        "stream": stream,
        "timeout_ms": 3000,
    }}), encoding="utf-8")


def run_matrix(binary, root, stream):
    provider_port, gateway_port = free_port(), free_port()
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", provider_port), Provider)
    provider.requests, provider.authorizations = [], []
    provider.expected_model = "matrix-stream" if stream else "matrix-json"
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix="litecrab-matrix-") as temporary:
        config = Path(temporary) / "llm.json"
        write_config(config, provider_port, stream)
        env = os.environ.copy()
        env["MATRIX_API_KEY"] = "matrix-secret"
        for name in ("LITECRAB_BASE_URL", "LITECRAB_API_KEY", "LITECRAB_MODEL"):
            env.pop(name, None)
        process = subprocess.Popen([
            binary, "--llm-config", str(config), "--workspace", root, "--port", str(gateway_port)
        ], cwd=root, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            with wait_for_server(process, gateway_port) as client:
                client.settimeout(10)
                check(ask(client, "request-shape") == "REQUEST_SHAPE_OK", "request shape/config")
                for fallback in ("route-invalid", "route-unknown", "route-low-confidence",
                                 "route-confidence-high", "route-confidence-negative",
                                 "route-empty-skill", "route-missing-selection",
                                 "route-empty-evidence", "route-bad-evidence"):
                    check(ask(client, fallback) == "BASE_FALLBACK_OK",
                          f"resolver safely falls back to base: {fallback}")
                check(ask(client, "route-markdown") == "MARKDOWN_ROUTE_OK", "resolver extracts fenced JSON")
                check(ask(client, "route-plc") == "PLC_ROUTE_OK", "resolver routes second eligible skill")
                check(ask(client, "route-none") == "BASE_FALLBACK_OK",
                      "resolver nullable selection continues with base agent")
                for query in BASE_ROUTE_QUERIES:
                    check(ask(client, query) == "BASE_ROUTE_OK",
                          f"base intent rule bypasses skill routing: {query}")
                for negative in ("解释一下 PLC 是什么", "不要执行 PLC 诊断，只解释通信原理",
                                 "workspace_summary 是什么"):
                    check(ask(client, negative) == "BASE_FALLBACK_OK",
                          f"hard negative does not activate a skill: {negative}")
                for positive in ("总结当前项目和 README", "PLC 通信失败，帮我诊断"):
                    check(ask(client, positive) == "POSITIVE_ROUTE_OK",
                          f"positive request selects the intended skill: {positive}")
                for query in EVIDENCE_NORMALIZATION_CASES:
                    check(ask(client, query) == "NORMALIZED_EVIDENCE_OK",
                          f"resolver evidence tolerates normalized whitespace/case: {query!r}")
                check(ask(client, "route-none-bypass") == "BYPASS_BLOCKED",
                      "skill_read cannot bypass router selection")
                check(ask(client, "unicode-你好-🌍") == "UNICODE_OK_你好_🌍", "Unicode message")
                check(ask(client, "history-one") == "HISTORY_ONE_OK", "history first turn")
                check(ask(client, "history-two") == "HISTORY_OK", "history second turn")
                check(ask(client, "tool-read") == "TOOL_READ_OK", "read tool")
                multi_answer = ask(client, "multi-tool")
                if multi_answer != "MULTI_TOOL_OK":
                    last_request = provider.requests[-1]
                    observed = [
                        m.get("content", "")[:500]
                        for m in last_request.get("messages", [])
                        if m.get("role") == "tool"
                    ][-3:]
                    raise AssertionError(f"parallel tool calls: {multi_answer!r}; outputs={observed!r}")
                check(ask(client, "bad-tool-args") == "BAD_TOOL_HANDLED", "bad tool arguments")
                check(ask(client, "disabled-tool") == "SHELL_DISABLED_OK",
                      "disabled shell tool is rejected")
                check(ask(client, "exec-plc-help") == "EXEC_PLC_OK",
                      "exec_program launches the PLC Skill binary without shell command parsing")
                check(ask(client, "skill") == "SKILL_OK", "skill_read workflow")
                check(ask(client, "skill-suspend") == "SKILL_SUSPENDED",
                      "skill without completion is suspended")
                check(ask(client, "new-unrelated-task") == "NEW_TASK_OK",
                      "waiting skill does not swallow unrelated task")
                check(ask(client, "continue") == "RESUME_INTENT_OK",
                      "explicit resume intent resumes unique waiting run")
                check(ask(client, "skill-suspend") == "SKILL_SUSPENDED",
                      "second suspended run for exact resume")
                check(ask(client, "resume-exact", replyToRunId="workspace_summary#003") ==
                      "EXACT_RESUME_OK", "exact run id deterministic resume")
                check("不存在或已结束" in ask(client, "bad-route", replyToRunId="missing-run"),
                      "unknown run id fails closed")
                client.sendall(b'{"userId":"u"}\n')
                check("content" in recv_line(client), "missing content validation")
                client.sendall(b'   \n')
                check("empty" in recv_line(client), "empty plain text validation")
                check(ask(client, "after-errors") == "ECHO:after-errors", "recovery after errors")
                check(ask(client, "multiline-response") == "LINE_ONE LINE_TWO",
                      "TCP line response normalizes embedded model newlines")
                second = socket.create_connection(("127.0.0.1", gateway_port), timeout=3)
                try:
                    second.settimeout(10)
                    check(ask(client, "isolation-a-seed") == "ISOLATION_A_SEEDED",
                          "session A seed")
                    check(ask(second, "isolation-b-probe") == "ISOLATION_B_OK",
                          "session B excludes session A history")
                    check(ask(client, "isolation-a-check") == "ISOLATION_A_OK",
                          "session A history survives session B turn")
                    client.shutdown(socket.SHUT_RDWR)
                    client.close()
                    time.sleep(0.05)
                    check(ask(second, "isolation-b-check") == "ISOLATION_B_STILL_OK",
                          "closing session A preserves session B")
                finally:
                    second.close()
            check(all(a == "Bearer matrix-secret" for a in provider.authorizations), "API key header")
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
            provider.shutdown()
            provider.server_close()
        check(process.returncode in (-15, 143, 0), "agent termination")


def run_config_failures(binary, root):
    with tempfile.TemporaryDirectory(prefix="litecrab-config-") as temporary:
        config = Path(temporary) / "llm.json"
        write_config(config, 1, True)
        env = os.environ.copy()
        env.pop("MATRIX_API_KEY", None)
        missing_key = subprocess.run(
            [binary, "--llm-config", str(config)], cwd=root, env=env,
            text=True, capture_output=True, timeout=5,
        )
        check(missing_key.returncode == 2, "missing apiKeyEnv must fail")
        check("MATRIX_API_KEY" in missing_key.stderr, "missing apiKeyEnv diagnostic")
        env["MATRIX_API_KEY"] = "secret-not-for-output"
        unknown = subprocess.run(
            [binary, "--llm-config", str(config), "--llm-provider", "missing"],
            cwd=root, env=env, text=True, capture_output=True, timeout=5,
        )
        check(unknown.returncode == 2, "unknown provider must fail")
        check("unknown default LLM provider" in unknown.stderr, "unknown provider diagnostic")
        check("secret-not-for-output" not in unknown.stderr, "secret leaked to stderr")
        help_result = subprocess.run(
            [binary, "--help"], cwd=root, text=True, capture_output=True, timeout=5,
        )
        check(help_result.returncode == 0 and "--llm-provider" in help_result.stderr, "CLI help")


def main():
    binary, root = sys.argv[1:3]
    plc_scripts = Path(root) / "skills" / "PLC_Diagnosis" / "scripts"
    plc_test_binary = plc_scripts / "main.wsl-test"
    subprocess.run(
        ["make", "TARGET=main.wsl-test"], cwd=plc_scripts, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        run_config_failures(binary, root)
        run_matrix(binary, root, stream=True)
        run_matrix(binary, root, stream=False)
    finally:
        plc_test_binary.unlink(missing_ok=True)
    print(f"agent matrix: {CHECKS} checks passed")


if __name__ == "__main__":
    main()
