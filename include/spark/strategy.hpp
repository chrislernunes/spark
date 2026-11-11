#pragma once

// Inventory-aware quoting from Guéant, Lehalle, Fernandez-Tapia (2012),
// "Dealing with the Inventory Risk", in the closed form used by subsequent
// GLFT approximations:
//
//   λ(δ) = A exp(-k δ)
//   c1 = 1/(ξ Δ) * log(1 + ξ Δ / k)
//   c2 = sqrt( (γ / (2 A Δ k)) * (1 + ξ Δ / k) ^ (k / (ξ Δ) + 1) )
//   half = c1 + (Δ / 2) * σ * c2
//   skew = σ * c2
//   reservation = mid - skew * q
//   bid = reservation - half
//   ask = reservation + half
//
// σ is the arithmetic Brownian increment of price per sqrt(second), NOT a
// log-return. Convert with sigma_from_log_vol().
// q is inventory in units of the strategy's quote size (a fill of one quote
// changes q by ±1), not raw contracts. Passing raw BTC into q makes the skew
// economically meaningless.
//
// These quotes are model prices. They are not executable until tick rounding,
// fee, latency and queue position are applied.

#include "spark/core.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace spark {

struct GlftParams {
  double gamma = 0.1;   // inventory risk aversion
  double xi = 0.1;      // terminal-penalty / intensity scaling; often set equal to gamma
  double A = 0.0;       // intensity level. 0 means uncalibrated.
  double k = 0.0;       // intensity decay per unit of price distance
  double sigma = 0.0;   // price units per sqrt(second)
  double delta_t = 1.0;
  bool calibrated = false;
};

struct GlftQuotes {
  bool ok = false;
  const char* reason = "";
  double reservation = 0;
  double bid = 0;
  double ask = 0;
  double half_spread = 0;
  double skew = 0;
  double c1 = 0;
  double c2 = 0;
};

inline double sigma_from_log_vol(double log_sigma_per_sqrt_s, double mid_price) {
  return log_sigma_per_sqrt_s * mid_price;
}

inline GlftQuotes glft_quotes(double mid, double q, const GlftParams& p) {
  GlftQuotes o;
  if (!(mid > 0) || !(p.gamma > 0) || !(p.xi > 0) || !(p.A > 0) || !(p.k > 0) || !(p.sigma > 0) ||
      !(p.delta_t > 0)) {
    o.reason = "non_positive_parameter";
    return o;
  }
  const double xi_dt = p.xi * p.delta_t;
  const double base = 1.0 + xi_dt / p.k;
  if (!(base > 1.0)) {
    o.reason = "intensity_base";
    return o;
  }
  o.c1 = (1.0 / xi_dt) * std::log(base);
  const double exponent = (p.k / xi_dt) + 1.0;
  const double vol_term = (p.gamma / (2.0 * p.A * p.delta_t * p.k)) * std::pow(base, exponent);
  if (!(vol_term > 0) || !std::isfinite(vol_term)) {
    o.reason = "c2_domain";
    return o;
  }
  o.c2 = std::sqrt(vol_term);
  o.half_spread = o.c1 + 0.5 * p.delta_t * p.sigma * o.c2;
  o.skew = p.sigma * o.c2;
  o.reservation = mid - o.skew * q;
  o.bid = o.reservation - o.half_spread;
  o.ask = o.reservation + o.half_spread;
  if (!(o.ask > o.bid) || !std::isfinite(o.bid) || !std::isfinite(o.ask)) {
    o.reason = "crossed_or_nan";
    return o;
  }
  o.ok = true;
  o.reason = p.calibrated ? "ok" : "ok_uncalibrated_intensity";
  return o;
}

// Round model prices onto the instrument tick.
// Bids round down, asks round up, so the order is never more aggressive than the model.
inline int64_t round_bid_tick(double px, int64_t tick) {
  if (!(tick > 0) || !std::isfinite(px)) return 0;
  return static_cast<int64_t>(std::floor(px / static_cast<double>(tick))) * tick;
}
inline int64_t round_ask_tick(double px, int64_t tick) {
  if (!(tick > 0) || !std::isfinite(px)) return 0;
  return static_cast<int64_t>(std::ceil(px / static_cast<double>(tick))) * tick;
}

// Censored exponential-intensity profile MLE.
// Observation: quote distance `delta` (price units), observation window `t` seconds,
// `filled` if a trade consumed the quote inside the window.
// λ(δ) = A exp(-k δ)
// For fixed k, A_hat = N_fills / sum_i exp(-k δ_i) t_i
// k is chosen on a grid by maximising the profile log-likelihood.
// Fewer than `min_fills` fills => not calibrated. The returned params must not
// be described as validated.
struct IntensityObs {
  double delta = 0;
  double t = 0;
  bool filled = false;
};

struct IntensityFit {
  bool calibrated = false;
  const char* reason = "";
  double A = 0;
  double k = 0;
  double loglik = 0;
  int fills = 0;
  int n = 0;
};

inline IntensityFit fit_intensity(const std::vector<IntensityObs>& obs, int min_fills = 30) {
  IntensityFit best;
  best.n = static_cast<int>(obs.size());
  for (const auto& o : obs)
    if (o.filled) ++best.fills;
  if (best.fills < min_fills) {
    best.reason = "insufficient_fills";
    return best;
  }
  bool any = false;
  for (int i = 1; i <= 80; ++i) {
    const double k = 0.05 * i;  // 0.05 .. 4.0 per price unit
    double denom = 0;
    for (const auto& o : obs) {
      if (!(o.t > 0) || !(o.delta >= 0)) continue;
      denom += std::exp(-k * o.delta) * o.t;
    }
    if (!(denom > 0)) continue;
    const double A = static_cast<double>(best.fills) / denom;
    double ll = 0;
    for (const auto& o : obs) {
      const double lam = A * std::exp(-k * o.delta);
      if (o.filled) ll += std::log(lam) - lam * o.t;
      else ll += -lam * o.t;
    }
    if (!any || ll > best.loglik) {
      any = true;
      best.A = A;
      best.k = k;
      best.loglik = ll;
    }
  }
  if (!any || !(best.A > 0) || !(best.k > 0)) {
    best.reason = "fit_failed";
    return best;
  }
  best.calibrated = true;
  best.reason = "profile_mle";
  return best;
}

// Cross-venue economics. Primary hedge venue is Hyperliquid. Maker venue is Lighter.
// A buy on Lighter is hedged by selling on Hyperliquid's bid (taker, unless configured otherwise).
// Edge is per 1 unit of base, in quote currency, using touch prices — not mids.
struct FeeSchedule {
  double maker = 0;  // fraction, e.g. 0.00015. Negative would be a rebate.
  double taker = 0;
  bool from_exchange = false;
};

struct CrossEdge {
  bool ok = false;
  const char* reason = "";
  // Buy maker-venue at its bid (join), sell hedge-venue at its bid.
  double buy_maker_sell_hedge = 0;
  // Sell maker-venue at its ask (join), buy hedge-venue at its ask.
  double sell_maker_buy_hedge = 0;
  double basis_mid = 0;  // maker_mid - hedge_mid. Informational only.
};

inline CrossEdge cross_edge(double maker_bid, double maker_ask, double hedge_bid, double hedge_ask,
                            FeeSchedule maker_fee, FeeSchedule hedge_fee) {
  CrossEdge e;
  if (!(maker_bid > 0) || !(maker_ask > maker_bid) || !(hedge_bid > 0) || !(hedge_ask > hedge_bid)) {
    e.reason = "crossed_or_empty_book";
    return e;
  }
  // Passive buy pays maker fee on the price paid. Aggressive sell receives bid after taker fee.
  e.buy_maker_sell_hedge =
      hedge_bid * (1.0 - hedge_fee.taker) - maker_bid * (1.0 + maker_fee.maker);
  e.sell_maker_buy_hedge =
      maker_ask * (1.0 - maker_fee.maker) - hedge_ask * (1.0 + hedge_fee.taker);
  e.basis_mid = 0.5 * (maker_bid + maker_ask) - 0.5 * (hedge_bid + hedge_ask);
  e.ok = true;
  e.reason = "touch_fees_only_no_latency_no_queue";
  return e;
}

// Expected funding carry over `horizon_s` is an estimate, not a locked cashflow.
// Rates are the venue's fractional funding for the venue's funding period
// (Hyperliquid publishes an hourly rate on fundingHistory).
inline double funding_carry(double maker_rate_per_period, double hedge_rate_per_period,
                            double position_sign, double notional, double horizon_s,
                            double period_s) {
  if (!(period_s > 0)) return 0;
  const double frac = horizon_s / period_s;
  // Longs pay a positive funding rate. A long maker position pays maker_rate
  // and the short hedge receives hedge_rate (i.e. pays -hedge_rate).
  const double net_rate = maker_rate_per_period - hedge_rate_per_period;
  return -position_sign * net_rate * notional * frac;
}

struct SignalRow {
  int64_t t_ns = 0;
  double imbalance = 0;
  double micro_dev = 0;       // (micro - mid) / mid
  double trade_imb = 0;       // signed trade qty over the lookback, normalized
  double spread_bps = 0;
  double depth_imb = 0;       // top-5
  double basis_bps = 0;       // (maker_mid - hedge_mid) / hedge_mid * 1e4
  bool valid = false;
};

// Albers, Cucuringu, Howison, Shestopaloff (2021), arXiv:2108.09750, motivate
// these short-horizon book and trade features and show that fee-aware taker
// PnL is not the same object as return R^2. Spark computes the features and
// their association with subsequent mid changes. It does not claim their R^2
// and it does not label a feature profitable.

struct FeatureSample {
  SignalRow x;
  double mid = 0;
  // Filled in later, once the future mid is known. NaN until then.
  double ret_100ms = 0;
  double ret_500ms = 0;
  double ret_1s = 0;
  double ret_5s = 0;
  bool fut_100ms = false;
  bool fut_500ms = false;
  bool fut_1s = false;
  bool fut_5s = false;
};

inline double pearson(const std::vector<double>& x, const std::vector<double>& y) {
  if (x.size() != y.size() || x.size() < 3) return 0;
  const size_t n = x.size();
  double sx = 0, sy = 0;
  for (size_t i = 0; i < n; ++i) {
    sx += x[i];
    sy += y[i];
  }
  const double mx = sx / static_cast<double>(n);
  const double my = sy / static_cast<double>(n);
  double num = 0, dx = 0, dy = 0;
  for (size_t i = 0; i < n; ++i) {
    const double a = x[i] - mx;
    const double b = y[i] - my;
    num += a * b;
    dx += a * a;
    dy += b * b;
  }
  if (!(dx > 0) || !(dy > 0)) return 0;
  return num / std::sqrt(dx * dy);
}

}  // namespace spark
