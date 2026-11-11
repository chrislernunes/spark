#!/usr/bin/env python3
"""Short-horizon association on the trades we actually downloaded.

This is not a backtest and not a PnL. A print does not tell us the book just
before the trade. The only honest statistic available from a trade-only file
is the autocorrelation of signed trade direction and the subsequent trade
return. Sample size is reported. Nothing here is labelled a strategy result.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path


def pearson(xs, ys):
    n = len(xs)
    if n < 3 or n != len(ys):
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    num = dx = dy = 0.0
    for a, b in zip(xs, ys):
        da, db = a - mx, b - my
        num += da * db
        dx += da * da
        dy += db * db
    if dx <= 0 or dy <= 0:
        return None
    return num / math.sqrt(dx * dy)


def hl_rows(path: Path):
    rows = json.loads(path.read_text())
    out = []
    for r in rows:
        side = 1.0 if r.get("side") == "B" else -1.0 if r.get("side") == "A" else None
        if side is None:
            continue
        out.append((int(r["time"]), float(r["px"]), side, float(r["sz"])))
    out.sort()
    return out


def lt_rows(path: Path):
    rows = json.loads(path.read_text()).get("trades", [])
    out = []
    for r in rows:
        # is_maker_ask true means the maker was the ask, so the taker bought.
        if "is_maker_ask" not in r:
            continue
        side = 1.0 if r["is_maker_ask"] else -1.0
        out.append((int(r["timestamp"]), float(r["price"]), side, float(r["size"])))
    out.sort()
    return out


def association(rows, horizon_ms: int):
    xs, ys = [], []
    j = 0
    for i, (t, px, side, sz) in enumerate(rows):
        while j < len(rows) and rows[j][0] < t + horizon_ms:
            j += 1
        if j >= len(rows) or px <= 0:
            continue
        fut = rows[j][1]
        xs.append(side)
        ys.append(math.log(fut / px))
    rho = pearson(xs, ys)
    return {"n": len(xs), "horizon_ms": horizon_ms, "corr_sign_vs_logret": rho}


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "data/samples")
    report = {"schema": "spark.signal_association.v1", "warning": "trade-only, no book state, not a backtest", "venues": {}}
    specs = [("hyperliquid", root / "hl_trades_btc.json", hl_rows), ("lighter", root / "lighter_trades_btc.json", lt_rows)]
    for name, path, loader in specs:
        if not path.is_file():
            report["venues"][name] = {"status": "missing"}
            continue
        rows = loader(path)
        horizons = []
        for h in (100, 500, 1000, 5000):
            horizons.append(association(rows, h))
        n = horizons[0]["n"] if horizons else 0
        report["venues"][name] = {
            "prints": len(rows),
            "horizons": horizons,
            "inference": "not_supported" if n < 200 else "descriptive_only",
            "reason": "sample below 200 aligned observations" if n < 200 else "correlation is not a tradable edge and fees are not applied",
        }
    text = json.dumps(report, indent=2)
    (root / "signal_association.json").write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
