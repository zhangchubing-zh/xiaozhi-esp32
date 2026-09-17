#!/usr/bin/env python3
"""Exercise the real server and libcurl against local ASP/LLM mocks."""
import http.server
import json
import os
import pathlib
import queue
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time

DEBUG = None


def wait(predicate, label, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.03)
    if DEBUG:
        print(DEBUG(), file=sys.stderr)
    raise AssertionError(label)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def alarm(seq, stamp="26-09-08 10:00:00"):
    return {
        "seqno": seq,
        "almid": 100,
        "almname": "通信异常",
        "equipid": seq,
        "equiptypeid": 33028,
        "equipname": f"设备-{seq}",
        "level": 2,
        "reason": 671,
        "position": 1,
        "localtime": stamp,
        "confirmstate": 0,
        "faultDesc": "通信异常：设备离线",
        "description": "不应原样传给模型的完整告警描述",
        "subReasonList": [{
            "subReason": "通信线缆异常",
            "subRepair": "检查通信线缆",
        }],
    }


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, data, status=200, kind="application/json"):
        if not isinstance(data, bytes):
            data = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Set-Cookie", "sid=mock; Path=/")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/action/login":
            self.server.logins += 1
            self.reply({"token": "mocktoken"})
            return
        request = json.loads(data)
        self.server.calls.append(request)
        answer = {"choices": [{"delta": {"content": "mock diagnosis result"}}]}
        if request.get("stream"):
            body = ("data: " + json.dumps(answer) + "\n\ndata: [DONE]\n\n").encode()
            self.reply(body, kind="text/event-stream")
        else:
            self.reply({"choices": [{"message": {"role": "assistant", "content": "mock diagnosis result"}}]})

    def do_GET(self):
        authenticated = self.headers.get("x-csrf-token") == "mocktoken" and \
            "sid=mock" in self.headers.get("Cookie", "")
        if not authenticated:
            self.server.auth_errors += 1
            self.reply({}, 403)
            return
        if self.path.startswith("/get_monitor_info.asp?type=21&"):
            self.server.poll_times.append(time.monotonic())
            self.reply({"errcode": "OK", "almlist": self.server.active})
            return
        if not self.path.startswith("/set_long_poll_start_info.asp?type=0&id=24"):
            self.server.unexpected.append(self.path)
            self.reply({}, 404)
            return
        self.server.subscriptions += 1
        self.server.pending += 1
        try:
            while not self.server.closing:
                if select.select([self.connection], [], [], 0)[0]:
                    if not self.connection.recv(1, socket.MSG_PEEK):
                        return
                try:
                    event = self.server.events.get(timeout=.05)
                    break
                except queue.Empty:
                    pass
            else:
                return
        finally:
            self.server.pending -= 1
        self.reply(event)


def serve():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.events = queue.Queue()
    server.active = []
    server.poll_times = []
    server.calls = []
    server.unexpected = []
    server.logins = server.auth_errors = server.subscriptions = server.pending = 0
    server.closing = False
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    global DEBUG
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    mode = os.environ.get("ALARM_TEST_MODE", "polling")
    asp, llm = serve(), serve()
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix="litecrab-alarm-e2e-") as workspace:
            root = pathlib.Path(workspace)
            config = root / "llm.json"
            config.write_text(json.dumps({"llm": {
                "default": "local",
                "local": {"baseUrl": f"http://127.0.0.1:{llm.server_port}/v1/chat/completions",
                          "apiKey": "test", "modelName": "mock"},
                "stream": True, "max_tokens": 100,
            }}))

            def start(alarm_enabled=True):
                output = (root / "process.log").open("a")
                args = [binary, "--workspace", workspace, "--llm-config", str(config),
                        "--alarm-base-url", f"http://127.0.0.1:{asp.server_port}",
                        "--alarm-user", "test", "--alarm-password", "test",
                        "--port", str(free_port())]
                if alarm_enabled:
                    args += ["--alarm"]
                if alarm_enabled and mode == "subscription":
                    args += ["--alarm-mode", "subscription"]
                child = subprocess.Popen(args, cwd=workspace, stdout=output, stderr=subprocess.STDOUT)
                output.close()
                return child

            def publish(items):
                asp.active = items
                if mode == "subscription":
                    asp.events.put({"AllAlmNum": len(items)})

            DEBUG = lambda: {
                "mode": mode, "polls": len(asp.poll_times), "subscriptions": asp.subscriptions,
                "logins": asp.logins, "llm_calls": len(llm.calls),
                "process_log": (root / "process.log").read_text()[-4000:],
            }

            process = start(False)
            time.sleep(.3)
            assert asp.logins == 0 and not asp.poll_times, \
                "connection settings alone must not enable monitoring"
            process.terminate()
            process.wait(timeout=3)
            assert process.returncode == 0
            process = None

            publish([alarm(1)])
            process = start()
            wait(lambda: len(llm.calls) == 1, "initial active alarm reaches Agent")

            publish([alarm(1), alarm(2)])
            wait(lambda: len(llm.calls) == 2, "only the new occurrence is forwarded")

            publish([dict(alarm(2), confirmstate=1), alarm(1)])
            time.sleep(5.4 if mode == "polling" else .5)
            assert len(llm.calls) == 2, "reorder/mutable fields must not duplicate"

            invalid = alarm(3)
            invalid.pop("localtime")
            publish([invalid])
            time.sleep(5.4 if mode == "polling" else .5)
            assert len(llm.calls) == 2, "an occurrence without time must be rejected"

            publish([])
            time.sleep(5.4 if mode == "polling" else .5)
            publish([alarm(1)])
            time.sleep(5.4 if mode == "polling" else .5)
            assert len(llm.calls) == 2, "same occurrence must remain deduplicated in one run"

            publish([alarm(1, "26-09-08 10:01:00")])
            wait(lambda: len(llm.calls) == 3, "a new occurrence time must forward")

            if mode == "polling":
                gaps = [b-a for a, b in zip(asp.poll_times, asp.poll_times[1:])]
                assert len(gaps) >= 4 and all(gap >= 4.8 for gap in gaps), gaps
                assert asp.subscriptions == 0, "polling is the default mode"
            else:
                wait(lambda: asp.pending == 1, "subscription must rearm")

            joined = json.dumps(llm.calls, ensure_ascii=False)
            assert "告警ID ：100," in joined
            assert "告警名称 ：通信异常," in joined
            assert "告警原因 ：通信异常：设备离线," in joined
            assert "告警序列号 ：1," in joined
            assert "告警时间 ：26-09-08 10:00:00," in joined
            assert "原因说明 ：通信线缆异常," in joined
            assert "建议处理 ：检查通信线缆," in joined
            assert "使用 PLC_Diagnosis 完整告警诊断、修复和回归验证" in joined
            assert "不应原样传给模型的完整告警描述" not in joined
            spool = root / ".crab/alarm/alarm_spool.bin"
            assert spool.exists() and spool.stat().st_size <= 8 * 1024 * 1024, \
                "durable alarm spool must exist and remain bounded"
            assert not asp.auth_errors and not asp.unexpected

            burst = [alarm(100 + index, f"26-09-08 11:{index:02d}:00")
                     for index in range(20)]
            publish(burst)
            wait(lambda: len(llm.calls) == 23,
                 "completed Agent results must not fill the response registry")
            # The mock seeing the LLM request precedes Agent completion and the
            # durable ACK by a small interval; wait for that responsibility
            # transfer before testing clean-restart deduplication.
            time.sleep(.8)

            begin = time.monotonic()
            process.terminate()
            process.wait(timeout=3)
            assert process.returncode == 0 and time.monotonic() - begin < 2
            process = None
            wait(lambda: asp.pending == 0, "subscription closes cleanly")

            process = start()
            if mode == "subscription":
                publish(asp.active)
            time.sleep(5.4 if mode == "polling" else .7)
            assert len(llm.calls) == 23, \
                "ACKed active occurrences must remain deduplicated after restart"
            publish([alarm(3)])
            wait(lambda: len(llm.calls) == 24, "new alarm after restart")
            process.terminate()
            process.wait(timeout=3)
            assert process.returncode == 0
            process = None
            print("PASS:", mode,
                  "active query, durable dedup, structured Agent completion, clean stop")
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait()
        for server in (asp, llm):
            server.closing = True
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
