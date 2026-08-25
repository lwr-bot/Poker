#!/usr/bin/env python3
"""Async TCP/heartbeat load harness with no third-party Python dependencies."""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import struct
import time
from dataclasses import asdict, dataclass
from pathlib import Path


def varint(value: int) -> bytes:
    result = bytearray()
    while value > 0x7F:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value)
    return bytes(result)


def heartbeat(request_id: int) -> bytes:
    now_ms = int(time.time() * 1000)
    body = b"\x08" + varint(now_ms)
    envelope = b"\x08\x01"                       # protocol_version = 1
    envelope += b"\x10" + varint(request_id)     # request_id
    envelope += b"\x20\x07"                      # message_type = HEARTBEAT
    envelope += b"\xd2\x01" + varint(len(body)) + body  # heartbeat, field 26
    return struct.pack("!I", len(envelope)) + envelope


@dataclass
class Counters:
    connected: int = 0
    responses: int = 0
    errors: int = 0


async def bot(index: int,
              args: argparse.Namespace,
              counters: Counters,
              latencies_ms: list[float]) -> None:
    await asyncio.sleep(index / args.ramp_per_second)
    writer: asyncio.StreamWriter | None = None
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(args.host, args.port), args.connect_timeout)
        counters.connected += 1
        deadline = time.monotonic() + args.duration
        request_id = index * 1_000_000 + 1
        while time.monotonic() < deadline:
            started = time.perf_counter()
            writer.write(heartbeat(request_id))
            await writer.drain()
            header = await asyncio.wait_for(reader.readexactly(4), args.response_timeout)
            size = struct.unpack("!I", header)[0]
            if size == 0 or size > 1024 * 1024:
                raise RuntimeError(f"invalid response frame size {size}")
            await asyncio.wait_for(reader.readexactly(size), args.response_timeout)
            latencies_ms.append((time.perf_counter() - started) * 1000.0)
            counters.responses += 1
            request_id += 1
            remaining = args.heartbeat_interval - (time.perf_counter() - started)
            if remaining > 0:
                await asyncio.sleep(remaining)
    except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, RuntimeError):
        counters.errors += 1
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)
    return ordered[index]


async def run(args: argparse.Namespace) -> dict[str, object]:
    counters = Counters()
    latencies: list[float] = []
    started = time.monotonic()
    await asyncio.gather(*(bot(index, args, counters, latencies)
                           for index in range(args.connections)))
    elapsed = time.monotonic() - started
    def rounded_percentile(fraction: float) -> float | None:
        value = percentile(latencies, fraction)
        return round(value, 3) if value is not None else None

    report = {
        "target": f"{args.host}:{args.port}",
        "requested_connections": args.connections,
        **asdict(counters),
        "duration_seconds": round(elapsed, 3),
        "responses_per_second": round(counters.responses / elapsed, 3) if elapsed else 0,
        "latency_ms": {
            "p50": rounded_percentile(0.50),
            "p95": rounded_percentile(0.95),
            "p99": rounded_percentile(0.99),
            "max": round(max(latencies), 3) if latencies else None,
        },
    }
    return report


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6000)
    parser.add_argument("--connections", type=int, default=5000)
    parser.add_argument("--duration", type=float, default=1800)
    parser.add_argument("--ramp-per-second", type=float, default=500)
    parser.add_argument("--heartbeat-interval", type=float, default=5)
    parser.add_argument("--connect-timeout", type=float, default=10)
    parser.add_argument("--response-timeout", type=float, default=5)
    parser.add_argument("--output", type=Path)
    result = parser.parse_args()
    if result.connections <= 0 or result.duration <= 0 or result.ramp_per_second <= 0:
        parser.error("connections, duration, and ramp-per-second must be positive")
    return result


def main() -> int:
    args = arguments()
    report = asyncio.run(run(args))
    rendered = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0 if report["errors"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
