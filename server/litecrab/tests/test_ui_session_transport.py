#!/usr/bin/env python3
import importlib.util
import json
import socket
import sys
import threading
import types
import time

sys.dont_write_bytecode = True


class DummyFlask:
    def __init__(self, *_args, **_kwargs):
        pass

    def route(self, *_args, **_kwargs):
        return lambda function: function

    def run(self, *_args, **_kwargs):
        pass


def load_ui(path):
    flask = types.ModuleType("flask")
    flask.Flask = DummyFlask
    flask.request = types.SimpleNamespace(get_json=lambda **_kwargs: {})
    flask.jsonify = lambda value, *args, **kwargs: value
    flask.render_template = lambda *_args, **_kwargs: ""
    sys.modules["flask"] = flask
    spec = importlib.util.spec_from_file_location("litecrab_ui_for_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class OneShotAgent:
    def __init__(self, port, response):
        self.port = port
        self.response = response
        self.payload = None
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()
        assert self.ready.wait(2)

    def join(self):
        self.thread.join(3)
        assert not self.thread.is_alive()

    def _run(self):
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", self.port))
            server.listen(1)
            self.ready.set()
            conn, _ = server.accept()
            with conn:
                data = bytearray()
                while not data.endswith(b"\n"):
                    chunk = conn.recv(4096)
                    if not chunk:
                        raise RuntimeError("UI closed before sending a line")
                    data.extend(chunk)
                self.payload = json.loads(data.decode())
                wire = (self.response + "\n").encode()
                conn.sendall(wire[:7])
                time.sleep(0.1)  # Packet gaps must not truncate the response.
                conn.sendall(wire[7:])


def main():
    ui = load_ui(sys.argv[1])
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    session = ui.BoardSession("127.0.0.1", port, "browser-session-123", "ui-owner")
    other = ui.BoardSession("127.0.0.1", port, "browser-session-456", "ui-owner")
    first = OneShotAgent(port, "before-restart")
    first.start()
    assert session.send_and_recv("第一轮") == "before-restart"
    first.join()

    other_first = OneShotAgent(port, "other-before-restart")
    other_first.start()
    assert other.send_and_recv("另一会话第一轮") == "other-before-restart"
    other_first.join()

    # The listening server has been destroyed and recreated on the same port.
    # BoardSession must not reuse the old TCP socket, but must reuse sessionId.
    second = OneShotAgent(port, "after-restart")
    second.start()
    assert session.send_and_recv("第二轮") == "after-restart"
    second.join()

    other_second = OneShotAgent(port, "other-after-restart")
    other_second.start()
    assert other.send_and_recv("另一会话第二轮") == "other-after-restart"
    other_second.join()

    assert first.payload == {
        "userId": "ui-owner",
        "sessionId": "browser-session-123",
        "content": "第一轮",
        "responseFormat": "json",
    }
    assert second.payload == {
        "userId": "ui-owner",
        "sessionId": "browser-session-123",
        "content": "第二轮",
        "responseFormat": "json",
    }
    assert other_first.payload["sessionId"] == "browser-session-456"
    assert other_second.payload == {
        "userId": "ui-owner",
        "sessionId": "browser-session-456",
        "content": "另一会话第二轮",
        "responseFormat": "json",
    }
    markdown = '**📋 诊断总结报告**\n\n- **已执行操作**：\n\n  1. `login`\n  2. `get_history_alarm`\n'
    agent = OneShotAgent(port, json.dumps({"responseFormat": "json", "content": markdown}))
    agent.start()
    assert session.send_and_recv("报告") == markdown
    agent.join()


if __name__ == "__main__":
    main()
