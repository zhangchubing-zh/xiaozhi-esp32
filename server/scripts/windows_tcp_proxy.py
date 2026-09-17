"""User-mode TCP bridge from the Windows LAN address to LiteCrab in WSL."""

import argparse
import asyncio


async def relay(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except ConnectionError:
            pass


async def handle_client(
    client_reader: asyncio.StreamReader,
    client_writer: asyncio.StreamWriter,
    target_host: str,
    target_port: int,
) -> None:
    peer = client_writer.get_extra_info("peername")
    try:
        upstream_reader, upstream_writer = await asyncio.open_connection(target_host, target_port)
    except OSError as exc:
        print(f"proxy connect failed for {peer}: {exc}", flush=True)
        client_writer.close()
        await client_writer.wait_closed()
        return

    print(f"proxy connected {peer} -> {target_host}:{target_port}", flush=True)
    await asyncio.gather(
        relay(client_reader, upstream_writer),
        relay(upstream_reader, client_writer),
    )


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-host", required=True)
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--target-host", required=True)
    parser.add_argument("--target-port", type=int, required=True)
    args = parser.parse_args()

    server = await asyncio.start_server(
        lambda reader, writer: handle_client(
            reader, writer, args.target_host, args.target_port
        ),
        args.listen_host,
        args.listen_port,
    )
    addresses = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    print(f"proxy listening on {addresses}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
