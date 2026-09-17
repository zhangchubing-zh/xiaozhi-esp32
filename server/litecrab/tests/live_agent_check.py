#!/usr/bin/env python3
"""Run opt-in checks against an already running LiteCrab TCP gateway."""
from __future__ import annotations

import argparse
import json
import socket


def request(host: str, port: int, content: str) -> str:
    with socket.create_connection((host, port), timeout=5) as client:
        client.settimeout(180)
        payload = json.dumps({"userId": "live-check", "content": content}, ensure_ascii=False)
        client.sendall(payload.encode("utf-8") + b"\n")
        data = bytearray(client.recv(4096))
        client.settimeout(0.05)
        while len(data) <= 1024 * 1024:
            try:
                chunk = client.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            data.extend(chunk)
        return data.decode("utf-8", errors="replace").rstrip("\r\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10003)
    parser.add_argument("--mode", choices=("model", "tool", "skill"), required=True)
    args = parser.parse_args()
    prompts = {
        "model": "Reply with exactly MODEL_CONNECTION_OK",
        "tool": "You must call the read tool on CMakeLists.txt. After inspecting it, reply with TOOL_READ_OK and the project name. Do not answer from memory.",
        "skill": "Use the workspace_summary skill to inspect this LiteCrab workspace. Follow every skill step and include its required completion marker.",
    }
    print(request(args.host, args.port, prompts[args.mode]))


if __name__ == "__main__":
    main()
