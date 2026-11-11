# Quantitative validation

No strategy in this repository is labelled profitable.

## What the quote is

GLFT gives a reservation and a half-spread from mid, inventory in quote-size units, price volatility per square-root second, and an intensity λ(δ)=A exp(-kδ). The desk's A and k are sliders. `fit_intensity` will not set `calibrated` below 30 filled observations. We have no such observations. The reason string is `ok_uncalibrated_intensity`.

A long inventory (positive q) lowers the reservation. The test checks the direction, not a pnl.

## What the signal script is

`python/evaluate.py` correlates aggressor sign with the log return to a later print. That is not a mid-to-mid forecast and it is not a strategy. On this sample:

- Hyperliquid: 10 prints, 9 aligned at 100ms to 1s, none at 5s. Status `not_supported`.
- Lighter: 400 prints, correlation about -0.33 at 100ms through -0.16 at 5s. Status `descriptive_only`. A negative sign means the next print was, in this window, on average against the aggressor. Fees are not applied. The window is a few minutes of one market. It is not a walk-forward, not a multiple-testing correction, and not evidence of a maker edge. Albers et al. (2021) are cited because they measured how fast that kind of conclusion dies once fees and a real order are included.

## Rules the research path has to keep

- Features at time t use events with exchange time ≤ t. The trade script only has prints, so it cannot see the book that existed before the print. It does not pretend to.
- In-sample and out-of-sample are not split here because there is no fitted trading parameter being promoted. Fitting A, k on these 400 prints and then quoting a pnl would be the leak this note is here to prevent.
- Walk-forward, regime splits, and capacity are unimplemented because there is no strategy return series.
- Cross-venue research must use the executable touch minus fees (`cross_edge`), not the mid basis. Funding is not in that number.

Gross pnl, net pnl, turnover, drawdown and capacity are not reported. There is nothing to put in those cells that would not be simulated theatre.
