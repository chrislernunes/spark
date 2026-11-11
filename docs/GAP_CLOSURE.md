# Gap closure

There was no Spark tree in the workspace when this work started. Nothing below
is a report against a previous implementation. Status words mean what the tests
and the captured files show.

| Requirement | State | Evidence | Not done |
| --- | --- | --- | --- |
| Hyperliquid market data | Implemented and tested | Parser on a live `l2Book` fixture; websocket capture `l2Book`, `trades`, `bbo`, `activeAssetCtx` | No address, so no user-fill stream |
| Lighter market data | Implemented and tested | Nonce replay of `data/samples/live/lighter.jsonl` (snapshot + 99 updates, 0 gaps) | REST desk view is top-20 orders, not that full stream |
| Order book | Implemented and tested | Snapshot replace, absolute size, zero delete, crossed book rejected for mid | HL book is truncated by the venue |
| Timestamps | Implemented and tested | Journal stores exchange, recv, and process separately; the test checks they differ | No NIC timestamp, no PTP |
| Journal and replay | Implemented and tested | Torn tail ignored; fingerprint matches | Not memory-mapped |
| Order state machine | Implemented and tested | Pending to Unknown on disconnect; absent reconcile refused if fills are gapped | No live reconcile against an account |
| Risk | Implemented and tested | Live refused by default; kill; stale; size; loss | Not wired to a real cancel-all, because no orders are sent |
| GLFT | Implemented and tested | Closed form matches `sparkctl`; long inventory skews the reservation down | A, k not fit on our fills (`min_fills` 30, and we have 0) |
| Cross-venue edge | Implemented and tested | Touch minus configured fees | No hedge order, no measured hedge delay |
| Queue simulator | Implemented and tested | Scripted ahead / through / cancel cases | Not validated. No own fills |
| Execution metrics | Implemented and tested | Sign convention covered by one synthetic sample | No empirical distribution |
| Signals | Implemented but not validated | `evaluate.py` on 10 HL prints (inference refused) and 400 Lighter prints (descriptive correlation only) | No book-aligned future mid, no walk-forward PnL |
| Historical L2 | Blocked | Docs identify the S3 layout | Requester-pays, not downloaded |
| Lighter parquet history | Blocked on fetch | Path recorded from the official page | AWS CLI pull not run |
| epoll reactor | Implemented and tested | socketpair round trip | No TLS client, so it does not speak to the venues |
| Live trading | Blocked | `config/paper.json` `live_authorized: false` | No signer, on purpose |
| Deployment supervisor | Partially implemented | Paper config, recorder, risk kill in the library and the desk | No systemd unit running against an account. See DEPLOYMENT.md |

Tests executed here: `make test` (pass), `make asan` (pass), `make tsan` (pass), `make bench` (numbers in BENCHMARKS.md), `python3 python/ingest.py`, `validate.py` (checksums matched), `evaluate.py`, `record_public.py` (Hyperliquid 45 messages, Lighter 122 messages, 0 nonce gaps).

Data acquired: the files under `data/samples/`. Data not acquired: Hyperliquid S3 L2 and fills, Lighter public parquet, any private fill.
