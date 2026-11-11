# Execution model

An order moves through `OrderFsm` and nowhere else.

`Created → PendingSubmit → Live → PartiallyFilled → Filled`

Cancels: `Live|PartiallyFilled|PendingSubmit → PendingCancel → Canceled`. A fill can still arrive in `PendingCancel`.

Rejects are terminal. A duplicate ack on an already-working order is ignored.

If the session drops in `PendingSubmit`, `PendingCancel`, or `PendingModify`, the state becomes `Unknown`. It stays there until either:

- the exchange open-order snapshot contains it (`reconcile_live`), or
- the fill stream is known complete and the order is absent (`reconcile_absent`). If the fill stream has a gap, absence does not cancel the order. The local book of orders is not trusted over the venue.

Hyperliquid's exchange response is the acknowledgement: `resting`, `filled`, or `error`. Lighter would acknowledge by the transaction result and then by the order and trade streams. That client is not implemented. The state machine does not special-case a venue that has not answered.

Partial fills reduce leaves. A fill larger than leaves is rejected and the state does not change.

`OrderManager::mark_disconnect` applies the disconnect transition to every order. It does not guess which ones were accepted.

Ids: `client_id` is local and monotonic. `venue_id` is empty until the venue assigns one (Hyperliquid `oid`, Lighter `order_index` / `order_id`). They are not interchangeable. Hyperliquid cloid is a 128-bit hex. Spark does not generate one until a signer exists.

Idempotency: a second ack does not double-apply. A second fill of the same quantity is not de-duplicated by trade id yet, because no live fill stream is connected. The recorder keeps `tid` / `trade_id` so a later fill applicator can. This is a real gap, stated in GAP_CLOSURE.md, not a silent success.

Fees on a hypothetical hedge:

- Buy passively on Lighter at `maker_bid`, sell aggressively on Hyperliquid at `hedge_bid`:
  `hedge_bid * (1 - hl_taker) - maker_bid * (1 + lt_maker)`.
- The opposite direction uses the Lighter ask and the Hyperliquid ask.
- Basis of mids is reported and is not the edge.
- Funding is not added. The two published rates do not share a documented unit.

No component marks an order filled because the price traded through it, except the queue simulator, and that path is labelled simulation.
