# References

Checked on 2026-11-11 unless a page says otherwise. "Verified" means the field
or URL was read from the official page or from a live public response.
"Observed" means a live capture exhibited it. "Assumption" is marked as such.

## Venues

- Hyperliquid docs index: https://hyperliquid.gitbook.io/hyperliquid-docs
- Websocket URLs (`wss://api.hyperliquid.xyz/ws`, testnet `wss://api.hyperliquid-testnet.xyz/ws`): https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket
- Subscriptions, including `l2Book`, `trades`, `bbo`, `activeAssetCtx`, `orderUpdates`, `userFills`: https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket/subscriptions
- Heartbeat `{"method":"ping"}` / `{"channel":"pong"}`, 60s idle close: https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket/timeouts-and-heartbeats
- Info endpoint (live POST `https://api.hyperliquid.xyz/info`): https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/info-endpoint
- Exchange endpoint `https://api.hyperliquid.xyz/exchange`, order action fields `a,b,p,s,r,t,c`: https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/exchange-endpoint
- Historical data, requester-pays buckets: https://hyperliquid.gitbook.io/hyperliquid-docs/historical-data
- Perps fee schedule, base tier taker 0.045% / maker 0.015%: https://hyperliquid.gitbook.io/hyperliquid-docs/trading/fees
- Lighter API reference and base URLs: https://docs.lighter.xyz/api
- Lighter websocket, order book nonce rule, trade channel: https://docs.lighter.xyz/api/websocket
- Lighter `orderBooks`, `orderBookOrders`, `trades`, `recentTrades`: https://docs.lighter.xyz/api
- Lighter fundings: https://docs.lighter.xyz/api/funding/fundings
- Lighter historical data, including the public parquet paths: https://docs.lighter.xyz/developers/integration-guides/historical-data

## Research

- Albers, Cucuringu, Howison, Shestopaloff (2021), *Fragmentation, Price Formation, and Cross-Impact in Bitcoin Markets*, arXiv:2108.09750. Used for the warning that short-horizon book features and taker PnL are different objects, and that the fee regime matters for cross-venue trading. Spark does not reproduce their R².
- Guéant, Lehalle, Fernandez-Tapia (2012), *Dealing with the Inventory Risk*. The quote formulas in `strategy.hpp` are the standard closed-form GLFT approximation of that model (intensity λ(δ)=A exp(-kδ), arithmetic Brownian mid). σ is a price increment, not a log-return, unless converted with `sigma_from_log_vol`.

## Engineering

- OpenHFT Chronicle Queue, https://github.com/OpenHFT/Chronicle-Queue. The journal is an append-only, checksummed, recoverable record stream with rotation. It does not link Chronicle and it is not memory-mapped. The resemblance is the idea of a durable ordered excerpt, not the implementation.
- Prior work inspected, not copied: https://github.com/chrislernunes/Data-Latency-Benchmark (separate receive and process timestamps, deterministic replay) and https://github.com/chrislernunes/_edgeflow (book, signals, fee-aware simulator). The repo named Latency-Benchmark was not found; Data-Latency-Benchmark is the public repository that matches that description.

## Live responses used as fixtures

- `POST /info {"type":"l2Book","coin":"BTC"}` returned `coin`, `time` (ms), `levels: [bids, asks]` of `{px, sz, n}`.
- `POST /info {"type":"recentTrades","coin":"BTC"}` returned `side` `B`/`A`, `px`, `sz`, `time`, `tid`.
- `POST /info {"type":"meta"}` returned BTC `szDecimals: 5`. That is the size scale, not a price tick.
- `GET /api/v1/orderBooks` on `mainnet.zklighter.elliot.ai` returned BTC perp `market_id: 1`, `supported_price_decimals: 1`, `maker_fee` and `taker_fee` strings `0.0000` at collection time.
- A websocket capture (`data/samples/live`) showed Lighter `subscribed/order_book` with a deep book and later updates containing `size: "0"`. Zero-size deletion is therefore observed on that session, not only assumed.
