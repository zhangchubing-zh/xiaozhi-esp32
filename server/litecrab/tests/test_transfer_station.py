#!/usr/bin/env python3
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import socketserver
import sys
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "others"))
from transfer_station import TransferConfig, TransferStation


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class BoardHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.wfile.write(b"BOARD:" + self.rfile.readline())


class LlmHandler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.last_payload = payload
        body = json.dumps({"model": payload["model"], "ok": True}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class TransferStationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.board = socketserver.ThreadingTCPServer(("127.0.0.1", free_port()), BoardHandler)
        cls.llm = ThreadingHTTPServer(("127.0.0.1", free_port()), LlmHandler)
        cls.board_thread = threading.Thread(target=cls.board.serve_forever, daemon=True)
        cls.llm_thread = threading.Thread(target=cls.llm.serve_forever, daemon=True)
        cls.board_thread.start(); cls.llm_thread.start()
        config = TransferConfig(tcp_port=free_port(), http_port=free_port(), board_port=cls.board.server_address[1], llm_url=f"http://127.0.0.1:{cls.llm.server_address[1]}/v1/chat/completions", default_model="default-test")
        cls.station = TransferStation(config); cls.station.start(); cls.config = config

    @classmethod
    def tearDownClass(cls):
        cls.station.stop(); cls.board.shutdown(); cls.board.server_close(); cls.llm.shutdown(); cls.llm.server_close()

    def test_health_status_and_cors(self):
        self.assertTrue(self.station.healthy())
        for path in ("health", "status"):
            with urlopen(f"http://127.0.0.1:{self.config.http_port}/{path}") as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(response.headers["Access-Control-Allow-Origin"], "*")

    def test_http_proxy_fills_model(self):
        body = json.dumps({"messages": [{"role": "user", "content": "hi"}]}).encode()
        with urlopen(Request(f"http://127.0.0.1:{self.config.http_port}/v1/chat/completions", data=body, headers={"Content-Type": "application/json"})) as response:
            result = json.load(response)
        self.assertTrue(result["ok"]); self.assertEqual(result["model"], "default-test")

    def test_invalid_http_request(self):
        with self.assertRaises(HTTPError) as error:
            urlopen(Request(f"http://127.0.0.1:{self.config.http_port}/v1/chat/completions", data=b"{}", headers={"Content-Type": "application/json"}))
        self.assertEqual(error.exception.code, 400)

    def test_tcp_proxy(self):
        with socket.create_connection(("127.0.0.1", self.config.tcp_port), 2) as client:
            client.sendall(b"hello\n"); reply = client.makefile("rb").readline()
        self.assertEqual(reply, b"BOARD:hello\n")

    def test_validation(self):
        with self.assertRaises(ValueError):
            TransferConfig(tcp_port=12000, http_port=12000).validate()


if __name__ == "__main__":
    unittest.main()
