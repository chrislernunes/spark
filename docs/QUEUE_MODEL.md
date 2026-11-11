# Queue model

Neither venue tells Spark where a resting order sits in the queue.

Hyperliquid publishes aggregated levels and an order count `n`, for the top of book only. Lighter's websocket publishes price levels (and the REST order endpoint publishes individual order ids and remaining size for the top of the book). An order we have not had acknowledged has no priority key on either venue.

## Default, conservative

1. Until acknowledgement time, the order cannot fill. A print that happens before `ack_ns` is ignored.
2. At acknowledgement, `ahead` is the displayed quantity at our price. We join the back. If the level is empty, `ahead` is 0. That is still not a promise about hidden size.
3. A print at our price reduces `ahead` first and then our leaves. A print strictly through our price clears `ahead` and then fills us, because the level at our price had to be consumed to reach that print. This is an inference from price-time priority. It is not a published matching rule for either venue's block engine, and it is labelled as such in the simulator.
4. If the displayed size falls by more than the prints at that price, the difference is a cancellation (or a revision the feed did not separate). In `ConservativeBehind` mode those cancellations are assumed to sit behind us. They do not improve `ahead`. This lowers fill probability relative to a proportional model.
5. `Proportional` mode removes `cancel * ahead / previous_size` from `ahead`. It is a research switch. It is not a reconstruction, and tests do not call its output a fill probability.

## What was tested

Scripted cases in `tests/test_main.cpp`:

- A print before ack does not fill.
- With 5 ahead and a print of 6 at our price, we fill 1.
- A displayed-size drop with no print leaves `ahead` unchanged in conservative mode.
- A proportional drop does reduce `ahead`.

The Lighter capture confirms that size 0 is a real update and that the nonce chain held for that session. It does not tell us our queue position, because we did not place an order.

## What would count as validation

Paper or small live orders, with submission, ack, cancel and fill timestamps, compared with the simulator's fill / no-fill and time-to-fill on the same prints. Sample size, confidence interval, and a split by venue and instrument. A handful of fills is not a validation. Spark has zero own fills, so the model is **implemented and tested, not validated**.
