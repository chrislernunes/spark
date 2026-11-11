# Operations

```bash
make test          # must print: spark tests passed
make asan
make tsan
make bench         # read the lines, do not check them into a dashboard as SLOs
./build/sparkctl quote --mid 100 --q 0 --sigma 0.1 --A 1 --k 1.5 --gamma 0.1 --xi 0.1
python3 python/ingest.py --out data/samples
python3 python/validate.py data/samples
python3 python/evaluate.py data/samples
python3 python/record_public.py --seconds 6 --out data/samples/live
```

`config/paper.json` is the only runtime configuration. If `live_authorized` is ever set true, the process still cannot sign, so the flag would only remove the risk rejection. Do not set it until a signer and a cancel-all exist and have been tested against a testnet or a tiny authorized account.

Recovery after a bad Lighter nonce: drop the book, resubscribe, wait for `subscribed/order_book`, then apply updates. Do not apply the update that failed the nonce check.

Recovery after a Hyperliquid disconnect: resubscribe. The next `l2Book` replaces the truncated book. Trades during the gap are gone unless you backfill from `recentTrades`, which only holds a short tail. Record that hole. Do not interpolate prices across it.

The desk poll is one second, server-side, public REST. It will show a stale banner if the Hyperliquid exchange timestamp and the server clock differ by more than 1.5 seconds. That banner is the feed-age control for the paper quotes.
