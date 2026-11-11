# Exchange protocols

Hyperliquid and Lighter are not interchangeable, and neither is a colocated
central-limit-order-book session of the kind a kernel-bypass feed handler
usually assumes. Both are request/response plus a websocket, with their own
acknowledgement and sequencing rules. Spark keeps venue identifiers and does
not flatten them into one order type beyond side, price, quantity and time in force.

## Hyperliquid (primary)

Verified:

- Market-data websocket: `wss://api.hyperliquid.xyz/ws`. Testnet: `wss://api.hyperliquid-testnet.xyz/ws`.
- Info: `POST https://api.hyperliquid.xyz/info`.
- Exchange actions: `POST https://api.hyperliquid.xyz/exchange`. Signing is required. The official page points at the Python SDK for EIP-712. Spark does not sign.
- Subscribe: `{"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}}`. Optional `nSigFigs`, `mantissa`, and `fast` (5 levels if true, 20 if false).
- `WsBook`: `coin`, `levels: [bids, asks]`, `time` in milliseconds. Each `WsLevel` is `px` (string), `sz` (string), `n` (order count). The official comment says the snapshot feed is pushed on each block that is at least 0.5s since the last push. There is no sequence number on this message.
- `WsTrade`: `coin`, `side` (`B` buy aggressor, `A` sell aggressor), `px`, `sz`, `time` ms, `tid`, `hash`, `users: [buyer, seller]`. `tid` alone is not a global id; the docs say use `(block_time, coin, tid)`.
- `bbo` is `WsBbo` with `bbo: [bid|null, ask|null]`, pushed when the touch changes.
- `activeAssetCtx` carries mark, funding and open interest context. The recorder subscribes to it and stores the payload. Strategy code does not invent fields from it.
- User streams (`orderUpdates`, `userFills`, `userEvents`) require an address. They are not subscribed without one. `userFills` has `isSnapshot` and `WsFill.crossed` (taker if true) plus `fee` (negative means a rebate).
- Order action, verified from the exchange-endpoint page: `a` asset, `b` isBuy, `p` price string, `s` size string, `r` reduceOnly, `t` limit `{tif: Alo|Ioc|Gtc}` or trigger, optional `c` cloid (128-bit hex). Cancel is `a` asset, `o` oid. Modify exists. Response statuses are `resting {oid}`, `filled {totalSz, avgPx, oid}`, or `error` string.
- A request that was written to the socket is not an acknowledgement. The order machine stays `PendingSubmit` until a status says resting or filled.
- Fees are account-tier. The published base perps tier is taker 0.045%, maker 0.015%. Config stores those numbers and says they are not an account's tier.
- `meta` gives `szDecimals` (BTC = 5) and `maxLeverage`. It does not give a price tick. Sampled BTC prices were whole numbers. The desk rounds Hyperliquid quotes to 1. That increment is observed, not a published tick table.
- Heartbeat: client `{"method":"ping"}`, server `{"channel":"pong"}`. The server closes a connection it has not written to for 60 seconds.
- Disconnects happen without notice. The official text says missed data is in the snapshot on resubscribe, and anything still missing has to be queried with the matching info request.

Not available, so not implemented as if it were:

- A sequenced incremental full order book.
- Per-order queue position.
- A free historical L2 archive. See DATA_SOURCES.md.

## Lighter (secondary)

Verified:

- REST: `https://mainnet.zklighter.elliot.ai`. Websocket: `wss://mainnet.zklighter.elliot.ai/stream`. Chain id 304. Testnet and Robinhood Chain hosts are different exchanges for account state; Spark targets Lighter mainnet only.
- Read-only from a restricted region: `?readonly=true`. The recorder uses that.
- BTC perp `market_id` is 1 (`orderBooks`). `supported_price_decimals` was 1, so the price quantum is 0.1. `supported_size_decimals` was 5.
- Order book subscribe: `{"type":"subscribe","channel":"order_book/1"}`. Responses use a colon: `order_book:1`.
- The first message is a snapshot (`subscribed/order_book`). Later messages (`update/order_book`) are state changes, about every 50ms.
- Continuity: `begin_nonce` of this update must equal `nonce` of the previous update. `offset` belongs to the API server and may jump on reconnect. Spark checks nonce and ignores offset as a sequence.
- Levels are `{price, size}` strings. A captured session contained many `size: "0"` updates, and replaying them as deletions produced a two-sided book with no nonce gap. That deletion rule is observed on 2026-11-11, not a sentence copied from the docs.
- Trade subscribe: `{"type":"subscribe","channel":"trade/1"}`. `timestamp` is Unix milliseconds. `is_maker_ask: true` means the taker bought.
- `GET /api/v1/orderBookOrders?market_id=1&limit=20` returns individual orders (`price`, `remaining_base_amount`, `order_id`), top 20 per side in the sample. The desk aggregates those for display and labels the source. It is not the websocket book.
- `maker_fee` / `taker_fee` on the BTC perp were the strings `0.0000` when collected. Spark stores the strings. It does not treat zero as a permanent schedule.
- Funding `GET /api/v1/fundings` needs `market_id`, `resolution` (`1h` or `1d`), `start_timestamp`, `end_timestamp`, `count_back`. `timestamp` on each row is Unix seconds. The rate is not subtracted from Hyperliquid's hourly rate. The units are not documented as identical, so Spark shows them side by side.

Signing and sending transactions require an API key. Spark does not implement that path and will not send an order while `live_authorized` is false. There is no testnet order in the paper config.

## Timestamp rules

| Clock | Meaning |
| --- | --- |
| exchange | Venue field, converted only when the unit is known (HL ms → ns, Lighter `last_updated_at` micros → ns, else envelope `timestamp` ms → ns) |
| recv | Local time the recorder or process read the bytes |
| process | Local time after parse |
| decision, submit, ack, fill | Separate fields. Zero means absent |

Feed age is `recv - exchange` and includes clock skew. It is not a one-way network delay. Spark rejects a book with no exchange timestamp rather than substituting local time.
