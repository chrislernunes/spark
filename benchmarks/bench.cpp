#include "spark/book.hpp"
#include "spark/journal.hpp"
#include "spark/strategy.hpp"
#include "spark/venues.hpp"

#include <cstdio>
#include <vector>

static int64_t pct(std::vector<int64_t> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
  return v[i];
}

int main() {
  std::string raw;
  if (!spark::load_file("tests/fixtures/hl_l2_btc.json", raw) &&
      !spark::load_file("spark/tests/fixtures/hl_l2_btc.json", raw)) {
    std::fprintf(stderr, "fixture missing\n");
    return 1;
  }
  constexpr int N = 20000;
  std::vector<int64_t> parse_ns;
  std::vector<int64_t> book_ns;
  parse_ns.reserve(N);
  book_ns.reserve(N);
  spark::Json j;
  std::string err;
  for (int i = 0; i < N; ++i) {
    int64_t t0 = spark::Clock::monotonic_ns();
    if (!spark::parse_json(raw, j, err)) return 2;
    int64_t t1 = spark::Clock::monotonic_ns();
    spark::ParsedBook pb;
    if (!spark::parse_hl_l2(j, pb)) return 3;
    spark::OrderBook book;
    book.apply_snapshot(pb.bids, pb.asks, pb.exchange_ns, true);
    int64_t t2 = spark::Clock::monotonic_ns();
    parse_ns.push_back(t1 - t0);
    book_ns.push_back(t2 - t1);
    int64_t mid = 0;
    if (!book.mid_px(mid)) return 4;
  }
  std::vector<int64_t> q_ns;
  spark::SpscQueue<int, 1024> q;
  constexpr int QN = 200000;
  int64_t qs = spark::Clock::monotonic_ns();
  int v = 0;
  int pushed = 0, popped = 0;
  while (pushed < QN || popped < QN) {
    if (pushed < QN && q.push(pushed)) ++pushed;
    if (q.pop(v)) ++popped;
  }
  int64_t qe = spark::Clock::monotonic_ns();
  (void)q_ns;

  spark::GlftParams p;
  p.gamma = 0.1;
  p.xi = 0.1;
  p.A = 1.2;
  p.k = 1.5;
  p.sigma = 2.5;
  p.delta_t = 1;
  p.calibrated = false;
  std::vector<int64_t> glft_ns;
  for (int i = 0; i < N; ++i) {
    int64_t t0 = spark::Clock::monotonic_ns();
    auto quotes = spark::glft_quotes(73920.0, 0.5, p);
    int64_t t1 = spark::Clock::monotonic_ns();
    if (!quotes.ok) return 5;
    glft_ns.push_back(t1 - t0);
  }

  const char* path = "/tmp/spark_bench_journal.bin";
  std::remove(path);
  spark::JournalWriter w(path);
  std::vector<spark::Level> bids(20), asks(20);
  for (int i = 0; i < 20; ++i) {
    bids[i] = {1000 - i, 10, 1};
    asks[i] = {1001 + i, 10, 1};
  }
  auto payload = spark::encode_book_snapshot(bids, asks, true);
  int64_t js = spark::Clock::monotonic_ns();
  constexpr int JN = 5000;
  for (int i = 0; i < JN; ++i) {
    if (!w.append(spark::EventType::BookSnapshot, i, i, i, payload.data(),
                  static_cast<uint32_t>(payload.size())))
      return 6;
  }
  w.flush();
  int64_t je = spark::Clock::monotonic_ns();
  std::vector<spark::JournalRecord> recs;
  int64_t rs = spark::Clock::monotonic_ns();
  if (!spark::read_journal(path, recs, err)) return 7;
  int64_t re = spark::Clock::monotonic_ns();

  std::printf("hardware_note host process, not a colocated measurement\n");
  std::printf("samples_parse %d\n", N);
  std::printf("parse_ns p50 %lld p90 %lld p99 %lld max %lld\n",
              (long long)pct(parse_ns, 0.50), (long long)pct(parse_ns, 0.90),
              (long long)pct(parse_ns, 0.99), (long long)pct(parse_ns, 1));
  std::printf("book_apply_ns p50 %lld p90 %lld p99 %lld max %lld\n",
              (long long)pct(book_ns, 0.50), (long long)pct(book_ns, 0.90),
              (long long)pct(book_ns, 0.99), (long long)pct(book_ns, 1));
  std::printf("spsc_roundtrip_ns_per_op %.2f count %d\n",
              double(qe - qs) / double(QN), QN);
  std::printf("glft_ns p50 %lld p99 %lld\n", (long long)pct(glft_ns, 0.50),
              (long long)pct(glft_ns, 0.99));
  std::printf("journal_append_ns_per %0.2f replay_ns_per %0.2f records %zu\n",
              double(je - js) / JN, double(re - rs) / double(recs.size()), recs.size());
  return recs.size() == static_cast<size_t>(JN) ? 0 : 8;
}
