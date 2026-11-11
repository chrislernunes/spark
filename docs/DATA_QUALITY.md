# Data quality

`python/validate.py` checks sha256 against the manifest, rejects non-positive prices, counts duplicate trade ids, and reports the timestamp span after sorting. Pages arrive newest-first. That order is not reported as a clock regression.

On the run that produced `data/samples/quality_report.json`:

- every downloaded file matched its sha256
- Hyperliquid trades: 10 prints, not a continuous history
- Lighter trades: 400 prints, not the parquet archive
- Hyperliquid L2: one snapshot, 20 and 20, not crossed, not an incremental history

The live capture summary:

- Hyperliquid, 6 seconds: 4 subscription acks, 2 `l2Book`, 4 `trades`, 6 `activeAssetCtx`, 29 `bbo`
- Lighter, 6 seconds: 1 connected, 100 `order_book:1`, 21 `trade:1`, 0 nonce gaps

Missing intervals are the entire history outside those windows. They are missing because we did not download them, not because the validator dropped them.

Duplicate detection is by `tid` or `trade_id` inside one file. None failed the price check on that run.

Sequence gaps apply to the Lighter websocket nonce, not to Hyperliquid. The C++ replay of the Lighter file is the gap test, and it passed for this file. One file is not a monitoring system.

Invalid rows would be listed under `issues` and left on disk. Do not "clean" a file by rewriting it without keeping the original.
