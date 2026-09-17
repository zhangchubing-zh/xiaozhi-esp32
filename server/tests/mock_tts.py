#!/usr/bin/env python3
"""Mock SiliconFlow /v1/audio/speech endpoint for xiaozhi e2e tests.

Receives the JSON body, generates a short 24 kHz mono 16-bit sine WAV whose
duration scales with the input length, and records the requests for the
runner to assert on.
"""
import http.server
import json
import math
import struct
import sys
import threading
import time

RATE = 24000


def make_wav(seconds=0.3):
    samples = int(seconds * RATE)
    frames = bytearray()
    for i in range(samples):
        frames += struct.pack("<h", int(6000 * math.sin(2 * math.pi * 440 * i / RATE)))
    data = bytes(frames)
    header = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
              struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) + b"data" +
              struct.pack("<I", len(data)))
    return header + data


class Provider(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    requests = []

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length))
        Provider.requests.append(body)
        wav = make_wav(min(1.0, 0.2 + len(body.get("input", "")) / 60.0))
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(wav)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(wav)

    def log_message(self, *_):
        pass


def start(port=0):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Provider)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    srv, bound = start(port)
    print(f"mock_tts listening on 127.0.0.1:{bound}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        srv.shutdown()
