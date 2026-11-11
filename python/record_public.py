#!/usr/bin/env python3
"""Record a few seconds of public Hyperliquid and Lighter market data.

The client is a minimal WebSocket implementation on the stdlib. It is the
capture path. It is not the latency-sensitive C++ reactor, and the timestamps
it stores are userspace CLOCK_REALTIME equivalents (time.time_ns) taken when
the frame is read. They are not NIC timestamps.

Hyperliquid: wss://api.hyperliquid.xyz/ws
  subscribe l2Book, trades, bbo, activeAssetCtx. Ping {"method":"ping"}.
  l2Book messages are snapshots. There is no sequence number to gap-detect.
Lighter: wss://mainnet.zklighter.elliot.ai/stream
  subscribe order_book/{id} and trade/{id}. Continuity is begin_nonce == previous nonce.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import time
from pathlib import Path
from urllib.parse import urlparse


def connect(url: str, timeout: float = 10):
    u = urlparse(url)
    host = u.hostname
    port = u.port or 443
    path = u.path or "/"
    if u.query:
        path += "?" + u.query
    raw = socket.create_connection((host, port), timeout=timeout)
    ctx = ssl.create_default_context()
    sock = ctx.wrap_socket(raw, server_hostname=host)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: spark-record/0.1\r\n\r\n"
    )
    sock.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("websocket handshake closed")
        buf += chunk
    header, rest = buf.split(b"\r\n\r\n", 1)
    if b" 101 " not in header.split(b"\r\n", 1)[0]:
        raise RuntimeError("handshake failed: " + header.decode("latin1", "replace")[:300])
    sock.settimeout(1.0)
    return sock, rest


def send_text(sock, text: str) -> None:
    payload = text.encode()
    mask = os.urandom(4)
    n = len(payload)
    header = bytearray([0x81])
    if n < 126:
        header.append(0x80 | n)
    elif n < 65536:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", n))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", n))
    header.extend(mask)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(header) + masked)


def frames(sock, initial: bytes):
    buf = bytearray(initial)
    while True:
        while len(buf) < 2:
            chunk = sock.recv(65536)
            if not chunk:
                return
            buf.extend(chunk)
        b0, b1 = buf[0], buf[1]
        opcode = b0 & 0x0F
        masked = (b1 & 0x80) != 0
        length = b1 & 0x7F
        i = 2
        if length == 126:
            while len(buf) < 4:
                buf.extend(sock.recv(65536))
            length = struct.unpack("!H", buf[2:4])[0]
            i = 4
        elif length == 127:
            while len(buf) < 10:
                buf.extend(sock.recv(65536))
            length = struct.unpack("!Q", buf[2:10])[0]
            i = 10
        if masked:
            while len(buf) < i + 4 + length:
                buf.extend(sock.recv(65536))
            mask = buf[i : i + 4]
            i += 4
            payload = bytes(buf[i + k] ^ mask[k % 4] for k in range(length))
            i += length
        else:
            while len(buf) < i + length:
                chunk = sock.recv(65536)
                if not chunk:
                    return
                buf.extend(chunk)
            payload = bytes(buf[i : i + length])
            i += length
        del buf[:i]
        if opcode == 0x8:
            return
        if opcode == 0x9:
            # pong
            pong = bytearray([0x8A, 0x80 | len(payload)])
            mask = os.urandom(4)
            pong.extend(mask)
            pong.extend(bytes(b ^ mask[j % 4] for j, b in enumerate(payload)))
            try:
                sock.sendall(pong)
            except OSError:
                return
            continue
        if opcode == 0x1:
            recv_ns = time.time_ns()
            yield recv_ns, payload.decode("utf-8", "replace")


def record(url: str, subs: list[dict], seconds: float, out_path: Path, venue: str) -> dict:
    sock, rest = connect(url)
    for sub in subs:
        send_text(sock, json.dumps(sub))
    end = time.time() + seconds
    last_ping = time.time()
    counts: dict[str, int] = {}
    gaps = 0
    last_nonce = None
    out_path.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with out_path.open("w") as fh:
        try:
            for recv_ns, text in frames(sock, rest):
                now = time.time()
                if now > end:
                    break
                if venue == "hyperliquid" and now - last_ping > 20:
                    send_text(sock, json.dumps({"method": "ping"}))
                    last_ping = now
                try:
                    msg = json.loads(text)
                except json.JSONDecodeError:
                    counts["bad_json"] = counts.get("bad_json", 0) + 1
                    continue
                channel = msg.get("channel") or msg.get("type") or "unknown"
                counts[str(channel)] = counts.get(str(channel), 0) + 1
                exch = None
                if isinstance(msg.get("data"), dict) and "time" in msg["data"]:
                    exch = int(msg["data"]["time"]) * 1_000_000
                ob = msg.get("order_book") if isinstance(msg.get("order_book"), dict) else None
                if ob and "nonce" in ob:
                    nonce = int(ob["nonce"])
                    begin = ob.get("begin_nonce")
                    if begin is not None and last_nonce is not None and int(begin) != last_nonce:
                        if not str(msg.get("type", "")).startswith("subscribed"):
                            gaps += 1
                    last_nonce = nonce
                    lu = ob.get("last_updated_at")
                    if lu is not None and len(str(lu)) >= 16:
                        exch = int(lu) * 1000
                row = {
                    "venue": venue,
                    "recv_ns": recv_ns,
                    "process_ns": time.time_ns(),
                    "exchange_ns": exch,
                    "channel": channel,
                    "payload": msg,
                }
                fh.write(json.dumps(row, separators=(",", ":")) + "\n")
                n += 1
        finally:
            sock.close()
    return {"path": str(out_path), "messages": n, "counts": counts, "nonce_gaps": gaps}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--out", default="data/samples/live")
    ap.add_argument("--coin", default="BTC")
    ap.add_argument("--market", default="1")
    args = ap.parse_args()
    out = Path(args.out)
    hl = record(
        "wss://api.hyperliquid.xyz/ws",
        [
            {"method": "subscribe", "subscription": {"type": "l2Book", "coin": args.coin}},
            {"method": "subscribe", "subscription": {"type": "trades", "coin": args.coin}},
            {"method": "subscribe", "subscription": {"type": "bbo", "coin": args.coin}},
            {"method": "subscribe", "subscription": {"type": "activeAssetCtx", "coin": args.coin}},
        ],
        args.seconds,
        out / "hyperliquid.jsonl",
        "hyperliquid",
    )
    lt = record(
        "wss://mainnet.zklighter.elliot.ai/stream?readonly=true",
        [
            {"type": "subscribe", "channel": f"order_book/{args.market}"},
            {"type": "subscribe", "channel": f"trade/{args.market}"},
        ],
        args.seconds,
        out / "lighter.jsonl",
        "lighter",
    )
    summary = {"hyperliquid": hl, "lighter": lt}
    (out / "capture_summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
