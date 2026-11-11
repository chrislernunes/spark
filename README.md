# Spark

Event-driven crypto execution and market-making infrastructure.
Hyperliquid is the primary venue. Lighter is the secondary venue.
The default configuration is paper trading. Live order entry is refused
unless `live_authorized` is set, and this repository does not contain a
signing implementation or any credential.

Spark is not a profitability claim. Simulated fills are model outputs.
They are not exchange executions.

## What is implemented

| Piece | State |
| --- | --- |
| Scaled prices, clocks that are not substituted | Implemented and tested |
| SPSC ring and bounded MPMC queue | Implemented and tested |
| Hyperliquid l2Book / trade parsers and truncated-book replace | Implemented and tested |
| Lighter order-book parser, nonce chain, size-0 delete | Implemented and tested, including a live capture replay |
| Binary journal, torn-tail recovery, replay fingerprint | Implemented and tested |
| Order lifecycle state machine, including Unknown after disconnect | Implemented and tested |
| Risk gate and kill switch. Live default off | Implemented and tested |
| GLFT inventory quotes and censored intensity MLE | Implemented and tested. Intensity is not calibrated on our fills |
| Cross-venue touch edge, fees explicit | Implemented and tested. Not a backtest |
| Conservative queue simulator and execution metrics | Implemented and tested on scripted cases. Not validated against our orders |
| epoll reactor | Implemented and tested on a socketpair. No TLS, no kernel timestamps |
| Public REST ingest, checksums, quality report | Implemented. Run produced the files in `data/samples` |
| Public WebSocket recorder | Implemented. An 6-second capture is in `data/samples/live` |
| Short-horizon trade association | Implemented. Sample is too small to infer anything on Hyperliquid |
| Live signed order entry | Blocked. No key, no signer |
| Hyperliquid historical L2 archive | Blocked. Requester-pays S3, not downloaded |
| Queue model validated on our own fills | Not started. We have no own fills |

## Build and test

Requires `g++` with C++20. CMake is optional; the Makefile is what this tree was built with.

```bash
cd spark
make test          # unit tests, including the Lighter capture replay
make asan          # AddressSanitizer + UBSan
make tsan          # ThreadSanitizer
make bench         # local processing timings, printed, not asserted
./build/sparkctl quote --mid 100 --q 0 --sigma 0.1 --A 1 --k 1.5 --gamma 0.1 --xi 0.1
```

Python, from the same directory:

```bash
python3 python/ingest.py --out data/samples
python3 python/validate.py data/samples
python3 python/evaluate.py data/samples
python3 python/record_public.py --seconds 6 --out data/samples/live
```

Paper configuration: [config/paper.json](config/paper.json). `live_authorized` is false.

## Layout

`include/spark/` is the library. `tests/test_main.cpp` is the test driver.
`python/` downloads and checks public data. `docs/` records venue facts,
the event schema, the queue model, and what was actually measured.

## Non-claims

- Hyperliquid `l2Book` is a top-of-book snapshot (20 levels, or 5 if `fast`), not a sequenced full-depth delta. The book state is `LiveTruncated`.
- Lighter's public stream does continue a nonce (`begin_nonce` must equal the previous `nonce`). `offset` is not that sequence. A captured session replayed without a gap; that is one session, not a general guarantee.
- Queue position is not published. The simulator joins the back of the displayed size and, by default, ignores cancellations. That is a bound, not a reconstruction.
- Local benchmark numbers are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md). They are userspace timings on the machine that ran them. They are not exchange latency and not NIC timestamps.
