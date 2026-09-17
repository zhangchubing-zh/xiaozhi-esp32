#!/usr/bin/env python3
"""Mock OpenAI-compatible /v1/audio/transcriptions endpoint for xiaozhi e2e tests.

Parses the multipart upload, verifies the payload is a RIFF/WAVE file (i.e.
the gateway really decoded Opus to PCM), and returns a fixed transcription so
the fake device can assert the exact stt text.
"""
import http.server
import json
import sys
import threading
import time


class Provider(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    last_wav_bytes = 0

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        content_type = self.headers.get("Content-Type", "")
        boundary = None
        for part in content_type.split(";"):
            part = part.strip()
            if part.startswith("boundary="):
                boundary = part[len("boundary="):]
        wav = b""
        model = ""
        if boundary:
            for section in body.split(b"--" + boundary.encode()):
                if b"filename=" not in section:
                    if b'name="model"' in section:
                        head, _, value = section.partition(b"\r\n\r\n")
                        model = value.strip().decode("utf-8", "replace")
                    continue
                head, _, value = section.partition(b"\r\n\r\n")
                if value.endswith(b"\r\n"):
                    value = value[:-2]
                wav = value
        ok = wav[:4] == b"RIFF" and wav[8:12] == b"WAVE"
        Provider.last_wav_bytes = len(wav)
        if ok:
            import os
            default = f"ASR_OK 模型{model} 音频{len(wav)}字节"
            text = os.environ.get("MOCK_ASR_TEXT", default)
        else:
            text = ""
        log_path = os.environ.get("MOCK_ASR_LOG")
        if log_path:
            with open(log_path, "a", encoding="utf-8") as fp:
                import time
                fp.write(f"{time.time():.3f} req_len={length} wav={len(wav)} "
                         f"ok={ok} text={text!r} wav_head={wav[:12].hex()}\n")
        payload = json.dumps({"text": text}, ensure_ascii=False).encode()
        self.send_response(200 if ok else 400)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_):
        pass


def start(port=0):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Provider)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    srv, bound = start(port)
    print(f"mock_asr listening on 127.0.0.1:{bound}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        srv.shutdown()
