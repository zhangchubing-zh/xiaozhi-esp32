#!/usr/bin/env python3
"""Fake xiaozhi-esp32 device for gateway e2e tests. Pure standard library.

Implements the client half of the xiaozhi WebSocket protocol:
- OTA checkpoint HTTP POST, expecting the websocket section in the response
- WebSocket handshake with Authorization/Device-Id/Client-Id headers
- Client frames are masked (RFC 6455), server frames are not
- hello / listen start-stop / binary opus frames / chat / abort / close
"""
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import time

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class DeviceError(Exception):
    pass


# MCP tool list simulating a real otto-robot device (schema mirrors firmware).
OTTO_TOOLS = [
    {
        "name": "self.otto.action",
        "description": "让机器人执行动作：walk/turn/jump/swing/moonwalk/bend/shake_leg/"
                       "updown/whirlwind_leg/sit/showcase/home/hands_up/hands_down/hand_wave",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "default": "sit",
                           "enum": ["walk", "turn", "jump", "swing", "moonwalk", "sit", "home"]},
                "steps": {"type": "integer", "default": 3, "minimum": 1, "maximum": 100},
                "speed": {"type": "integer", "default": 700, "minimum": 100, "maximum": 3000},
                "direction": {"type": "integer", "default": 1, "minimum": -1, "maximum": 1},
                "amount": {"type": "integer", "default": 30, "minimum": 0, "maximum": 170},
                "arm_swing": {"type": "integer", "default": 50, "minimum": 0, "maximum": 170},
            },
            "required": [],
        },
    },
    {
        "name": "self.otto.stop",
        "description": "立即停止所有动作并复位",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "self.battery.get_level",
        "description": "获取机器人电池电量和充电状态",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
]


def _recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise DeviceError("connection closed")
        data += chunk
    return data


class FakeDevice:
    def __init__(self, host, port, token="", device_id="AA:BB:CC:DD:EE:FF", client_id="e2e-client-1"):
        self.host = host
        self.port = port
        self.token = token
        self.device_id = device_id
        self.client_id = client_id
        self.sock = None
        self.session_id = ""
        self.mcp_requests = []
        self.audio_frames = []

    # -- checkpoint -------------------------------------------------------
    def checkpoint(self, timeout=5):
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        body = json.dumps({"application": {"name": "fake-device", "version": "0.1.0"},
                           "mac_address": self.device_id})
        conn.request("POST", "/", body=body,
                     headers={"Content-Type": "application/json", "Device-Id": self.device_id})
        resp = conn.getresponse()
        status = resp.status
        payload = json.loads(resp.read() or b"{}")
        conn.close()
        return status, payload

    # -- websocket --------------------------------------------------------
    def ws_connect(self, timeout=10, path="/xiaozhi/v1/", expect_status=101):
        key = base64.b64encode(os.urandom(16)).decode()
        headers = (f"GET {path} HTTP/1.1\r\n"
                   f"Host: {self.host}:{self.port}\r\n"
                   f"Upgrade: websocket\r\n"
                   f"Connection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\n"
                   f"Sec-WebSocket-Version: 13\r\n"
                   f"Device-Id: {self.device_id}\r\n"
                   f"Client-Id: {self.client_id}\r\n")
        if self.token:
            headers += f"Authorization: Bearer {self.token}\r\n"
        headers += "\r\n"
        self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
        self.sock.sendall(headers.encode())
        status_line = b""
        while b"\r\n\r\n" not in status_line:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise DeviceError("closed during handshake")
            status_line += chunk
        head, _, rest = status_line.partition(b"\r\n\r\n")
        self._buffer = rest
        try:
            status = int(head.split(b" ", 2)[1])
        except (IndexError, ValueError):
            raise DeviceError("bad status line: %r" % head.split(b"\r\n")[0])
        if status != expect_status:
            self.close()
            raise DeviceError(f"handshake status {status}, expected {expect_status}")
        if status == 101:
            accept = ""
            for line in head.split(b"\r\n"):
                if line.lower().startswith(b"sec-websocket-accept:"):
                    accept = line.split(b":", 1)[1].strip().decode()
            want = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            if accept != want:
                self.close()
                raise DeviceError(f"bad accept {accept!r}, want {want!r}")
        return status

    def _send_frame(self, opcode, payload):
        if self.sock is None:
            raise DeviceError("not connected")
        mask = os.urandom(4)
        header = bytearray([0x80 | opcode])
        n = len(payload)
        if n <= 125:
            header.append(0x80 | n)
        elif n <= 0xFFFF:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def send_json(self, obj):
        self._send_frame(0x1, json.dumps(obj, ensure_ascii=False).encode())

    def send_binary(self, data):
        self._send_frame(0x2, data)

    def send_ping(self, data=b"keepalive"):
        self._send_frame(0x9, data)

    def send_close(self):
        self._send_frame(0x8, b"")

    def recv_frame(self, timeout=5):
        """Returns (opcode, payload) of the next non-pong frame."""
        self.sock.settimeout(timeout)
        while True:
            head = _recv_exact(self.sock, 2)
            fin = bool(head[0] & 0x80)
            opcode = head[0] & 0x0F
            n = head[1] & 0x7F
            if n == 126:
                n = struct.unpack(">H", _recv_exact(self.sock, 2))[0]
            elif n == 127:
                n = struct.unpack(">Q", _recv_exact(self.sock, 8))[0]
            payload = _recv_exact(self.sock, n) if n else b""
            if opcode == 0xA:
                continue
            return opcode, payload

    # -- device MCP responder --------------------------------------------
    def handle_mcp_request(self, message):
        """Answers a gateway MCP request; returns True if the message was MCP."""
        payload = message.get("payload") or {}
        if message.get("type") != "mcp" or "method" not in payload:
            return False
        method = payload.get("method")
        req_id = payload.get("id")
        if method == "initialize":
            result = {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
                      "serverInfo": {"name": "fake-otto", "version": "0.1"}}
        elif method == "tools/list":
            result = {"tools": OTTO_TOOLS}
        elif method == "tools/call":
            name = (payload.get("params") or {}).get("name")
            arguments = (payload.get("params") or {}).get("arguments") or {}
            if name == "self.otto.action":
                action = arguments.get("action", "sit")
                steps = arguments.get("steps", 3)
                text = f"ok: executed action {action} steps {steps}"
            elif name == "self.otto.stop":
                text = "ok: stopped"
            elif name == "self.battery.get_level":
                text = '{"level":87,"charging":false}'
            else:
                self.send_json({"session_id": self.session_id, "type": "mcp", "payload": {
                    "jsonrpc": "2.0", "id": req_id,
                    "error": {"message": f"Unknown tool: {name}"}}})
                return True
            result = {"content": [{"type": "text", "text": text}], "isError": False}
        else:
            result = None
        reply = {"session_id": self.session_id, "type": "mcp", "payload": {}}
        if result is not None:
            reply["payload"] = {"jsonrpc": "2.0", "id": req_id, "result": result}
        else:
            reply["payload"] = {"jsonrpc": "2.0", "id": req_id,
                                "error": {"message": f"Method not implemented: {method}"}}
        self.send_json(reply)
        self.mcp_requests.append(payload)
        return True

    def recv_json(self, timeout=5):
        opcode, payload = self.recv_frame(timeout)
        if opcode != 0x1:
            raise DeviceError(f"expected text frame, got opcode {opcode}")
        return json.loads(payload.decode())

    def expect_json(self, wanted_type, wanted_state=None, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                msg = self.recv_json(timeout=max(0.1, deadline - time.time()))
            except socket.timeout:
                break
            if msg.get("type") == "mcp":
                self.handle_mcp_request(msg)
                continue
            if msg.get("type") != wanted_type:
                continue
            if wanted_state is not None and msg.get("state") != wanted_state:
                continue
            if msg.get("session_id"):
                self.session_id = msg["session_id"]
            return msg
        raise DeviceError(f"timeout waiting for type={wanted_type} state={wanted_state}")

    def expect_silence(self, duration):
        deadline = time.time() + duration
        while time.time() < deadline:
            try:
                opcode, payload = self.recv_frame(timeout=max(0.1, deadline - time.time()))
                if opcode == 0x1:
                    msg = json.loads(payload.decode())
                    if msg.get("type") == "mcp":
                        self.handle_mcp_request(msg)
                        continue
                    raise DeviceError(f"unexpected frame during silence: {payload[:200]!r}")
                raise DeviceError(f"unexpected frame opcode={opcode} during silence window")
            except socket.timeout:
                return True
        return True

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    # -- high-level turns -------------------------------------------------
    def hello(self):
        self.send_json({"type": "hello", "version": 1, "transport": "websocket",
                        "features": {"mcp": True},
                        "audio_params": {"format": "opus", "sample_rate": 16000,
                                         "channels": 1, "frame_duration": 60}})
        reply = self.expect_json("hello", timeout=10)
        if reply.get("transport") != "websocket":
            raise DeviceError("server hello transport mismatch")
        return reply

    def voice_turn(self, frames, chunk_delay=0.0):
        self.send_json({"session_id": self.session_id, "type": "listen",
                        "state": "start", "mode": "manual"})
        for frame in frames:
            self.send_binary(frame)
            if chunk_delay:
                time.sleep(chunk_delay)
        self.send_json({"session_id": self.session_id, "type": "listen", "state": "stop"})

    def read_frames_file(self, path):
        frames = []
        with open(path, "rb") as fp:
            data = fp.read()
        off = 0
        while off + 2 <= len(data):
            n = (data[off] << 8) | data[off + 1]
            off += 2
            frames.append(data[off:off + n])
            off += n
        return frames


def collect_reply(device, timeout=20, expect_tts_audio=False):
    """Collects one full turn, answering device MCP requests along the way.

    Sequence: stt -> llm -> tts start -> sentences (+ optional binary opus
    frames after each sentence when the gateway has TTS enabled) -> tts stop.
    Audio frames are appended to device.audio_frames."""
    stt = device.expect_json("stt", timeout=timeout)
    llm = device.expect_json("llm", timeout=timeout)
    assert device.expect_json("tts", "start", timeout=timeout)
    sentences = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        opcode, payload = device.recv_frame(timeout=max(0.1, deadline - time.time()))
        if opcode == 0x2:
            device.audio_frames.append(payload)
            continue
        msg = json.loads(payload.decode())
        if msg.get("type") == "mcp":
            device.handle_mcp_request(msg)
            continue
        if msg.get("type") != "tts":
            continue
        state = msg.get("state")
        if state == "sentence_start":
            sentences.append(msg.get("text", ""))
        elif state == "stop":
            if expect_tts_audio and not device.audio_frames:
                raise DeviceError("expected tts audio frames but none were sent")
            return stt, llm, sentences
    raise DeviceError("tts stop not received")


def expect_mcp_bootstrap(device, timeout=10):
    """Handles the gateway MCP handshake: initialize + tools/list. Returns the
    number of answered requests once the tool list has been delivered."""
    seen_list = False
    deadline = time.time() + timeout
    while time.time() < deadline and not seen_list:
        opcode, payload = device.recv_frame(timeout=max(0.1, deadline - time.time()))
        if opcode != 0x1:
            continue
        msg = json.loads(payload.decode())
        if msg.get("type") != "mcp":
            continue
        method = (msg.get("payload") or {}).get("method")
        device.handle_mcp_request(msg)
        if method == "tools/list":
            seen_list = True
    if not seen_list:
        raise DeviceError("gateway did not request the device tool list")
    return len(device.mcp_requests)
