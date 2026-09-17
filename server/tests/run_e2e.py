#!/usr/bin/env python3
"""xiaozhi gateway end-to-end test runner.

Starts mock LLM/ASR endpoints plus the real litecrab_server (built in WSL),
then drives a fake xiaozhi device through complete protocol turns:

1. checkpoint            - OTA POST returns the websocket section, no mqtt
2. auth_reject           - wrong bearer token gets HTTP 401
3. voice_turn            - opus frames -> ASR (mock) -> agent -> tts text
4. text_chat             - non-voice message goes straight to the agent
5. empty_audio           - listen stop with no audio sends nothing back
6. abort_recover         - abort during a slow agent turn, then a new turn works

Usage (inside WSL, from server/):
    python3 tests/run_e2e.py <path-to-litecrab_server> [path-to-litecrab_opusenc]
"""
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_llm
import mock_asr
import mock_tts
from fake_device import FakeDevice, DeviceError, collect_reply, expect_mcp_bootstrap

HOST = "127.0.0.1"
TOKEN = "e2e-token-123"


def free_port():
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def wait_listening(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def make_wav(path, seconds=2.0, rate=16000):
    import math
    samples = int(seconds * rate)
    frames = bytearray()
    for i in range(samples):
        value = int(8000 * math.sin(2 * math.pi * 440 * i / rate))
        frames += struct.pack("<h", value)
    data = bytes(frames)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + \
        struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + \
        struct.pack("<I", len(data))
    with open(path, "wb") as fp:
        fp.write(header + data)


class Results:
    def __init__(self):
        self.items = []

    def run(self, name, fn):
        print(f"[RUN ] {name}", flush=True)
        try:
            fn()
            self.items.append((name, True, ""))
            print(f"[PASS] {name}", flush=True)
        except Exception as exc:  # noqa: BLE001 - test harness reports all failures
            self.items.append((name, False, str(exc)))
            print(f"[FAIL] {name}: {exc}", flush=True)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server_bin = os.path.abspath(sys.argv[1])
    opusenc = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ""
    if not os.path.exists(server_bin):
        print(f"server binary not found: {server_bin}")
        return 2
    if opusenc and not os.path.exists(opusenc):
        opusenc = ""

    llm_srv, llm_port = mock_llm.start(delay_seconds=4.0)
    asr_srv, asr_port = mock_asr.start()
    tts_srv, tts_port = mock_tts.start()
    ws_port = free_port()
    tcp_port = free_port()

    workspace = tempfile.mkdtemp(prefix="xiaozhi-e2e-")
    base_config = {
        "server": {"listen_ip": "127.0.0.1", "listen_port": tcp_port, "worker_threads": 2,
                   "max_connections": 8},
        "workspace_root": workspace,
        "xiaozhi_ws": {
            "enabled": True,
            "listen_ip": "0.0.0.0",
            "listen_port": ws_port,
            "require_auth": True,
            "auth_token": TOKEN,
            "agent_reply_timeout_ms": 30000,
            "asr": {
                "provider": "siliconflow",
                "endpoint": f"http://{HOST}:{asr_port}/v1/audio/transcriptions",
                "model": "FunAudioLLM/SenseVoiceSmall",
                "api_key": "e2e-asr-key",
                "timeout_ms": 10000,
                "max_audio_seconds": 30,
                "fallback_to_stub": True,
                "stub_input_text": "预设指令兜底文本",
            },
            "mcp_enabled": True,
            "tts": {
                "enabled": True,
                "endpoint": f"http://{HOST}:{tts_port}/v1/audio/speech",
                "model": "mock-tts",
                "voice": "mock-voice",
                "api_key": "e2e-tts-key",
                "timeout_ms": 10000,
                "sample_rate": 24000,
                "frame_ms": 60,
            },
        },
    }
    llm_config = {
        "llm": {
            "default": "siliconflow",
            "siliconflow": {
                "baseUrl": f"http://{HOST}:{llm_port}/v1/chat/completions",
                "apiKey": "e2e-llm-key",
                "modelName": "mock-model",
            },
            "max_tokens": 512,
            "temperature": 0.2,
            "stream": False,
            "timeout_ms": 30000,
        }
    }
    base_path = os.path.join(workspace, "base_config.json")
    llm_path = os.path.join(workspace, "llm_config.json")
    with open(base_path, "w", encoding="utf-8") as fp:
        json.dump(base_config, fp, ensure_ascii=False, indent=2)
    with open(llm_path, "w", encoding="utf-8") as fp:
        json.dump(llm_config, fp, ensure_ascii=False, indent=2)

    server = subprocess.Popen(
        [server_bin, "--config", base_path, "--llm-config", llm_path, "--workspace", workspace],
        cwd=workspace, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    results = Results()
    try:
        if server.poll() is not None:
            print(server.stderr.read())
            return 1
        if not wait_listening(ws_port):
            print("xiaozhi gateway did not start:", server.stderr.read())
            return 1
        print(f"server up: ws={ws_port} llm={llm_port} asr={asr_port}", flush=True)

        def test_checkpoint():
            device = FakeDevice(HOST, ws_port, TOKEN)
            status, payload = device.checkpoint()
            assert status == 200, f"checkpoint status {status}"
            ws = payload.get("websocket")
            assert isinstance(ws, dict), payload
            assert f"ws://{HOST}:{ws_port}/" in ws.get("url", ""), ws
            assert ws.get("token") == TOKEN, ws
            assert ws.get("version") == 1, ws
            assert "mqtt" not in payload, payload
            assert "firmware" not in payload, payload

        def test_auth_reject():
            device = FakeDevice(HOST, ws_port, "wrong-token")
            try:
                device.ws_connect()
                raise AssertionError("handshake with wrong token unexpectedly succeeded")
            except DeviceError as exc:
                assert "401" in str(exc), exc
            anonymous = FakeDevice(HOST, ws_port, "")
            try:
                anonymous.ws_connect()
                raise AssertionError("handshake without token unexpectedly succeeded")
            except DeviceError as exc:
                assert "401" in str(exc), exc

        def make_connected():
            device = FakeDevice(HOST, ws_port, TOKEN)
            status, _ = device.checkpoint()
            assert status == 200
            device.ws_connect()
            hello = device.hello()
            assert hello.get("session_id", "").startswith("xiaozhi-"), hello
            return device

        frames = None
        if opusenc:
            wav_path = os.path.join(workspace, "sine.wav")
            frames_path = os.path.join(workspace, "sine.frames")
            make_wav(wav_path)
            subprocess.run([opusenc, wav_path, frames_path], check=True,
                           capture_output=True)
            device_probe = make_connected()
            frames = device_probe.read_frames_file(frames_path)
            assert len(frames) > 10, f"only {len(frames)} opus frames encoded"
            device_probe.close()

        def test_voice_turn():
            if frames is None:
                print("      (opusenc not provided, skipped)")
                return
            device = make_connected()
            device.voice_turn(frames)
            stt, llm, sentences = collect_reply(device)
            assert stt["text"].startswith("ASR_OK"), stt
            assert sentences and any("MOCK_REPLY[" in s for s in sentences), sentences
            replied = "".join(sentences)
            assert stt["text"] in replied or "MOCK_REPLY[" + stt["text"] in replied, replied
            device.send_close()
            device.close()

        def test_text_chat():
            device = make_connected()
            text = "直接文本消息不需要语音识别"
            device.send_json({"type": "chat", "text": text})
            stt, llm, sentences = collect_reply(device)
            assert stt["text"] == text, stt
            replied = "".join(sentences)
            assert text in replied, replied
            device.send_close()
            device.close()

        def test_empty_audio():
            device = make_connected()
            device.voice_turn([])
            device.expect_silence(2.0)
            device.send_close()
            device.close()

        def test_voice_robot_command():
            """Voice -> mock ASR -> LLM calls the device MCP tool -> fake device
            executes -> LLM summarizes -> TTS audio frames reach the device."""
            if frames is None:
                print("      (opusenc not provided, skipped)")
                return
            os.environ["MOCK_ASR_TEXT"] = "请前进三步"
            device = make_connected()
            bootstrapped = expect_mcp_bootstrap(device)
            assert bootstrapped >= 2, f"mcp bootstrap requests: {device.mcp_requests}"
            device.voice_turn(frames)
            stt, llm, sentences = collect_reply(device, timeout=30, expect_tts_audio=True)
            assert stt["text"] == "请前进三步", stt
            tool_calls = [p for p in device.mcp_requests if p.get("method") == "tools/call"]
            assert tool_calls, f"no tools/call reached the device: {device.mcp_requests}"
            call = tool_calls[0]["params"]
            assert call["name"] == "self.otto.action", call
            assert call["arguments"].get("action") == "walk", call
            assert call["arguments"].get("steps") == 3, call
            replied = "".join(sentences)
            assert "ROBOT_DONE" in replied, replied
            assert "ok: executed action walk steps 3" in replied, replied
            assert device.audio_frames, "no tts audio frames were delivered"
            assert all(len(f) > 2 for f in device.audio_frames)
            del os.environ["MOCK_ASR_TEXT"]
            device.send_close()
            device.close()

        def test_abort_recover():
            device = make_connected()
            device.send_json({"type": "chat", "text": "SLOW:这条消息会触发慢响应然后被打断"})
            device.expect_json("stt", timeout=10)
            device.expect_json("llm", timeout=10)
            time.sleep(0.5)
            device.send_json({"type": "abort", "reason": "wake_word_detected"})
            device.expect_silence(2.0)
            followup = "打断之后的第二条消息"
            device.send_json({"type": "chat", "text": followup})
            stt, llm, sentences = collect_reply(device, timeout=25)
            assert stt["text"] == followup, stt
            replied = "".join(sentences)
            assert followup in replied, replied
            device.send_close()
            device.close()

        results.run("checkpoint", test_checkpoint)
        results.run("auth_reject", test_auth_reject)
        results.run("voice_turn", test_voice_turn)
        results.run("text_chat", test_text_chat)
        results.run("empty_audio", test_empty_audio)
        results.run("voice_robot_command", test_voice_robot_command)
        results.run("abort_recover", test_abort_recover)
    finally:
        server.send_signal(signal.SIGTERM)
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
        llm_srv.shutdown()
        asr_srv.shutdown()
        tts_srv.shutdown()

    failed = [name for name, ok, _ in results.items if not ok]
    print(f"\ne2e summary: {len(results.items) - len(failed)}/{len(results.items)} passed")
    if failed:
        print(f"workspace kept for debugging: {workspace}")
        print(f"server log: {workspace}/logs/litecrab.log")
    else:
        shutil.rmtree(workspace, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
