# Risk model

`RiskEngine::check` runs before any live intent and does not call the strategy. The order of checks is the order in `execution.hpp`: kill switch, strategy halt, live authorization, size, position, net, gross, outstanding count, daily loss, drawdown, unhedged exposure, hedge delay, book sync, feed age, order rate.

`live_authorized` defaults to false. `config/paper.json` sets it false. A paper intent (the flag `live` is false) can pass the authorization check and still fail the book and size checks. The desk never sets `live`.

Feed age uses the venue timestamp and the local clock. Age below `-max_clock_skew_ns` (default 2s, exchange clock far ahead) is stale. Age above `max_feed_age_ns` is stale. A missing venue timestamp is stale. The engine does not replace a missing timestamp with local time.

Daily loss compares `realized_pnl` with `-max_daily_loss`. The test sets pnl to -100 against a limit of 100 and expects a reject. Unrealized pnl is not in that check. That is a limit of the check, not a claim that unrealized loss does not matter. Drawdown uses `peak_pnl - realized_pnl`.

The kill switch fails every check until `reset_kill`. The desk kill clears paper quotes. It cannot send a cancel-all. A real cancel-all has to be a signed exchange action, which this tree does not perform.

Position limits are in scaled lots. The caller supplies exposure. The engine does not read a wallet. After a restart the only acceptable source of position is the venue, not the journal. The journal can say what we believed. Reconciliation is `Unknown` until the venue says otherwise. That procedure is specified; the account query is not connected.

Rate limit state is an in-process vector of timestamps. It resets on restart. That is correct for a single process and wrong if two processes share an account. Do not run two until the limit is external.
