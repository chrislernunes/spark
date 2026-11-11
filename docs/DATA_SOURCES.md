# Data sources

Collected 2026-11-11 with `python/ingest.py` and `python/record_public.py`.
Checksums are in `data/samples/manifest.json`. The quality report is
`data/samples/quality_report.json`.

## Hyperliquid

| Data | Coverage actually obtained | Timestamp | Snapshot or incremental | Mechanism | Limit |
| --- | --- | --- | --- | --- | --- |
| L2 book | One live snapshot, 20x20, plus a 6s websocket recording of `l2Book` (2 snapshots in that window) | ms | Snapshot of published depth. Not a delta. Not full depth | `info` `l2Book`; websocket `l2Book` | No sequence number. Pushed at most about every 0.5s per the docs |
| Trades | `recentTrades` returned 10 prints. Websocket added 4 in 6s | ms | Prints | `info` `recentTrades`; websocket `trades` | Not the historical fill archive |
| Funding | 48 hourly rows from `fundingHistory` over the requested window | ms | Periodic | `info` `fundingHistory` | Hourly rate, not a tick history |
| Instrument | BTC row of `meta`: `szDecimals` 5, `maxLeverage` 40 | n/a | Metadata | `info` `meta` | No price tick field |
| Historical L2 | Not downloaded | — | The docs say L2 snapshots exist under `s3://hyperliquid-archive/market_data/[date]/[hour]/l2Book/[coin].lz4` | AWS CLI `--request-payer requester` | Requester pays. No guarantee the archive is complete or timely. This is still snapshots, not a full incremental history |
| Historical trades | Not downloaded | — | `s3://hl-mainnet-node-data/node_fills_by_block` and older `node_fills` / `node_trades` | Requester-pays S3 | Different formats across prefixes. Not fetched |

Official page: https://hyperliquid.gitbook.io/hyperliquid-docs/historical-data

Rate limits exist (`rate-limits-and-user-limits` in the docs). The ingester retries HTTP 429 with backoff. It does not claim a numeric budget it has not measured.

License: public market data from the exchange API. The S3 archive is requester-pays; using it means paying transfer. No credential was stored.

## Lighter

| Data | Coverage actually obtained | Timestamp | Snapshot or incremental | Mechanism | Limit |
| --- | --- | --- | --- | --- | --- |
| Order book | Websocket: one snapshot (1602 bids, 2175 asks on that session) then 99 updates, 0 nonce gaps, 425 zero-size levels. REST: top 20 orders per side | `last_updated_at` microseconds; envelope `timestamp` milliseconds | Snapshot then incremental state changes | `order_book/1`; REST `orderBookOrders` | `offset` is not a sequence. REST top-20 is not the full book |
| Trades | 400 prints via paginated `GET /api/v1/trades`, plus 21 websocket trades in 6s | ms | Prints | REST cursor; websocket `trade/1` | Not the parquet archive |
| Funding | 48 hourly rows | seconds | Periodic | `GET /api/v1/fundings` | Do not net against Hyperliquid without a unit check |
| Instrument | BTC perp market 1, fee strings, decimals, open interest | n/a | Metadata | `GET /api/v1/orderBooks` | Fee strings were 0.0000 at collection. Treat as observed metadata |
| Bulk history | Not downloaded | daily parquet | Trades and hourly open interest from genesis 2025-01-17, per the docs | `s3://aws-public-blockchain/v1.1/lighter/core/mainnet/trades/date=YYYY-MM-DD/trades.parquet` with `--no-sign-request` | AWS CLI was not used in this environment. The path is documented, the files were not fetched |

Official page: https://docs.lighter.xyz/developers/integration-guides/historical-data

No third-party vendor was assumed to have data. None was used.

## What a snapshot is not

A Hyperliquid `l2Book` message replaces the published levels. Levels that fell out of the top 20 are unknown, not zero. Spark marks that book `LiveTruncated`. Research that needs queue depletion inside the visible level can use the stream. Research that needs the full historical book cannot use these files and must not be told that it can.
