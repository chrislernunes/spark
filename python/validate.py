#!/usr/bin/env python3
"""Validate a Spark sample directory. Bad rows are reported, not deleted."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def load(path: Path):
    return json.loads(path.read_text())


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "data/samples")
    manifest = load(root / "manifest.json")
    report = {
        "schema": "spark.data_quality.v1",
        "root": str(root),
        "files": [],
        "issues": [],
    }
    ok = True
    for entry in manifest["files"]:
        if "path" not in entry:
            report["issues"].append({"file": entry.get("source"), "issue": "not_downloaded", "detail": entry.get("error")})
            continue
        path = Path(entry["path"])
        if not path.is_file():
            # manifest paths are relative to the spark root when ingest is run from spark/
            alt = root.parent.parent / entry["path"] if not path.is_file() else path
            path = path if path.is_file() else alt
        if not path.is_file():
            report["issues"].append({"file": entry["path"], "issue": "missing"})
            ok = False
            continue
        blob = path.read_bytes()
        digest = sha256(blob)
        item = {"path": entry["path"], "sha256_ok": digest == entry["sha256"], "bytes": len(blob)}
        if digest != entry["sha256"]:
            report["issues"].append({"file": entry["path"], "issue": "checksum_mismatch"})
            ok = False
        report["files"].append(item)

    def check_prices(rows, price_key, size_key, ts_key, label):
        dups = 0
        bad = 0
        ids = set()
        for row in rows:
            try:
                px = float(row[price_key])
                sz = float(row[size_key])
                ts = int(row[ts_key])
            except (KeyError, TypeError, ValueError):
                bad += 1
                continue
            if px <= 0 or sz < 0 or ts <= 0:
                bad += 1
            tid = row.get("tid", row.get("trade_id"))
            if tid in ids:
                dups += 1
            ids.add(tid)
        ordered = []
        for row in rows:
            try:
                ordered.append(int(row[ts_key]))
            except (KeyError, TypeError, ValueError):
                continue
        ordered.sort()
        span = ordered[-1] - ordered[0] if len(ordered) >= 2 else None
        # REST pages are newest-first. That arrival order is not a clock error.
        # Report the span after sorting and do not emit one issue per print.
        report.setdefault("coverage", {}).setdefault(label, {})
        report["coverage"][label].update({"span_ms": span, "duplicate_ids": dups, "rows": len(rows)})
        if bad:
            report["issues"].append({"file": label, "issue": "invalid_price_or_size", "count": bad})
            nonlocal_ok[0] = False
        if dups:
            report["issues"].append({"file": label, "issue": "duplicate_trade_id", "count": dups})

    nonlocal_ok = [ok]
    hl_trades_path = root / "hl_trades_btc.json"
    if hl_trades_path.is_file():
        rows = load(hl_trades_path)
        check_prices(rows, "px", "sz", "time", "hl_trades_btc.json")
        # recentTrades is a short live window, not a continuous history.
        report["coverage"] = report.get("coverage", {})
        report["coverage"]["hyperliquid_trades"] = {
            "count": len(rows),
            "continuous_history": False,
            "reason": "info recentTrades returns a short tail, not the S3 fill archive",
        }
    lt_path = root / "lighter_trades_btc.json"
    if lt_path.is_file():
        rows = load(lt_path).get("trades", [])
        check_prices(rows, "price", "size", "timestamp", "lighter_trades_btc.json")
        report.setdefault("coverage", {})["lighter_trades"] = {
            "count": len(rows),
            "continuous_history": False,
            "reason": "paginated recent REST trades, not the AWS Open Data parquet set",
        }
    hl_book = root / "hl_l2_btc.json"
    if hl_book.is_file():
        book = load(hl_book)
        bids, asks = book["levels"]
        if not bids or not asks:
            report["issues"].append({"file": "hl_l2_btc.json", "issue": "empty_side"})
            nonlocal_ok[0] = False
        if float(bids[0]["px"]) >= float(asks[0]["px"]):
            report["issues"].append({"file": "hl_l2_btc.json", "issue": "crossed_book"})
            nonlocal_ok[0] = False
        report.setdefault("coverage", {})["hyperliquid_l2"] = {
            "kind": "single_snapshot",
            "bid_levels": len(bids),
            "ask_levels": len(asks),
            "incremental_history": False,
        }
    report["ok"] = nonlocal_ok[0] and not any(i["issue"] != "not_downloaded" for i in report["issues"] if i["issue"] in {"checksum_mismatch", "missing", "invalid_price_or_size", "crossed_book"})
    text = json.dumps(report, indent=2)
    (root / "quality_report.json").write_text(text)
    print(text)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
