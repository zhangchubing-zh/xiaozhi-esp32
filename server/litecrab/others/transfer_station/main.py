from __future__ import annotations

import argparse
import signal
import threading

from .config import TransferConfig, load_config
from .service import TransferStation


def main() -> int:
    parser = argparse.ArgumentParser(description="LiteCrab PC transfer station")
    parser.add_argument("--config", required=True)
    parser.add_argument("--verbose", action="store_true", help="enable debug logging")
    args = parser.parse_args()
    config = load_config(args.config)
    if args.verbose:
        config = TransferConfig(**{**config.__dict__, "verbose": True})
    station = TransferStation(config)
    station.configure_logging()
    station.start()
    stopped = threading.Event()

    def shutdown(_signum: int, _frame: object) -> None:
        stopped.set()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    while not stopped.wait(1.0):
        if station.healthy():
            continue
        station.logger.error("a transfer service stopped unexpectedly; restarting")
        station.stop()
        station.start()
    station.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
