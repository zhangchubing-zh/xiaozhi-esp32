#!/usr/bin/env python3
"""Launch LiteCrab from its native base and LLM configuration files."""
from __future__ import annotations

import argparse
import os
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="config/base_config.example.json")
    parser.add_argument("--llm-config", default="config/llm_config.json")
    parser.add_argument("--llm-provider")
    parser.add_argument("--binary", default="./build/litecrab_server")
    parser.add_argument("--workspace", default=".")
    parser.add_argument("--port", type=int)
    args = parser.parse_args()

    binary = str(Path(args.binary).resolve())
    command = [
        binary,
        "--config",
        str(Path(args.config).resolve()),
        "--llm-config",
        str(Path(args.llm_config).resolve()),
        "--workspace",
        str(Path(args.workspace).resolve()),
    ]
    if args.llm_provider:
        command.extend(("--llm-provider", args.llm_provider))
    if args.port is not None:
        command.extend(("--port", str(args.port)))
    os.execv(binary, command)


if __name__ == "__main__":
    main()
