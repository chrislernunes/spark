# Simulator

`QueueSimulator` in `include/spark/sim.hpp`. Rules and the honesty limits are in [QUEUE_MODEL.md](QUEUE_MODEL.md).

The desk runs the same conservative rule on public Hyperliquid prints after you arm paper quotes. A fill shown there is that function, on those prints, with `ahead` taken from the displayed size at the quoted price. It is not an order the exchange saw.

Latency enters as `ack_ns = submit_ns + ack_latency`. The library test uses 50ns only as a control value so a print at 20ns cannot fill. It is not a measured acknowledgement delay.

Fees, slippage and adverse selection for a hypothetical fill are `execution_metrics`:

- effective spread = `2 * side_sign * (fill - mid_at_fill) / mid_at_fill`. Positive is a cost for that side.
- realized spread at a horizon = `2 * side_sign * (fill - mid_h) / mid_at_fill`. Positive means the later mid moved in favor of the liquidity provider who sold high or bought low.
- adverse selection = `side_sign * (mid_h - mid_fill) / mid_fill`. For a buy, a rising mid is positive. That is the buyer's markout, not automatically the maker's loss. The name in code is the sign, not a slogan.
- implementation shortfall = `side_sign * (fill - decision_mid) * qty + fee`, in quote currency.

Horizons the research note cares about (100ms, 500ms, 1s, 5s, 30s) need a book or a print at that time. The trade-only evaluator does not have a mid path, so it does not emit those markouts. It says so.

Do not read a simulator fill count as a live fill rate.
