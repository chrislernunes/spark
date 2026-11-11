#include "spark/book.hpp"
#include "spark/execution.hpp"
#include "spark/journal.hpp"
#include "spark/reactor.hpp"
#include "spark/sim.hpp"
#include "spark/strategy.hpp"
#include "spark/venues.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>

#include <sys/socket.h>

static int g_failed = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++g_failed;                                                                   \
    }                                                                               \
  } while (0)

static void test_decimal() {
  int64_t v = 0;
  CHECK(spark::parse_scaled("73920.0", 8, v));
  CHECK(v == 73920LL * 100000000LL);
  CHECK(spark::parse_scaled("2.21367", 8, v));
  CHECK(v == 221367000LL);
  CHECK(spark::parse_scaled("-0.5", 1, v));
  CHECK(v == -5);
  CHECK(!spark::parse_scaled("1.234", 2, v));
  CHECK(spark::parse_scaled("1.2300", 2, v));
  CHECK(v == 123);
  CHECK(!spark::parse_scaled("1e2", 2, v));
  CHECK(spark::format_scaled(7392000000000LL, 8) == "73920.00000000");
}

static void test_book_and_hl() {
  std::string raw;
  CHECK(spark::load_file("tests/fixtures/hl_l2_btc.json", raw) ||
        spark::load_file("spark/tests/fixtures/hl_l2_btc.json", raw));
  spark::Json j;
  std::string err;
  CHECK(spark::parse_json(raw, j, err));
  spark::ParsedBook pb;
  CHECK(spark::parse_hl_l2(j, pb));
  CHECK(pb.truncated);
  CHECK(pb.bids.size() == 20);
  CHECK(pb.asks.size() == 20);
  CHECK(pb.exchange_ns == pb.exchange_ms * 1000000LL);
  CHECK(pb.bids.front().px > pb.bids.back().px);
  spark::OrderBook book;
  book.apply_snapshot(pb.bids, pb.asks, pb.exchange_ns, true);
  CHECK(book.sync() == spark::BookSync::LiveTruncated);
  CHECK(book.best_bid()->px < book.best_ask()->px);
  int64_t mid = 0, micro = 0, spr = 0;
  CHECK(book.mid_px(mid));
  CHECK(book.microprice(micro));
  CHECK(book.spread_px(spr));
  CHECK(spr > 0);
  double imb = 0;
  CHECK(book.top_imbalance(imb));
  CHECK(imb <= 1 && imb >= -1);
  // Absolute delete and replace semantics on a fresh incremental book.
  spark::OrderBook inc;
  constexpr int64_t U = 100000000LL;
  inc.apply_absolute(spark::Side::Buy, spark::Level{100 * U, 5 * U, 2}, 1000);
  inc.apply_absolute(spark::Side::Sell, spark::Level{101 * U, 4 * U, 1}, 1000);
  CHECK(inc.qty_at(spark::Side::Buy, 100 * U) == 5 * U);
  inc.apply_absolute(spark::Side::Buy, spark::Level{100 * U, 0, 0}, 1001);
  CHECK(inc.qty_at(spark::Side::Buy, 100 * U) == 0);
  CHECK(inc.best_bid() == nullptr);
  // Snapshot replaces; it does not merge with previous depth.
  std::vector<spark::Level> nb{{99 * U, 1 * U, 1}};
  std::vector<spark::Level> na{{102 * U, 1 * U, 1}};
  inc.apply_snapshot(nb, na, 2000, true);
  CHECK(inc.qty_at(spark::Side::Sell, 101 * U) == 0);
  CHECK(inc.best_ask()->px == 102 * U);
}

static void test_lighter_nonce() {
  const char* snap = R"({
    "channel":"order_book:1",
    "timestamp":1774884082326,
    "type":"subscribed/order_book",
    "order_book":{
      "asks":[{"price":"2064.54","size":"0.3285"}],
      "bids":[{"price":"2064.30","size":"1.0392"}],
      "nonce":100,
      "begin_nonce":90,
      "last_updated_at":1774884082309144
    }
  })";
  spark::Json j;
  std::string err;
  CHECK(spark::parse_json(snap, j, err));
  spark::ParsedBook pb;
  CHECK(spark::parse_lighter_book_msg(j, pb));
  CHECK(pb.is_snapshot);
  CHECK(pb.nonce == 100);
  CHECK(pb.exchange_ns == 1774884082309144LL * 1000LL);
  spark::OrderBook book;
  CHECK(book.apply_lighter_batch(pb.begin_nonce, pb.nonce, true, pb.bids, pb.asks, pb.exchange_ns));
  CHECK(book.sync() == spark::BookSync::LiveIncremental);
  const char* upd = R"({
    "type":"update/order_book",
    "timestamp":1774884082400,
    "order_book":{
      "asks":[{"price":"2064.54","size":"0"}],
      "bids":[{"price":"2064.30","size":"2.0000"}],
      "nonce":110,
      "begin_nonce":100,
      "last_updated_at":1774884082400000
    }
  })";
  CHECK(spark::parse_json(upd, j, err));
  CHECK(spark::parse_lighter_book_msg(j, pb));
  CHECK(book.apply_lighter_batch(pb.begin_nonce, pb.nonce, false, pb.bids, pb.asks, pb.exchange_ns));
  CHECK(book.qty_at(spark::Side::Sell, book.best_ask() ? book.best_ask()->px : 0) == 0 ||
        book.best_ask() == nullptr);
  CHECK(book.best_ask() == nullptr);
  // Gap: begin_nonce does not match.
  const char* gap = R"({
    "type":"update/order_book",
    "order_book":{
      "asks":[{"price":"2065.00","size":"1"}],
      "bids":[],
      "nonce":200,
      "begin_nonce":150
    }
  })";
  CHECK(spark::parse_json(gap, j, err));
  CHECK(spark::parse_lighter_book_msg(j, pb));
  CHECK(!book.apply_lighter_batch(pb.begin_nonce, pb.nonce, false, pb.bids, pb.asks, 1));
  CHECK(book.sync() == spark::BookSync::Gap);
  CHECK(!book.tradable());
}

static void test_journal_replay() {
  const char* path = "/tmp/spark_journal_test.bin";
  std::remove(path);
  spark::JournalWriter w(path, 1024);
  constexpr int64_t U = 100000000LL;
  std::vector<spark::Level> bids{{100 * U, 2 * U, 1}, {99 * U, 3 * U, 2}};
  std::vector<spark::Level> asks{{101 * U, 4 * U, 3}};
  auto payload = spark::encode_book_snapshot(bids, asks, true);
  CHECK(w.append(spark::EventType::BookSnapshot, 1000, 2000, 3000, payload.data(),
                 static_cast<uint32_t>(payload.size())));
  bids[0].qty = 5 * U;
  payload = spark::encode_book_snapshot(bids, asks, true);
  CHECK(w.append(spark::EventType::BookSnapshot, 4000, 5000, 6000, payload.data(),
                 static_cast<uint32_t>(payload.size())));
  w.flush();
  std::vector<spark::JournalRecord> recs;
  std::string err;
  CHECK(spark::read_journal(path, recs, err));
  CHECK(recs.size() == 2);
  CHECK(recs[0].header.exchange_ns == 1000);
  CHECK(recs[0].header.recv_ns == 2000);
  CHECK(recs[0].header.process_ns == 3000);
  CHECK(recs[0].header.exchange_ns != recs[0].header.recv_ns);
  spark::OrderBook live, replay;
  uint64_t live_fp = 0;
  for (const auto& r : recs) {
    std::vector<spark::Level> b, a;
    bool trunc = false;
    CHECK(spark::decode_book_snapshot(r.payload.data(), r.payload.size(), b, a, trunc));
    live.apply_snapshot(b, a, r.header.exchange_ns, trunc);
    live_fp = live.fingerprint();
  }
  // Torn tail must not invent a record.
  {
    std::ofstream extra(path, std::ios::binary | std::ios::app);
    const char junk[] = "TORN";
    extra.write(junk, 4);
  }
  recs.clear();
  CHECK(spark::read_journal(path, recs, err));
  CHECK(recs.size() == 2);
  for (const auto& r : recs) {
    std::vector<spark::Level> b, a;
    bool trunc = false;
    CHECK(spark::decode_book_snapshot(r.payload.data(), r.payload.size(), b, a, trunc));
    replay.apply_snapshot(b, a, r.header.exchange_ns, trunc);
  }
  CHECK(replay.fingerprint() == live_fp);
  CHECK(replay.best_bid()->qty == 5 * U);
}

static void test_fsm_and_risk() {
  spark::OrderFsm fsm;
  fsm.set_qty(10);
  CHECK(fsm.submit().ok);
  CHECK(!fsm.submit().ok);
  CHECK(fsm.ack_resting().ok);
  CHECK(fsm.state() == spark::OrderState::Live);
  CHECK(fsm.duplicate_ack().ok);
  CHECK(fsm.fill(4).ok);
  CHECK(fsm.state() == spark::OrderState::PartiallyFilled);
  CHECK(!fsm.fill(100).ok);
  CHECK(fsm.request_cancel().ok);
  CHECK(fsm.fill(6).ok);
  CHECK(fsm.state() == spark::OrderState::Filled);
  CHECK(fsm.leaves_qty() == 0);

  spark::OrderFsm unk;
  unk.set_qty(5);
  CHECK(unk.submit().ok);
  CHECK(unk.on_disconnect().ok);
  CHECK(unk.state() == spark::OrderState::Unknown);
  CHECK(!unk.reconcile_absent(false).ok);
  CHECK(unk.state() == spark::OrderState::Unknown);
  CHECK(unk.reconcile_absent(true).ok);
  CHECK(unk.state() == spark::OrderState::Canceled);

  spark::OrderManager om;
  auto& o = om.create(spark::Venue::Hyperliquid, spark::Side::Buy, 1, 3);
  CHECK(o.fsm.submit().ok);
  CHECK(om.mark_disconnect() == 1);
  CHECK(om.orders()[0].fsm.state() == spark::OrderState::Unknown);

  spark::RiskLimits lim;
  lim.max_order_lots = 10;
  lim.max_position_lots = 10;
  lim.max_outstanding = 2;
  lim.max_orders_per_sec = 2;
  lim.max_feed_age_ns = 1000000000LL;
  lim.max_daily_loss = 100;
  lim.live_authorized = false;
  spark::RiskEngine risk(lim);
  spark::OrderBook book;
  constexpr int64_t U = 100000000LL;
  book.apply_snapshot({{100 * U, U, 1}}, {{101 * U, U, 1}}, 10000000000LL, true);
  spark::Exposure ex;
  spark::OrderIntent intent;
  intent.qty = 5;
  intent.live = true;
  CHECK(risk.check(intent, ex, book, 10500000000LL) == spark::RiskCode::LiveNotAuthorized);
  intent.live = false;
  CHECK(risk.check(intent, ex, book, 10500000000LL) == spark::RiskCode::Ok);
  intent.qty = 11;
  CHECK(risk.check(intent, ex, book, 10500000000LL) == spark::RiskCode::OrderTooLarge);
  intent.qty = 5;
  ex.realized_pnl = -100;
  CHECK(risk.check(intent, ex, book, 10500000000LL) == spark::RiskCode::DailyLoss);
  ex.realized_pnl = 0;
  CHECK(risk.check(intent, ex, book, 10000000000LL + 5000000000LL) == spark::RiskCode::StaleFeed);
  risk.kill();
  CHECK(risk.check(intent, ex, book, 10500000000LL) == spark::RiskCode::KillSwitch);
}

static void test_glft_and_cross() {
  spark::GlftParams p;
  p.gamma = 0.1;
  p.xi = 0.1;
  p.A = 1.0;
  p.k = 1.5;
  p.sigma = 0.1;
  p.delta_t = 1;
  p.calibrated = true;
  auto q = spark::glft_quotes(100.0, 0.0, p);
  CHECK(q.ok);
  CHECK(std::fabs(q.reservation - 100.0) < 1e-9);
  CHECK(q.ask > 100 && q.bid < 100);
  CHECK(std::fabs((q.ask - q.bid) / 2.0 - q.half_spread) < 1e-9);
  auto q_long = spark::glft_quotes(100.0, 2.0, p);
  CHECK(q_long.ok);
  CHECK(q_long.reservation < q.reservation);
  CHECK(q_long.bid < q.bid);
  // Locked vector from the formula, not a guessed target.
  const double xi_dt = 0.1;
  const double base = 1.0 + xi_dt / 1.5;
  const double c1 = (1.0 / xi_dt) * std::log(base);
  const double c2 = std::sqrt((0.1 / (2.0 * 1.0 * 1.0 * 1.5)) * std::pow(base, (1.5 / xi_dt) + 1.0));
  CHECK(std::fabs(q.c1 - c1) < 1e-12);
  CHECK(std::fabs(q.c2 - c2) < 1e-12);

  std::vector<spark::IntensityObs> obs;
  for (int i = 0; i < 40; ++i) {
    obs.push_back({0.2, 1.0, true});
    obs.push_back({2.0, 1.0, false});
  }
  auto fit = spark::fit_intensity(obs, 30);
  CHECK(fit.calibrated);
  CHECK(fit.A > 0 && fit.k > 0);
  auto thin = spark::fit_intensity(std::vector<spark::IntensityObs>{{0.1, 1.0, true}}, 30);
  CHECK(!thin.calibrated);

  spark::FeeSchedule maker{0.0, 0.0, true};
  spark::FeeSchedule hedge{0.00015, 0.00045, true};
  auto edge = spark::cross_edge(100.0, 100.2, 100.5, 100.7, maker, hedge);
  CHECK(edge.ok);
  // Buy 100 on maker, sell 100.5 on hedge after 4.5 bps.
  const double expect = 100.5 * (1.0 - 0.00045) - 100.0;
  CHECK(std::fabs(edge.buy_maker_sell_hedge - expect) < 1e-9);
  CHECK(edge.buy_maker_sell_hedge > 0);
}

static void test_queue_and_exec() {
  spark::QueueSimulator sim(spark::QueueCancelMode::ConservativeBehind);
  constexpr int64_t U = 100000000LL;
  auto& o = sim.submit(spark::Side::Buy, 100 * U, 2 * U, 1000, 50, 5 * U);
  CHECK(!o.acknowledged);
  // Trade before ack does not fill.
  auto early = sim.on_trade(100 * U, 10 * U, spark::Side::Sell, 1020);
  CHECK(early.empty());
  sim.acknowledge(o.id, 1050, 5 * U);
  // A displayed-size drop with no trade must not move us forward.
  sim.on_displayed(spark::Side::Buy, 100 * U, 1 * U, 5 * U, 0);
  CHECK(sim.find(o.id)->ahead == 5 * U);
  // 5 ahead, then our 2. A sell of 6 at our price fills 1 of us.
  auto fills = sim.on_trade(100 * U, 6 * U, spark::Side::Sell, 1100);
  CHECK(fills.size() == 1);
  CHECK(fills[0].qty == 1 * U);
  CHECK(sim.find(o.id)->leaves == 1 * U);
  // Displayed size drop without a trade does not improve the queue.
  sim.on_displayed(spark::Side::Buy, 100 * U, 1 * U, 10 * U, 0);
  CHECK(sim.find(o.id)->ahead == 0);
  // A through-trade clears the remainder.
  fills = sim.on_trade(99 * U, 1 * U, spark::Side::Sell, 1200);
  CHECK(fills.size() == 1);
  CHECK(sim.find(o.id)->done);

  spark::QueueSimulator prop(spark::QueueCancelMode::Proportional);
  auto& p = prop.submit(spark::Side::Buy, 50 * U, U, 0, 0, 10 * U);
  prop.acknowledge(p.id, 0, 10 * U);
  prop.on_displayed(spark::Side::Buy, 50 * U, 0, 10 * U, 0);
  CHECK(prop.find(p.id)->ahead < 10 * U);

  spark::ExecSample s;
  s.side = spark::Side::Buy;
  s.fill_px = 101;
  s.qty = 2;
  s.decision_mid = 100;
  s.fill_mid = 100;
  s.mid_h = 99;
  s.has_horizon = true;
  s.fee = 0.01;
  auto m = spark::execution_metrics(s);
  CHECK(m.ok);
  CHECK(std::fabs(m.effective_spread - 2.0 * (101.0 - 100.0) / 100.0) < 1e-12);
  // Buyer, mid fell: adverse selection is negative (mid moved against a long? 
  // signed markout buyer-positive-if-mid-rises: mid fell, so negative).
  CHECK(m.adverse_selection < 0);
  CHECK(m.implementation_shortfall > 0);

  std::vector<int64_t> lat(1000);
  for (int i = 0; i < 1000; ++i) lat[i] = i;
  auto sum = spark::summarize_ns(lat);
  CHECK(sum.n == 1000);
  CHECK(sum.p999_valid);
  CHECK(sum.max == 999);
}

static void test_queue_spsc_and_reactor() {
  spark::SpscQueue<int, 8> q;
  int v = 0;
  CHECK(!q.pop(v));
  for (int i = 0; i < 7; ++i) CHECK(q.push(i));
  CHECK(!q.push(99));
  CHECK(q.pop(v) && v == 0);
  CHECK(q.push(7));
  spark::MpmcQueue<int> mq(2, spark::OverflowPolicy::Reject);
  CHECK(mq.push(1));
  CHECK(mq.push(2));
  CHECK(!mq.push(3));
  spark::MpmcQueue<int> drop(1, spark::OverflowPolicy::DropOldest);
  CHECK(drop.push(1));
  CHECK(drop.push(2));
  CHECK(drop.pop(v) && v == 2);

  spark::Reactor reactor;
  CHECK(reactor.ok());
  int fds[2] = {-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  bool got = false;
  CHECK(reactor.add(fds[0], EPOLLIN, [&](uint32_t) {
    char buf[8];
    ssize_t n = ::read(fds[0], buf, sizeof(buf));
    got = n == 1 && buf[0] == 'x';
  }));
  CHECK(::write(fds[1], "x", 1) == 1);
  CHECK(reactor.poll(200) >= 1);
  CHECK(got);
  ::close(fds[0]);
  ::close(fds[1]);
}

static void test_live_lighter_capture() {
  std::string raw;
  if (!spark::load_file("data/samples/live/lighter.jsonl", raw)) {
    std::fprintf(stderr, "FAIL missing lighter capture\n");
    ++g_failed;
    return;
  }
  spark::OrderBook book;
  int applied = 0;
  int zero_deletes = 0;
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t nl = raw.find('\n', pos);
    if (nl == std::string::npos) nl = raw.size();
    std::string_view line(raw.data() + pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    spark::Json wrap;
    std::string err;
    if (!spark::parse_json(line, wrap, err)) {
      CHECK(false);
      return;
    }
    const spark::Json* payload = wrap.get("payload");
    if (!payload || !payload->get("order_book")) continue;
    spark::ParsedBook pb;
    CHECK(spark::parse_lighter_book_msg(*payload, pb));
    if (!pb.ok) return;
    for (const auto& l : pb.bids)
      if (l.qty == 0) ++zero_deletes;
    for (const auto& l : pb.asks)
      if (l.qty == 0) ++zero_deletes;
    bool ok = book.apply_lighter_batch(pb.begin_nonce, pb.nonce, pb.is_snapshot, pb.bids, pb.asks,
                                       pb.exchange_ns);
    CHECK(ok);
    if (!ok) return;
    ++applied;
  }
  CHECK(applied > 50);
  CHECK(zero_deletes > 0);
  CHECK(book.tradable());
  CHECK(book.sync() == spark::BookSync::LiveIncremental);
  CHECK(book.bid_levels() + book.ask_levels() > 100);
  int64_t mid = 0;
  CHECK(book.mid_px(mid));
  CHECK(mid > 0);
}

static void test_hl_trade_fixture() {
  std::string raw;
  CHECK(spark::load_file("tests/fixtures/hl_trades_btc.json", raw) ||
        spark::load_file("spark/tests/fixtures/hl_trades_btc.json", raw));
  spark::Json j;
  std::string err;
  CHECK(spark::parse_json(raw, j, err));
  CHECK(j.type == spark::Json::Type::Array);
  CHECK(!j.arr.empty());
  spark::ParsedTrade t;
  CHECK(spark::parse_hl_trade(j.arr[0], t));
  CHECK(t.side_known);
  CHECK(t.px > 0 && t.qty > 0);
  CHECK(t.exchange_ns > 0);
}

int main() {
  test_decimal();
  test_book_and_hl();
  test_lighter_nonce();
  test_journal_replay();
  test_fsm_and_risk();
  test_glft_and_cross();
  test_queue_and_exec();
  test_queue_spsc_and_reactor();
  test_hl_trade_fixture();
  test_live_lighter_capture();
  if (g_failed) {
    std::fprintf(stderr, "%d checks failed\n", g_failed);
    return 1;
  }
  std::puts("spark tests passed");
  return 0;
}
