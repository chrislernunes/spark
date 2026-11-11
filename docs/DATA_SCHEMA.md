# Data schema

Internal prices and quantities in the C++ book are int64 with scale 1e8. Zero exchange time means the field was absent.

## Sample files

`data/samples/manifest.json` is `spark.manifest.v1`. Each downloaded file has `source`, `kind`, `path`, `bytes`, `sha256`, `collected_at_unix_ms`.

| File | Original fields kept |
| --- | --- |
| `hl_l2_btc.json` | Hyperliquid `l2Book` object as returned |
| `hl_trades_btc.json` | array of `recentTrades` |
| `hl_funding_btc.json` | array of `fundingHistory` |
| `hl_btc_meta.json` | reduced: universe count plus the BTC object. The other 200+ names were not stored |
| `lighter_btc_meta.json` | the BTC perp object from `orderBooks` |
| `lighter_orders_btc.json` | `orderBookOrders` response |
| `lighter_trades_btc.json` | `{code, trades}` from paginated `/trades` |
| `lighter_funding_btc.json` | fundings response, timestamps in seconds |
| `live/*.jsonl` | one JSON object per line: venue, recv_ns, process_ns, exchange_ns, channel, payload |

Schema version of the journal is separate and is `kJournalVersion = 1` in `journal.hpp`.

Quality status is the report next to the files, not a column inside each row. A bad row is listed. The validator does not delete it.

UTC: exchange fields stay in the unit the venue used. `exchange_ns` on a jsonl line is the conversion described in EXCHANGE_PROTOCOLS.md. Local `recv_ns` is `time.time_ns()` on the machine that recorded, which is UTC based on the system clock and is not a disciplined PTP clock.
