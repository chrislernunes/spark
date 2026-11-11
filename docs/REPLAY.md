# Replay

```bash
make test
```

The journal test is the replay. It writes two book snapshots with distinct exchange, receive, and process timestamps, appends four torn bytes, reads the file back, and requires:

- exactly two records
- the original timestamps
- the same `fingerprint()` as the book built from those payloads

`fingerprint` mixes sync state, exchange time, nonce, and every level. It is a regression check, not a hash you should publish as an integrity proof. Integrity of the file is the per-record CRC.

What replay does not do yet: re-run the GLFT function inside the journal reader. The quote function is pure, so the same mid, inventory and parameters produce the same quotes. The test covers that purity directly. Wiring every decision record back through the strategy is straightforward and not yet a separate binary. I am not calling that a replay of a strategy session.

Nondeterminism that is isolated on purpose:

- `Clock::realtime_ns` and `monotonic_ns`
- anything that would read the network
- the proportional queue mode's floating multiply, which the default mode does not use

The Lighter capture replay is a second, stronger check: the recorded websocket lines are parsed and applied in order, and the book must stay incremental and tradable with a valid mid. Same file, same result.

There is no wall-clock sleep in replay. Speed is "as fast as the reader". Stepping is "the test applies one record at a time", which is the loop in `test_journal_replay` and `test_live_lighter_capture`. A checkpoint is an offset the reader can be given later; the file format supports scanning from the start and stopping, and does not yet store an index of offsets. Scanning is correct and is not indexed.
