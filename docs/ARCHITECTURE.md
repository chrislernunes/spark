# Architecture

Spark is a library plus a recorder, not a single trading process that hides its state.

```
public websocket / REST
        │
        ▼
python/record_public.py  and  python/ingest.py     capture, checksum, no strategy
        │
        ▼
include/spark/venues.hpp     parse only what the venue documented or we observed
        │
        ▼
OrderBook                     truncated snapshot (HL) or nonce incremental (Lighter)
        │
        ├── RiskEngine        can refuse. Does not know the quote formula
        ├── strategy.hpp      GLFT and cross-venue edge. Does not send
        ├── OrderFsm          PendingSubmit is not Live
        └── QueueSimulator    paper fills, conservative ahead
        │
        ▼
Journal                       ordered records, separate timestamps
        │
        ▼
replay                        same snapshots, same fingerprint
```

Decisions:

- Integer prices at 1e8. Accounting does not use double. The GLFT formula does, and the result is rounded bid-down, ask-up onto a tick before it can be treated as an order price.
- One book type, two apply paths. Forcing Lighter's nonce updates into Hyperliquid's snapshot replace would drop the gap check. Forcing Hyperliquid's snapshot into an incremental apply would invent levels the venue did not send.
- The epoll reactor is real and local. Venue IO is the Python recorder because this environment has libcurl and libssl shared objects but not the development headers needed to link a TLS client. Pretending the reactor speaks `wss://` would be false.
- Live signing is a separate, absent module. The risk gate's default makes that absence safe: a live intent is rejected.
- The browser desk repeats the quote and queue formulas so a person can see them on live public books. `sparkctl` is the parity check for the quote. The desk is not a second order manager and it does not submit.

Threads: tests are single-threaded except the sanitizer run. The SPSC queue is safe for one producer and one consumer; the bench does not exercise two threads. The MPMC queue takes a mutex. That choice is documented in `core.hpp` and stands until a contended benchmark says otherwise.
