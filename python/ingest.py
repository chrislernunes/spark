#!/usr/bin/env python3
"""Download public market data from Hyperliquid and Lighter.

No credentials. Hyperliquid bulk L2 history lives in requester-pays S3 and is
not fetched here. This tool records what the public REST APIs actually return,
writes a manifest, and refuses to label a snapshot as an incremental history.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import time
import urllib.error
import urllib.request
from pathlib import Path

HL = "https://api.hyperliquid.xyz/info"
LT = "https://mainnet.zklighter.elliot.ai"


def get(url: str, timeout: int = 30) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "spark-ingest/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def post(url: str, body: dict, timeout: int = 30) -> bytes:
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json", "User-Agent": "spark-ingest/0.1"}
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def write(path: Path, blob: bytes) -> dict:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    return {
        "path": str(path.as_posix()),
        "bytes": len(blob),
        "sha256": sha256(blob),
        "collected_at_unix_ms": int(time.time() * 1000),
    }


def hl_info(body: dict) -> bytes:
    delay = 0.4
    for attempt in range(5):
        try:
            return post(HL, body)
        except urllib.error.HTTPError as exc:
            if exc.code != 429 or attempt == 4:
                raise
            time.sleep(delay)
            delay *= 2
    raise RuntimeError("unreachable")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/samples")
    ap.add_argument("--coin", default="BTC")
    ap.add_argument("--lighter-market", type=int, default=1)
    args = ap.parse_args()
    out = Path(args.out)
    manifest = {
        "schema": "spark.manifest.v1",
        "coin": args.coin,
        "notes": [
            "Hyperliquid l2Book is one live snapshot of published depth, not a history of incremental updates.",
            "S3 archive s3://hyperliquid-archive/market_data is requester-pays and was not downloaded.",
            "Lighter public trades are a recent window, not the AWS Open Data parquet archive.",
        ],
        "files": [],
    }

    book = hl_info({"type": "l2Book", "coin": args.coin})
    manifest["files"].append({"source": "hyperliquid.info.l2Book", "kind": "l2_snapshot", **write(out / "hl_l2_btc.json", book)})

    trades = hl_info({"type": "recentTrades", "coin": args.coin})
    manifest["files"].append({"source": "hyperliquid.info.recentTrades", "kind": "trades_recent", **write(out / "hl_trades_btc.json", trades)})

    now_ms = int(time.time() * 1000)
    start = now_ms - 48 * 3600 * 1000
    funding = hl_info({"type": "fundingHistory", "coin": args.coin, "startTime": start})
    manifest["files"].append(
        {"source": "hyperliquid.info.fundingHistory", "kind": "funding", **write(out / "hl_funding_btc.json", funding)}
    )

    meta = hl_info({"type": "meta"})
    # Keep only the BTC row plus the response shape, so the sample stays small.
    meta_obj = json.loads(meta)
    btc = next(a for a in meta_obj["universe"] if a.get("name") == args.coin)
    meta_small = {
        "source": "hyperliquid.info.meta",
        "universe_count": len(meta_obj["universe"]),
        "btc": btc,
        "note": "Full universe is not stored in the sample. szDecimals is the size increment exponent.",
    }
    blob = json.dumps(meta_small, indent=2).encode()
    manifest["files"].append({"source": "hyperliquid.info.meta", "kind": "instrument", **write(out / "hl_btc_meta.json", blob)})

    ob = get(f"{LT}/api/v1/orderBooks")
    books = json.loads(ob)
    btc_m = next(x for x in books["order_books"] if x.get("symbol") == "BTC" and x.get("market_type") == "perp")
    blob = json.dumps({"source": "lighter.orderBooks", "btc_perp": btc_m}, indent=2).encode()
    manifest["files"].append({"source": "lighter.orderBooks", "kind": "instrument", **write(out / "lighter_btc_meta.json", blob)})

    orders = get(f"{LT}/api/v1/orderBookOrders?market_id={args.lighter_market}&limit=20")
    manifest["files"].append(
        {"source": "lighter.orderBookOrders", "kind": "order_snapshot", **write(out / "lighter_orders_btc.json", orders)}
    )

    # Recent public trades. Paginate a short window. Stop on repeated trade ids.
    all_trades = []
    cursor = None
    seen = set()
    for _ in range(4):
        url = f"{LT}/api/v1/trades?market_id={args.lighter_market}&sort_by=timestamp&limit=100"
        if cursor:
            url += f"&cursor={cursor}"
        page = json.loads(get(url))
        batch = page.get("trades") or []
        new = 0
        for tr in batch:
            tid = tr.get("trade_id")
            if tid in seen:
                continue
            seen.add(tid)
            all_trades.append(tr)
            new += 1
        cursor = page.get("next_cursor")
        if not cursor or new == 0:
            break
        time.sleep(0.3)
    blob = json.dumps({"code": 200, "trades": all_trades}, indent=2).encode()
    manifest["files"].append(
        {
            "source": "lighter.trades",
            "kind": "trades_recent",
            "trade_count": len(all_trades),
            **write(out / "lighter_trades_btc.json", blob),
        }
    )

    end = now_ms
    fund_url = (
        f"{LT}/api/v1/fundings?market_id={args.lighter_market}&resolution=1h"
        f"&start_timestamp={start}&end_timestamp={end}&count_back=48"
    )
    try:
        lt_funding = get(fund_url)
        manifest["files"].append(
            {"source": "lighter.fundings", "kind": "funding", **write(out / "lighter_funding_btc.json", lt_funding)}
        )
    except urllib.error.HTTPError as exc:
        manifest["files"].append({"source": "lighter.fundings", "kind": "funding", "error": f"HTTP {exc.code}"})

    man_bytes = json.dumps(manifest, indent=2).encode()
    (out / "manifest.json").write_bytes(man_bytes)
    print(json.dumps({"wrote": str(out / "manifest.json"), "files": len(manifest["files"])}, indent=2))


if __name__ == "__main__":
    main()
