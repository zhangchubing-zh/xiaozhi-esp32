#!/usr/bin/env python3
import http.server
import json
import os
import subprocess
import sys
import threading
import urllib.parse


DESCRIPTION_MARKER = "RAW_DESCRIPTION_MUST_NOT_REACH_LLM_"
CACHE_MARKER = "RAW_CACHE_MUST_NOT_REACH_LLM_"


def alarm(seq, equip, reasonum):
    reasons = "\n".join(f"{i}. reason-{seq}-{i}" for i in range(1, 5))
    repairs = "\n".join(f"{i}. repair-{seq}-{i}" for i in range(1, 5))
    return {
        "seqno": seq,
        "almid": 1154,
        "almname": "Southbound device communication anomaly",
        "equipid": equip,
        "equiptypeid": 33028,
        "equipname": f"Logger({equip})",
        "level": 2,
        "confirmstate": 0,
        "reason": 671,
        "reasonum": reasonum,
        "position": equip,
        "description": DESCRIPTION_MARKER + ("x" * 5000),
        "type": 1,
        "localtime": f"2026-09-08 10:56:{seq:02d}",
        "lockstate": 0,
        "locationInfo": f"rack-{equip}",
        "faultDesc": "The inverter communication is abnormal.",
        "subReasonList": [{"subReason": reasons, "subRepair": repairs}],
    }


ACTIVE = {"errcode": 0, "almlist": [alarm(24, 1, 1), alarm(25, 2, 2)]}
HISTORY = {
    "errcode": 0,
    "totalnum": 2,
    "hisalmlist": [
        {"seqno": 24, "alarmed": 1154, "alarmname": "Communication anomaly",
         "equipid": 1, "equiptypeid": 33028, "equipname": "Logger(1)", "level": 2,
         "startime": "2026-09-08 10:56:24", "endtime": "", "confirmstate": 0,
         "reason": 671, "reasonum": 1},
        {"seqno": 23, "alarmed": 1002, "alarmname": "Example alarm",
         "equipid": 1, "equiptypeid": 33028, "equipname": "Logger(1)", "level": 1,
         "startime": "2026-09-07 08:10:00", "endtime": "2026-09-07 08:15:00",
         "confirmstate": 1, "reason": 12, "reasonum": 1},
    ],
}
CACHE = {"privateCache": CACHE_MARKER + ("z" * 8000), "devices": [1, 2, 3]}


class Provider(http.server.BaseHTTPRequestHandler):
    band = 1

    def log_message(self, *_):
        pass

    def reply(self, payload):
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    @classmethod
    def monitor(cls):
        return {
            "errCode": 0,
            "sigList": [
                {"sigId": 8201, "sigName": "Network frequency band", "sigValue": cls.band,
                 "sigUnit": "", "sigAuth": 48, "valType": 17,
                 "enumList": [{"enumName": "Band 1", "enumVal": 1},
                              {"enumName": "Band 2", "enumVal": 3},
                              {"enumName": "Band 3", "enumVal": 23}]},
                {"sigId": 8210, "sigName": "Voltage", "sigValue": 48, "sigUnit": "V",
                 "minVal": 0, "maxVal": 60, "precision": 1},
                {"sigId": 8211, "sigName": "Enabled", "sigValue": "1", "sigUnit": "",
                 "enumList": [{"enumName": "Disabled", "enumVal": 0},
                              {"enumName": "Enabled", "enumVal": 1}]},
            ],
        }

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        if parsed.path == "/get_monitor_info.asp" and query.get("type") == ["21"]:
            self.reply(ACTIVE)
        elif parsed.path == "/get_monitor_info.asp" and query.get("type") == ["13"]:
            self.reply(self.monitor())
        elif parsed.path == "/get_history_info.asp":
            self.reply(HISTORY)
        elif parsed.path == "/get_cache_info.asp":
            self.reply(CACHE)
        else:
            self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        if self.path == "/action/login":
            self.reply({"token": "abc123"})
        elif self.path == "/set_signal_info.asp":
            type(self).band = 3
            self.reply({"errCode": 0, "resultList": [{"equipTypeId": 33036,
                "equipId": 4099, "result": 0,
                "sigResultList": [{"sigId": 8201, "setResult": 0}]}]})
        else:
            self.send_error(404)


def run(binary, base_url, operation, *args):
    env = dict(os.environ)
    env["LITECRAB_ALARM_USER"] = "tester"
    env["LITECRAB_ALARM_PASSWORD"] = "secret"
    completed = subprocess.run([binary, "-o", operation, "--base-url", base_url, *args],
                               env=env, text=True, capture_output=True, check=False)
    assert completed.returncode == 0, (operation, completed.returncode, completed.stderr, completed.stdout)
    lines = completed.stdout.splitlines()
    assert len(lines) == 1, (operation, lines)
    result = json.loads(lines[0])
    assert result["operation"] == operation and result["success"] is True
    assert "body" not in result and "url" not in result
    return result, len(completed.stdout.encode())


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_port}"
    sizes = {}
    try:
        login, sizes["login"] = run(binary, base_url, "login")
        assert login["data"]["authenticated"] is True

        cache, sizes["get_cache_info"] = run(binary, base_url, "get_cache_info")
        assert cache["data"]["schema_known"] is False
        assert CACHE_MARKER not in json.dumps(cache)

        active, sizes["get_active_alarm"] = run(binary, base_url, "get_active_alarm")
        assert active["data"]["returned_alarm_count"] == 2
        assert DESCRIPTION_MARKER not in json.dumps(active)
        for item in active["data"]["alarms"]:
            assert len(item["topCauses"]) == 3
            assert item["cause_repair_total_count"] == 4
            assert item["cause_repair_truncated"] is True
            assert item["topCauses"][0]["reason"].startswith("reason-")
            assert item["topCauses"][0]["repair"].startswith("repair-")

        monitor, sizes["get_monitor_info"] = run(
            binary, base_url, "get_monitor_info", "--equip-id", "4099",
            "--equip-type-id", "33036", "--para3", "4", "--para4", "2")
        signals = monitor["data"]["devices"][0]["signals"]
        assert [item["sigId"] for item in signals] == [8201, 8210, 8211]
        assert signals[2]["displayValue"] == "Enabled"
        assert signals[0]["displayValue"] == "Band 1"
        assert signals[2]["displayValue"] == "Enabled"
        assert all("enumList" not in item for item in signals)

        history, sizes["get_history_alarm"] = run(
            binary, base_url, "get_history_alarm", "--equip-id", "1",
            "--page-index", "1", "--page-size", "20")
        assert history["data"]["returned_count"] == 2
        assert history["data"]["alarms"][0]["alarm_id"] == 1154

        frequency, sizes["set_freq_band"] = run(
            binary, base_url, "set_freq_band", "--equip-id", "4099",
            "--equip-type-id", "33036")
        assert frequency["before"]["band_number"] == 1
        assert frequency["planned"]["band_number"] == 2
        assert frequency["after"]["band_number"] == 2
        assert frequency["write"]["accepted"] is True
        assert frequency["verification"]["readback_matches"] is True
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    print(json.dumps({"normalized_stdout_bytes": sizes}, sort_keys=True))


if __name__ == "__main__":
    main()
