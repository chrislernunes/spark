#pragma once

#include "spark/book.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace spark {

enum class OrderState : uint8_t {
  Created = 1,
  PendingSubmit = 2,
  Live = 3,
  PartiallyFilled = 4,
  PendingCancel = 5,
  PendingModify = 6,
  Filled = 7,
  Canceled = 8,
  Rejected = 9,
  Unknown = 10,
};

inline const char* order_state_name(OrderState s) noexcept {
  switch (s) {
    case OrderState::Created: return "created";
    case OrderState::PendingSubmit: return "pending_submit";
    case OrderState::Live: return "live";
    case OrderState::PartiallyFilled: return "partially_filled";
    case OrderState::PendingCancel: return "pending_cancel";
    case OrderState::PendingModify: return "pending_modify";
    case OrderState::Filled: return "filled";
    case OrderState::Canceled: return "canceled";
    case OrderState::Rejected: return "rejected";
    case OrderState::Unknown: return "unknown";
  }
  return "invalid";
}

inline bool is_terminal(OrderState s) noexcept {
  return s == OrderState::Filled || s == OrderState::Canceled || s == OrderState::Rejected;
}

struct Transition {
  bool ok = false;
  OrderState state = OrderState::Created;
  const char* reason = "";
};

// Explicit order lifecycle. Transmission is not acceptance.
class OrderFsm {
 public:
  OrderState state() const noexcept { return state_; }
  int64_t cum_qty() const noexcept { return cum_; }
  int64_t leaves_qty() const noexcept { return leaves_; }

  void set_qty(int64_t qty) { qty_ = qty; leaves_ = qty; }

  Transition submit() { return move(OrderState::Created, OrderState::PendingSubmit, "submit"); }

  Transition ack_resting() {
    if (state_ != OrderState::PendingSubmit && state_ != OrderState::Unknown) {
      return bad("ack_resting from " + std::string(order_state_name(state_)));
    }
    state_ = (cum_ > 0 && leaves_ > 0) ? OrderState::PartiallyFilled : OrderState::Live;
    return ok_now("ack_resting");
  }

  Transition reject() {
    if (state_ != OrderState::PendingSubmit && state_ != OrderState::Unknown &&
        state_ != OrderState::PendingModify) {
      return bad("reject from " + std::string(order_state_name(state_)));
    }
    state_ = OrderState::Rejected;
    leaves_ = 0;
    return ok_now("reject");
  }

  // Duplicate ack is ignored, not an error, once the order is already working.
  Transition duplicate_ack() {
    if (state_ == OrderState::Live || state_ == OrderState::PartiallyFilled ||
        state_ == OrderState::PendingCancel) {
      return ok_now("duplicate_ack_ignored");
    }
    return bad("duplicate_ack");
  }

  Transition fill(int64_t qty) {
    if (qty <= 0) return bad("non_positive_fill");
    if (state_ != OrderState::Live && state_ != OrderState::PartiallyFilled &&
        state_ != OrderState::PendingCancel && state_ != OrderState::PendingSubmit &&
        state_ != OrderState::Unknown && state_ != OrderState::PendingModify) {
      return bad("fill from " + std::string(order_state_name(state_)));
    }
    if (qty > leaves_) return bad("fill exceeds leaves");
    cum_ += qty;
    leaves_ -= qty;
    if (leaves_ == 0) state_ = OrderState::Filled;
    else if (state_ != OrderState::PendingCancel && state_ != OrderState::Unknown)
      state_ = OrderState::PartiallyFilled;
    return ok_now("fill");
  }

  Transition request_cancel() {
    if (state_ != OrderState::Live && state_ != OrderState::PartiallyFilled &&
        state_ != OrderState::PendingSubmit) {
      return bad("cancel from " + std::string(order_state_name(state_)));
    }
    state_ = OrderState::PendingCancel;
    return ok_now("request_cancel");
  }

  Transition canceled() {
    if (state_ != OrderState::PendingCancel && state_ != OrderState::Live &&
        state_ != OrderState::PartiallyFilled && state_ != OrderState::Unknown &&
        state_ != OrderState::PendingSubmit) {
      return bad("canceled from " + std::string(order_state_name(state_)));
    }
    state_ = OrderState::Canceled;
    leaves_ = 0;
    return ok_now("canceled");
  }

  // Connection loss while an acknowledgement is outstanding.
  Transition on_disconnect() {
    if (state_ == OrderState::PendingSubmit || state_ == OrderState::PendingCancel ||
        state_ == OrderState::PendingModify) {
      state_ = OrderState::Unknown;
      return ok_now("disconnect_unknown");
    }
    return ok_now("disconnect_noop");
  }

  // Exchange open-order snapshot contains the order.
  Transition reconcile_live() {
    if (state_ != OrderState::Unknown && state_ != OrderState::PendingSubmit) return bad("reconcile_live");
    state_ = leaves_ < qty_ && leaves_ > 0 ? OrderState::PartiallyFilled : OrderState::Live;
    return ok_now("reconcile_live");
  }

  // Fills stream is known complete and the order is gone.
  Transition reconcile_absent(bool fills_stream_healthy) {
    if (state_ != OrderState::Unknown) return bad("reconcile_absent");
    if (!fills_stream_healthy) return bad("fills_stream_gapped_stay_unknown");
    state_ = cum_ == qty_ && qty_ > 0 ? OrderState::Filled : OrderState::Canceled;
    leaves_ = 0;
    return ok_now("reconcile_absent");
  }

 private:
  Transition move(OrderState from, OrderState to, const char* why) {
    if (state_ != from) return bad(std::string(why) + " from " + order_state_name(state_));
    state_ = to;
    return ok_now(why);
  }
  Transition ok_now(const char* why) {
    Transition t;
    t.ok = true;
    t.state = state_;
    t.reason = why;
    return t;
  }
  Transition bad(std::string why) {
    last_error_ = std::move(why);
    Transition t;
    t.ok = false;
    t.state = state_;
    t.reason = last_error_.c_str();
    return t;
  }

  OrderState state_ = OrderState::Created;
  int64_t qty_ = 0;
  int64_t cum_ = 0;
  int64_t leaves_ = 0;
  std::string last_error_;
};

enum class RiskCode : uint8_t {
  Ok = 0,
  LiveNotAuthorized = 1,
  KillSwitch = 2,
  OrderTooLarge = 3,
  Position = 4,
  Gross = 5,
  Net = 6,
  Outstanding = 7,
  DailyLoss = 8,
  Drawdown = 9,
  Rate = 10,
  StaleFeed = 11,
  HedgeDelay = 12,
  Unhedged = 13,
  BadBook = 14,
  StrategyHalted = 15,
};

inline const char* risk_name(RiskCode c) noexcept {
  switch (c) {
    case RiskCode::Ok: return "ok";
    case RiskCode::LiveNotAuthorized: return "live_not_authorized";
    case RiskCode::KillSwitch: return "kill_switch";
    case RiskCode::OrderTooLarge: return "order_too_large";
    case RiskCode::Position: return "position";
    case RiskCode::Gross: return "gross";
    case RiskCode::Net: return "net";
    case RiskCode::Outstanding: return "outstanding";
    case RiskCode::DailyLoss: return "daily_loss";
    case RiskCode::Drawdown: return "drawdown";
    case RiskCode::Rate: return "rate";
    case RiskCode::StaleFeed: return "stale_feed";
    case RiskCode::HedgeDelay: return "hedge_delay";
    case RiskCode::Unhedged: return "unhedged";
    case RiskCode::BadBook: return "bad_book";
    case RiskCode::StrategyHalted: return "strategy_halted";
  }
  return "unknown";
}

struct RiskLimits {
  int64_t max_position_lots = 0;
  int64_t max_order_lots = 0;
  int64_t max_gross_lots = 0;
  int64_t max_net_lots = 0;
  int max_outstanding = 0;
  int64_t max_daily_loss = 0;     // positive number; loss is negative pnl
  int64_t max_drawdown = 0;
  int max_orders_per_sec = 0;
  int64_t max_feed_age_ns = 0;
  int64_t max_clock_skew_ns = 2000000000LL;  // exchange clock ahead of local
  int64_t max_hedge_delay_ns = 0;
  int64_t max_unhedged_lots = 0;
  bool live_authorized = false;  // default refuses anything that would leave the process
};

struct Exposure {
  int64_t position_lots = 0;
  int64_t gross_lots = 0;
  int64_t net_lots = 0;
  int outstanding = 0;
  int64_t realized_pnl = 0;  // scaled quote currency; negative is a loss
  int64_t peak_pnl = 0;
  int64_t unhedged_lots = 0;
  int64_t hedge_pending_ns = 0;  // age of oldest unhedged fill; 0 if flat hedge
};

struct OrderIntent {
  Venue venue = Venue::Unknown;
  Side side = Side::Buy;
  int64_t qty = 0;
  bool live = false;  // true means the request would be sent to an exchange
  bool hedge = false;
};

class RiskEngine {
 public:
  explicit RiskEngine(RiskLimits limits) : limits_(limits) {}

  const RiskLimits& limits() const noexcept { return limits_; }
  bool killed() const noexcept { return killed_; }
  void kill() { killed_ = true; }
  void reset_kill() { killed_ = false; }
  void halt_strategy() { halted_ = true; }
  void resume_strategy() { halted_ = false; }

  void note_order(int64_t now_ns) {
    while (!stamps_.empty() && now_ns - stamps_.front() > 1000000000LL) stamps_.erase(stamps_.begin());
    stamps_.push_back(now_ns);
  }

  RiskCode check(const OrderIntent& o, const Exposure& e, const OrderBook& book, int64_t now_ns) const {
    if (killed_) return RiskCode::KillSwitch;
    if (halted_) return RiskCode::StrategyHalted;
    if (o.live && !limits_.live_authorized) return RiskCode::LiveNotAuthorized;
    if (o.qty <= 0) return RiskCode::OrderTooLarge;
    if (limits_.max_order_lots > 0 && o.qty > limits_.max_order_lots) return RiskCode::OrderTooLarge;
    const int64_t next_pos = e.position_lots + (o.side == Side::Buy ? o.qty : -o.qty);
    if (limits_.max_position_lots > 0 && std::llabs(next_pos) > limits_.max_position_lots)
      return RiskCode::Position;
    if (limits_.max_net_lots > 0 && std::llabs(e.net_lots) > limits_.max_net_lots) return RiskCode::Net;
    if (limits_.max_gross_lots > 0 && e.gross_lots > limits_.max_gross_lots) return RiskCode::Gross;
    if (limits_.max_outstanding > 0 && e.outstanding >= limits_.max_outstanding) return RiskCode::Outstanding;
    if (limits_.max_daily_loss > 0 && e.realized_pnl <= -limits_.max_daily_loss) return RiskCode::DailyLoss;
    const int64_t dd = e.peak_pnl - e.realized_pnl;
    if (limits_.max_drawdown > 0 && dd >= limits_.max_drawdown) return RiskCode::Drawdown;
    if (limits_.max_unhedged_lots > 0 && std::llabs(e.unhedged_lots) > limits_.max_unhedged_lots && !o.hedge)
      return RiskCode::Unhedged;
    if (o.hedge && limits_.max_hedge_delay_ns > 0 && e.hedge_pending_ns > limits_.max_hedge_delay_ns)
      return RiskCode::HedgeDelay;
    if (!book.tradable()) return RiskCode::BadBook;
    if (book.exchange_ns() == 0 || now_ns == 0) return RiskCode::StaleFeed;
    const int64_t age = now_ns - book.exchange_ns();
    if (age < -limits_.max_clock_skew_ns) return RiskCode::StaleFeed;
    if (limits_.max_feed_age_ns > 0 && age > limits_.max_feed_age_ns) return RiskCode::StaleFeed;
    if (limits_.max_orders_per_sec > 0) {
      int n = 0;
      for (int64_t t : stamps_)
        if (now_ns - t <= 1000000000LL) ++n;
      if (n >= limits_.max_orders_per_sec) return RiskCode::Rate;
    }
    return RiskCode::Ok;
  }

 private:
  RiskLimits limits_;
  bool killed_ = false;
  bool halted_ = false;
  std::vector<int64_t> stamps_;
};

struct ManagedOrder {
  uint64_t client_id = 0;
  std::string venue_id;
  Venue venue = Venue::Unknown;
  Side side = Side::Buy;
  int64_t px = 0;
  OrderFsm fsm;
};

// Tracks orders. Never treats a transmitted request as accepted.
class OrderManager {
 public:
  uint64_t next_id() { return ++seq_; }

  ManagedOrder& create(Venue venue, Side side, int64_t px, int64_t qty) {
    ManagedOrder o;
    o.client_id = next_id();
    o.venue = venue;
    o.side = side;
    o.px = px;
    o.fsm.set_qty(qty);
    orders_.push_back(std::move(o));
    return orders_.back();
  }

  ManagedOrder* find(uint64_t id) {
    for (auto& o : orders_)
      if (o.client_id == id) return &o;
    return nullptr;
  }

  int outstanding() const {
    int n = 0;
    for (const auto& o : orders_)
      if (!is_terminal(o.fsm.state())) ++n;
    return n;
  }

  // After a disconnect, in-flight requests become Unknown.
  int mark_disconnect() {
    int n = 0;
    for (auto& o : orders_) {
      auto t = o.fsm.on_disconnect();
      if (t.ok && o.fsm.state() == OrderState::Unknown) ++n;
    }
    return n;
  }

  const std::vector<ManagedOrder>& orders() const { return orders_; }

 private:
  uint64_t seq_ = 0;
  std::vector<ManagedOrder> orders_;
};

}  // namespace spark
