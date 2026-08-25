#!/usr/bin/env python3
"""Create legal heads-up tables and continuously drive the authoritative state machine."""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import struct
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import poker_pb2 as pb
except ImportError as error:
    raise SystemExit(
        "poker_pb2 is missing; run scripts/run_active_load.sh so protoc generates it"
    ) from error


class ProtocolError(RuntimeError):
    pass


class Connection:
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None
        self.request_id = 1
        self.sequence = 0
        self.latest_snapshot: pb.TableSnapshot | None = None

    async def connect(self) -> None:
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=10)

    async def request(self, message: pb.Envelope, sequenced: bool = True) -> pb.Envelope:
        if self.reader is None or self.writer is None:
            raise ProtocolError("connection is not open")
        message.protocol_version = 1
        message.request_id = self.request_id
        expected = self.request_id
        self.request_id += 1
        if sequenced:
            self.sequence += 1
            message.client_sequence = self.sequence
        payload = message.SerializeToString()
        self.writer.write(struct.pack("!I", len(payload)) + payload)
        await self.writer.drain()
        while True:
            header = await asyncio.wait_for(self.reader.readexactly(4), timeout=10)
            size = struct.unpack("!I", header)[0]
            if size == 0 or size > 1024 * 1024:
                raise ProtocolError(f"invalid frame size {size}")
            raw = await asyncio.wait_for(self.reader.readexactly(size), timeout=10)
            response = pb.Envelope()
            response.ParseFromString(raw)
            if response.message_type == pb.TABLE_SNAPSHOT:
                self.latest_snapshot = response.table_snapshot
            if response.request_id == expected:
                if response.message_type == pb.ERROR_RESPONSE:
                    raise ProtocolError(response.error_response.message)
                return response

    async def close(self) -> None:
        if self.writer is None:
            return
        self.writer.close()
        try:
            await self.writer.wait_closed()
        except OSError:
            pass
        self.writer = None
        self.reader = None


@dataclass
class Bot:
    user_id: int
    token: str
    lobby: Connection
    game: Connection | None = None


@dataclass
class Counters:
    tables_ready: int = 0
    actions: int = 0
    hands: int = 0
    errors: int = 0


async def create_bot(index: int, prefix: str, args: argparse.Namespace) -> Bot:
    connection = Connection(args.host, args.port)
    await connection.connect()
    username = f"bot_{prefix}_{index}"
    password = f"Load-test-password-{prefix}-{index}"

    register = pb.Envelope(message_type=pb.REGISTER_REQUEST)
    register.register_request.username = username
    register.register_request.password = password
    await connection.request(register, sequenced=False)

    login = pb.Envelope(message_type=pb.LOGIN_REQUEST)
    login.login_request.username = username
    login.login_request.password = password
    response = await connection.request(login, sequenced=False)
    return Bot(response.login_response.user_id,
               response.login_response.session_token,
               connection)


def endpoint(value: str) -> tuple[str, int]:
    host, separator, port = value.rpartition(":")
    if not separator or not host:
        raise ProtocolError(f"invalid endpoint {value!r}")
    return host, int(port)


async def connect_game(bot: Bot, address: str) -> Connection:
    host, port = endpoint(address)
    game = Connection(host, port)
    await game.connect()
    authenticate = pb.Envelope(message_type=pb.AUTHENTICATE_SESSION_REQUEST)
    authenticate.authenticate_session_request.session_token = bot.token
    await game.request(authenticate, sequenced=False)
    bot.game = game
    await bot.lobby.close()
    return game


async def setup_table(number: int,
                      prefix: str,
                      args: argparse.Namespace,
                      counters: Counters) -> tuple[Bot, Bot, int]:
    first, second = await asyncio.gather(
        create_bot(number * 2, prefix, args),
        create_bot(number * 2 + 1, prefix, args))

    create = pb.Envelope(message_type=pb.CREATE_TABLE_REQUEST)
    create.create_table_request.name = f"Load {number}"
    create.create_table_request.max_players = 2
    create.create_table_request.small_blind = 1
    create.create_table_request.big_blind = 2
    create.create_table_request.min_buy_in = 100
    create.create_table_request.max_buy_in = 1000
    created = await first.lobby.request(create)
    table_id = created.create_table_response.table.table_id

    join = pb.Envelope(message_type=pb.JOIN_TABLE_REQUEST)
    join.join_table_request.table_id = table_id
    joined = await second.lobby.request(join)

    first_game, second_game = await asyncio.gather(
        connect_game(first, created.create_table_response.node_endpoint),
        connect_game(second, joined.join_table_response.node_endpoint))

    sit_first = pb.Envelope(message_type=pb.SIT_DOWN_REQUEST)
    sit_first.sit_down_request.table_id = table_id
    sit_first.sit_down_request.seat = 0
    sit_first.sit_down_request.buy_in = 1000
    sit_first.sit_down_request.join_ticket = created.create_table_response.join_ticket
    sit_second = pb.Envelope(message_type=pb.SIT_DOWN_REQUEST)
    sit_second.sit_down_request.table_id = table_id
    sit_second.sit_down_request.seat = 1
    sit_second.sit_down_request.buy_in = 1000
    sit_second.sit_down_request.join_ticket = joined.join_table_response.join_ticket
    await first_game.request(sit_first)
    await second_game.request(sit_second)

    ready_first = pb.Envelope(message_type=pb.READY_REQUEST)
    ready_first.ready_request.table_id = table_id
    ready_first.ready_request.ready = True
    ready_second = pb.Envelope(message_type=pb.READY_REQUEST)
    ready_second.ready_request.table_id = table_id
    ready_second.ready_request.ready = True
    await first_game.request(ready_first)
    response = await second_game.request(ready_second)
    second_game.latest_snapshot = response.table_snapshot
    counters.tables_ready += 1
    counters.hands += 1
    return first, second, table_id


async def play_table(number: int,
                     prefix: str,
                     args: argparse.Namespace,
                     counters: Counters,
                     latencies_ms: list[float],
                     setup_limit: asyncio.Semaphore) -> None:
    first: Bot | None = None
    second: Bot | None = None
    try:
        async with setup_limit:
            first, second, table_id = await setup_table(number, prefix, args, counters)
        players = {first.user_id: first.game, second.user_id: second.game}
        current = second.game.latest_snapshot
        deadline = time.monotonic() + args.duration
        while time.monotonic() < deadline:
            if current is None:
                raise ProtocolError("table has no snapshot")
            if current.street == pb.SETTLED:
                for bot in (first, second):
                    ready = pb.Envelope(message_type=pb.READY_REQUEST)
                    ready.ready_request.table_id = table_id
                    ready.ready_request.ready = True
                    response = await bot.game.request(ready)
                    current = response.table_snapshot
                counters.hands += 1
                continue
            actor = players.get(current.acting_user_id)
            if actor is None:
                await asyncio.sleep(0.001)
                continue
            player = next((value for value in current.players
                           if value.user_id == current.acting_user_id), None)
            if player is None:
                raise ProtocolError("acting player is missing from snapshot")
            action = pb.Envelope(message_type=pb.ACTION_REQUEST)
            action.action_request.table_id = table_id
            action.action_request.hand_id = current.hand_id
            action.action_request.action = (pb.CALL
                                             if player.street_commitment < current.current_bet
                                             else pb.CHECK)
            started = time.perf_counter()
            response = await actor.request(action)
            latencies_ms.append((time.perf_counter() - started) * 1000.0)
            counters.actions += 1
            current = response.table_snapshot
    except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, ProtocolError) as error:
        print(f"table {number} failed: {type(error).__name__}: {error}")
        counters.errors += 1
    finally:
        for bot in (first, second):
            if bot is not None:
                await bot.lobby.close()
                if bot.game is not None:
                    await bot.game.close()


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)
    return round(ordered[index], 3)


async def run(args: argparse.Namespace) -> dict[str, object]:
    counters = Counters()
    latencies: list[float] = []
    prefix = str(int(time.time()))[-7:]
    setup_limit = asyncio.Semaphore(args.setup_concurrency)
    started = time.monotonic()
    await asyncio.gather(*(play_table(index, prefix, args, counters, latencies, setup_limit)
                           for index in range(args.tables)))
    elapsed = time.monotonic() - started
    return {
        "target": f"{args.host}:{args.port}",
        "requested_tables": args.tables,
        "tables_ready": counters.tables_ready,
        "hands_started": counters.hands,
        "actions": counters.actions,
        "errors": counters.errors,
        "elapsed_seconds": round(elapsed, 3),
        "actions_per_second": round(counters.actions / elapsed, 3) if elapsed else 0,
        "latency_ms": {
            "p50": percentile(latencies, 0.50),
            "p95": percentile(latencies, 0.95),
            "p99": percentile(latencies, 0.99),
            "max": round(max(latencies), 3) if latencies else None,
        },
    }


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6000)
    parser.add_argument("--tables", type=int, default=10)
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--setup-concurrency", type=int, default=10)
    parser.add_argument("--output", type=Path)
    result = parser.parse_args()
    if result.tables <= 0 or result.duration <= 0 or result.setup_concurrency <= 0:
        parser.error("tables, duration, and setup-concurrency must be positive")
    return result


def main() -> int:
    args = arguments()
    report = asyncio.run(run(args))
    rendered = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0 if report["errors"] == 0 and report["tables_ready"] == args.tables else 1


if __name__ == "__main__":
    raise SystemExit(main())
