# Benchmarks

These numbers were printed by `make bench` in this environment. They are not
targets, and they are not exchange round-trip times.

## Method

- Host: Linux 6.12.8+, x86_64, Intel Xeon Platinum 8481C @ 2.70GHz, 2 logical CPUs.
- Compiler: g++ 12.2.0, `-std=c++20 -O2 -Wall -Wextra -pthread`. No `-march=native` beyond the compiler default. No PGO. No CPU affinity.
- Clock: `CLOCK_MONOTONIC` around each iteration.
- Workload: the captured Hyperliquid BTC `l2Book` fixture (20 levels per side), parsed and applied 20,000 times in one thread.
- SPSC: one thread pushing and popping 200,000 integers on an 1024-slot ring. This is a same-thread throughput figure. It is not cross-core latency. Do not quote it as an inter-thread hop.
- GLFT: 20,000 closed-form quotes.
- Journal: 5,000 snapshot records through `std::ofstream`, then a full read. This is buffered file IO, not a memory-mapped Chronicle-style journal.
- Percentiles: nearest rank on the sorted sample, index `q * (n-1)`.

## Result of the run recorded with the tests

```
samples_parse 20000
parse_ns p50 15554 p90 15948 p99 31465 max 498647
book_apply_ns p50 3438 p90 3530 p99 4754 max 586070
spsc_roundtrip_ns_per_op 1.63 count 200000
glft_ns p50 27 p99 29
journal_append_ns_per 9721.07 replay_ns_per 9778.86 records 5000
```

Re-run `make bench` before citing these. The max values are single outliers and are not a tail model; n=20000 does support a p99, and it does not by itself support a stable p99.9. `summarize_ns` refuses p99.9 below 1000 samples and the bench does not print p99.9 for that reason.

## What was not measured

- Network receive to parse. The bench parses a buffer already in memory.
- Strategy decision to a signed request. There is no signer.
- Acknowledgement or cancel latency on either exchange. That requires an authorized account and a clock-sync story this process does not have.
- Kernel `SO_TIMESTAMPING`. It is not enabled. Userspace time after `read` is later than a NIC timestamp, and a NIC timestamp would still not remove the difference between the venue clock and ours.
- Allocation rate and RSS. Not instrumented in this run. The JSON parser allocates; that is visible in the parse time and is an obvious later target, not a claimed optimization.

AddressSanitizer and ThreadSanitizer both ran the unit tests clean on the same compiler.
