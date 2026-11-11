# Benchmark methodology

See [BENCHMARKS.md](BENCHMARKS.md) for the numbers and the machine.

Stages the specification asked for, and where each one stands:

| Stage | Measured? |
| --- | --- |
| Network receive to parse | No. Parse is of a buffer already in memory. Receive time is stored by the Python recorder and is not paired with this bench. |
| Parse to book update | Yes. `book_apply_ns` starts after `parse_json` returns and ends after `apply_snapshot`. |
| Book to features | Not a separate timer. Mid, microprice and imbalance are O(1) or O(top) and are inside the book call in production use. They are not the parse cost. |
| Features to decision | `glft_ns` is the closed form only, with parameters already in registers. It does not include feature calculation. |
| Decision to submit | Not measured. Submit is not implemented. |
| Submit to ack | Not measured. Requires an authorized order. |
| Cancel to confirm | Not measured. Same reason. |

p50, p90, p99 and max are reported for parse and book apply. p99.9 is withheld unless n ≥ 1000 in `summarize_ns`; the bench does not print it. CPU, RSS and allocation counts were not sampled. I am not filling them in.

Comparing a local parse time with an internet round trip to Hyperliquid or Lighter is a category error. The venues answer from their own networks. Clock offset between us and them is unknown beyond the feed-age guard of 1.5s / 2s in the risk engine. Kernel timestamps would not remove that offset.

Re-run before and after any change you intend to call an optimization. The current code has not had an optimization pass. The JSON parser allocates, and that is the obvious cost in the 15 microsecond parse. It stays until a profile says the surrounding system is waiting on it.
