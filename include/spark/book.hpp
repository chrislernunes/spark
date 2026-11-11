#pragma once

#include "spark/core.hpp"

#include <functional>
#include <map>
#include <vector>

namespace spark {

struct Level {
  int64_t px = 0;
  int64_t qty = 0;
  int32_t n_orders = -1;  // -1: venue did not provide a count
};

enum class BookSync : uint8_t {
  Empty = 0,
  // Hyperliquid l2Book replaces the published top-N. Depth beyond N is unknown.
  LiveTruncated = 1,
  // Lighter incremental book with a verified nonce chain.
  LiveIncremental = 2,
  Gap = 3,
  Stale = 4,
};

inline const char* book_sync_name(BookSync s) noexcept {
  switch (s) {
    case BookSync::Empty: return "empty";
    case BookSync::LiveTruncated: return "live_truncated";
    case BookSync::LiveIncremental: return "live_incremental";
    case BookSync::Gap: return "gap";
    case BookSync::Stale: return "stale";
  }
  return "unknown";
}

// Bids stored descending, asks ascending.
// Hyperliquid: apply_snapshot REPLACES the book. It is not a delta.
// Lighter: apply_absolute sets the level size. qty == 0 deletes the level.
//   ASSUMPTION (documented, not a quoted official sentence): a published size of
//   zero removes the level. The official stream says updates after the snapshot
//   are state changes carrying absolute sizes.
class OrderBook {
 public:
  void clear() {
    bids_.clear();
    asks_.clear();
    sync_ = BookSync::Empty;
    exchange_ns_ = 0;
    last_nonce_ = 0;
    has_nonce_ = false;
    truncated_ = false;
  }

  void apply_snapshot(const std::vector<Level>& bids, const std::vector<Level>& asks,
                      int64_t exchange_ns, bool truncated) {
    bids_.clear();
    asks_.clear();
    for (const Level& l : bids) {
      if (l.qty > 0) bids_.emplace(l.px, l);
    }
    for (const Level& l : asks) {
      if (l.qty > 0) asks_.emplace(l.px, l);
    }
    exchange_ns_ = exchange_ns;
    truncated_ = truncated;
    sync_ = BookSync::LiveTruncated;
    has_nonce_ = false;
    last_nonce_ = 0;
  }

  // Returns false if the book was not live-incremental and this is not the
  // establishing snapshot (begin_nonce < 0 means "this message is the snapshot").
  bool apply_lighter_batch(int64_t begin_nonce, int64_t end_nonce, bool is_snapshot,
                           const std::vector<Level>& bid_changes,
                           const std::vector<Level>& ask_changes, int64_t exchange_ns) {
    if (!is_snapshot) {
      if (!has_nonce_ || sync_ == BookSync::Gap || sync_ == BookSync::Empty) {
        sync_ = BookSync::Gap;
        return false;
      }
      if (begin_nonce != last_nonce_) {
        sync_ = BookSync::Gap;
        return false;
      }
    }
    for (const Level& l : bid_changes) apply_absolute_unchecked(Side::Buy, l);
    for (const Level& l : ask_changes) apply_absolute_unchecked(Side::Sell, l);
    last_nonce_ = end_nonce;
    has_nonce_ = true;
    exchange_ns_ = exchange_ns;
    truncated_ = false;
    sync_ = BookSync::LiveIncremental;
    return true;
  }

  bool apply_absolute(Side side, Level level, int64_t exchange_ns) {
    if (sync_ == BookSync::Gap) return false;
    apply_absolute_unchecked(side, level);
    exchange_ns_ = exchange_ns;
    if (sync_ == BookSync::Empty) sync_ = BookSync::LiveIncremental;
    return true;
  }

  void mark_stale() { sync_ = BookSync::Stale; }

  BookSync sync() const noexcept { return sync_; }
  bool tradable() const noexcept {
    return sync_ == BookSync::LiveTruncated || sync_ == BookSync::LiveIncremental;
  }
  bool truncated() const noexcept { return truncated_; }
  int64_t exchange_ns() const noexcept { return exchange_ns_; }
  int64_t last_nonce() const noexcept { return last_nonce_; }
  bool has_nonce() const noexcept { return has_nonce_; }

  const Level* best_bid() const {
    if (bids_.empty()) return nullptr;
    return &bids_.begin()->second;
  }
  const Level* best_ask() const {
    if (asks_.empty()) return nullptr;
    return &asks_.begin()->second;
  }

  // Integer mid, half-tick rounds toward zero by integer division.
  // Returns false if the book is crossed or one-sided.
  bool mid_px(int64_t& out) const {
    const Level* b = best_bid();
    const Level* a = best_ask();
    if (!b || !a || a->px <= b->px) return false;
    out = b->px + (a->px - b->px) / 2;
    return true;
  }

  bool spread_px(int64_t& out) const {
    const Level* b = best_bid();
    const Level* a = best_ask();
    if (!b || !a || a->px <= b->px) return false;
    out = a->px - b->px;
    return true;
  }

  // Microprice = (ask * bid_qty + bid * ask_qty) / (bid_qty + ask_qty).
  bool microprice(int64_t& out) const {
    const Level* b = best_bid();
    const Level* a = best_ask();
    if (!b || !a || a->px <= b->px) return false;
    const __int128 denom = static_cast<__int128>(b->qty) + static_cast<__int128>(a->qty);
    if (denom <= 0) return false;
    const __int128 num = static_cast<__int128>(a->px) * b->qty + static_cast<__int128>(b->px) * a->qty;
    out = static_cast<int64_t>(num / denom);
    return true;
  }

  bool top_imbalance(double& out) const {
    const Level* b = best_bid();
    const Level* a = best_ask();
    if (!b || !a) return false;
    const double den = static_cast<double>(b->qty) + static_cast<double>(a->qty);
    if (!(den > 0)) return false;
    out = (static_cast<double>(b->qty) - static_cast<double>(a->qty)) / den;
    return true;
  }

  int64_t depth_qty(Side side, int levels) const {
    int64_t sum = 0;
    int n = 0;
    if (side == Side::Buy) {
      for (const auto& kv : bids_) {
        if (n++ >= levels) break;
        sum += kv.second.qty;
      }
    } else {
      for (const auto& kv : asks_) {
        if (n++ >= levels) break;
        sum += kv.second.qty;
      }
    }
    return sum;
  }

  int64_t qty_at(Side side, int64_t px) const {
    if (side == Side::Buy) {
      auto it = bids_.find(px);
      return it == bids_.end() ? 0 : it->second.qty;
    }
    auto it = asks_.find(px);
    return it == asks_.end() ? 0 : it->second.qty;
  }

  size_t bid_levels() const { return bids_.size(); }
  size_t ask_levels() const { return asks_.size(); }

  // Stable fingerprint for replay equality. Not a cryptographic hash.
  uint64_t fingerprint() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) {
      h ^= x;
      h *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(sync_));
    mix(static_cast<uint64_t>(exchange_ns_));
    mix(static_cast<uint64_t>(last_nonce_));
    for (const auto& kv : bids_) {
      mix(static_cast<uint64_t>(kv.second.px));
      mix(static_cast<uint64_t>(kv.second.qty));
      mix(static_cast<uint64_t>(kv.second.n_orders));
    }
    mix(0xB1D);
    for (const auto& kv : asks_) {
      mix(static_cast<uint64_t>(kv.second.px));
      mix(static_cast<uint64_t>(kv.second.qty));
      mix(static_cast<uint64_t>(kv.second.n_orders));
    }
    return h;
  }

  template <typename Fn>
  void for_each_bid(Fn fn) const {
    for (const auto& kv : bids_) fn(kv.second);
  }
  template <typename Fn>
  void for_each_ask(Fn fn) const {
    for (const auto& kv : asks_) fn(kv.second);
  }

 private:
  void apply_absolute_unchecked(Side side, Level level) {
    if (side == Side::Buy) {
      if (level.qty <= 0) bids_.erase(level.px);
      else bids_[level.px] = level;
    } else {
      if (level.qty <= 0) asks_.erase(level.px);
      else asks_[level.px] = level;
    }
  }

  std::map<int64_t, Level, std::greater<int64_t>> bids_;
  std::map<int64_t, Level> asks_;
  BookSync sync_ = BookSync::Empty;
  int64_t exchange_ns_ = 0;
  int64_t last_nonce_ = 0;
  bool has_nonce_ = false;
  bool truncated_ = false;
};

}  // namespace spark
