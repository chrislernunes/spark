#pragma once

// Queue-position simulator.
//
// Hyperliquid does not publish per-order queue position, and its l2Book is an
// aggregated top-N snapshot. Lighter's public book publishes price levels
// (and, on the REST order endpoint, individual orders) but a hypothetical
// order that has not been acknowledged has no observed priority.
//
// Default model, conservative:
//   * On acknowledgement, queue_ahead = displayed quantity already at our price.
//     We join the back. We do not assume we are alone at an empty level unless
//     the displayed quantity is zero.
//   * Trades that print at our price, or that trade through it, reduce queue_ahead
//     and then our leaves.
//   * A decrease in displayed size that is larger than the trades just printed
//     at that price is attributed to cancellation. By default cancellations are
//     assumed to be BEHIND us, so they do not improve our queue. This biases
//     fill probability down. A research mode splits the decrease in proportion
//     to the fraction of the level that is ahead of us. That mode is not exact
//     and must not be described as a reconstruction.
//   * The order cannot fill before its acknowledgement time.

#include "spark/core.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace spark {

enum class QueueCancelMode : uint8_t { ConservativeBehind = 1, Proportional = 2 };

struct SimOrder {
  uint64_t id = 0;
  Side side = Side::Buy;
  int64_t px = 0;
  int64_t qty = 0;
  int64_t leaves = 0;
  int64_t ahead = 0;
  int64_t submit_ns = 0;
  int64_t ack_ns = 0;  // not eligible before this
  bool acknowledged = false;
  bool done = false;
  int64_t filled = 0;
  int64_t last_fill_ns = 0;
};

struct SimFill {
  uint64_t id = 0;
  Side side = Side::Buy;
  int64_t px = 0;
  int64_t qty = 0;
  int64_t ts_ns = 0;
  bool through = false;  // true if the trade price went through our limit
};

class QueueSimulator {
 public:
  explicit QueueSimulator(QueueCancelMode mode = QueueCancelMode::ConservativeBehind) : mode_(mode) {}

  QueueCancelMode mode() const noexcept { return mode_; }

  SimOrder& submit(Side side, int64_t px, int64_t qty, int64_t submit_ns, int64_t ack_latency_ns,
                   int64_t displayed_ahead) {
    SimOrder o;
    o.id = ++seq_;
    o.side = side;
    o.px = px;
    o.qty = qty;
    o.leaves = qty;
    o.ahead = std::max<int64_t>(0, displayed_ahead);
    o.submit_ns = submit_ns;
    o.ack_ns = submit_ns + std::max<int64_t>(0, ack_latency_ns);
    orders_.push_back(o);
    return orders_.back();
  }

  void acknowledge(uint64_t id, int64_t now_ns, int64_t displayed_ahead) {
    SimOrder* o = find(id);
    if (!o || o->done) return;
    o->acknowledged = true;
    o->ack_ns = std::max(o->ack_ns, now_ns);
    o->ahead = std::max<int64_t>(0, displayed_ahead);
  }

  // Trade print. `aggressor` is the taker side. A taker buy lifts the ask.
  std::vector<SimFill> on_trade(int64_t trade_px, int64_t trade_qty, Side aggressor, int64_t ts_ns) {
    std::vector<SimFill> fills;
    if (trade_qty <= 0) return fills;
    int64_t remaining_at_level = trade_qty;
    for (auto& o : orders_) {
      if (o.done || !o.acknowledged || ts_ns < o.ack_ns) continue;
      const bool at = trade_px == o.px;
      const bool through = (o.side == Side::Buy && aggressor == Side::Sell && trade_px < o.px) ||
                           (o.side == Side::Sell && aggressor == Side::Buy && trade_px > o.px);
      // A sell aggressor hits bids at trade_px. Our buy is eligible if trade_px <= our bid.
      const bool hits_us = (o.side == Side::Buy && aggressor == Side::Sell && trade_px <= o.px) ||
                           (o.side == Side::Sell && aggressor == Side::Buy && trade_px >= o.px);
      if (!hits_us) continue;
      int64_t consume = trade_qty;
      if (at) {
        // Only the residual not already assigned to earlier orders at this price
        // in this call. Orders are kept in acknowledgement order.
        consume = remaining_at_level;
      }
      if (consume <= 0) continue;
      if (o.ahead > 0 && at) {
        const int64_t eat = std::min(o.ahead, consume);
        o.ahead -= eat;
        consume -= eat;
        if (at) remaining_at_level -= eat;
      } else if (through) {
        // The trade printed strictly through us, so the whole level at our price
        // was consumed before this print. ahead is cleared.
        o.ahead = 0;
      }
      if (consume <= 0) continue;
      const int64_t fill_qty = std::min(o.leaves, consume);
      if (fill_qty <= 0) continue;
      o.leaves -= fill_qty;
      o.filled += fill_qty;
      o.last_fill_ns = ts_ns;
      if (at) remaining_at_level -= fill_qty;
      if (o.leaves == 0) o.done = true;
      fills.push_back(SimFill{o.id, o.side, o.px, fill_qty, ts_ns, through});
    }
    // Remember prints so a subsequent level-size update can separate cancels.
    last_trade_px_ = trade_px;
    last_trade_qty_ = trade_qty;
    last_trade_ns_ = ts_ns;
    return fills;
  }

  // Absolute displayed size at `px` after the trade at the same timestamp has
  // been applied. `traded_at_px` is the quantity already explained by prints.
  void on_displayed(Side side, int64_t px, int64_t new_qty, int64_t previous_qty, int64_t traded_at_px) {
    const int64_t explained = std::max<int64_t>(0, previous_qty - new_qty);
    int64_t cancel = explained - std::max<int64_t>(0, traded_at_px);
    if (cancel < 0) cancel = 0;
    for (auto& o : orders_) {
      if (o.done || !o.acknowledged || o.side != side || o.px != px) continue;
      if (cancel <= 0 || o.ahead <= 0) continue;
      if (mode_ == QueueCancelMode::ConservativeBehind) {
        // Cancellations do not improve our place.
        continue;
      }
      const int64_t level_before = std::max<int64_t>(previous_qty, 1);
      const double frac_ahead = std::min(1.0, static_cast<double>(o.ahead) / static_cast<double>(level_before));
      const int64_t improve = static_cast<int64_t>(std::floor(static_cast<double>(cancel) * frac_ahead));
      o.ahead = std::max<int64_t>(0, o.ahead - improve);
    }
  }

  const std::vector<SimOrder>& orders() const { return orders_; }
  SimOrder* find(uint64_t id) {
    for (auto& o : orders_)
      if (o.id == id) return &o;
    return nullptr;
  }

 private:
  QueueCancelMode mode_;
  std::vector<SimOrder> orders_;
  uint64_t seq_ = 0;
  int64_t last_trade_px_ = 0;
  int64_t last_trade_qty_ = 0;
  int64_t last_trade_ns_ = 0;
};

// Execution-quality sign convention (documented, used everywhere):
//   side_sign = +1 for a buy, -1 for a sell.
//   effective spread (fraction of mid) = 2 * side_sign * (fill - mid_fill) / mid_fill
//       A buyer who pays above mid has a positive effective spread (cost).
//   realized spread at horizon h = 2 * side_sign * (fill - mid_h) / mid_fill
//       Positive means the fill price was favorable relative to the later mid
//       (the maker captured spread). Negative means adverse selection.
//   adverse selection = side_sign * (mid_h - mid_fill) / mid_fill
//       Positive means the mid moved against the liquidity provider? 
//       For a buy, mid rising after the fill is favorable to the buyer, so
//       adverse selection for the BUYER is negative when mid rises.
//       Reports name the perspective: "signed_markout_buyer_positive_if_mid_rises".
//   implementation shortfall vs decision mid, quote currency:
//       side_sign * (fill - decision_mid) * qty_units + fee
struct ExecSample {
  Side side = Side::Buy;
  double fill_px = 0;
  double qty = 0;
  double decision_mid = 0;
  double submit_mid = 0;
  double ack_mid = 0;
  double fill_mid = 0;
  double mid_h = 0;  // later mid; 0 if unavailable
  bool has_horizon = false;
  double fee = 0;  // quote currency, positive is a cost
  Liquidity liq = Liquidity::Unknown;
};

struct ExecMetrics {
  bool ok = false;
  double effective_spread = 0;
  double realized_spread = 0;
  double adverse_selection = 0;
  double implementation_shortfall = 0;
  double slippage_vs_decision = 0;
};

inline ExecMetrics execution_metrics(const ExecSample& s) {
  ExecMetrics m;
  if (!(s.fill_px > 0) || !(s.fill_mid > 0) || !(s.qty > 0)) return m;
  const double sgn = side_sign(s.side);
  m.effective_spread = 2.0 * sgn * (s.fill_px - s.fill_mid) / s.fill_mid;
  m.slippage_vs_decision =
      (s.decision_mid > 0) ? sgn * (s.fill_px - s.decision_mid) / s.decision_mid : 0;
  m.implementation_shortfall =
      (s.decision_mid > 0) ? sgn * (s.fill_px - s.decision_mid) * s.qty + s.fee : 0;
  if (s.has_horizon && s.mid_h > 0) {
    m.realized_spread = 2.0 * sgn * (s.fill_px - s.mid_h) / s.fill_mid;
    m.adverse_selection = sgn * (s.mid_h - s.fill_mid) / s.fill_mid;
  }
  m.ok = true;
  return m;
}

struct LatencySummary {
  size_t n = 0;
  int64_t p50 = 0;
  int64_t p90 = 0;
  int64_t p95 = 0;
  int64_t p99 = 0;
  int64_t p999 = 0;
  int64_t max = 0;
  bool p999_valid = false;
};

inline LatencySummary summarize_ns(std::vector<int64_t> v) {
  LatencySummary s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.n = v.size();
  auto at = [&](double q) {
    const double idx = q * static_cast<double>(v.size() - 1);
    return v[static_cast<size_t>(idx)];
  };
  s.p50 = at(0.50);
  s.p90 = at(0.90);
  s.p95 = at(0.95);
  s.p99 = at(0.99);
  s.max = v.back();
  s.p999_valid = v.size() >= 1000;
  if (s.p999_valid) s.p999 = at(0.999);
  return s;
}

}  // namespace spark
