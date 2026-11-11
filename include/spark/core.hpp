#pragma once

// Spark core types.
// Prices and quantities are integer-scaled. Accounting never uses binary
// floating point. Quote *models* may use double and must round back to ticks
// before an order is considered.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>

namespace spark {

inline constexpr int kPriceScale = 8;
inline constexpr int kQtyScale = 8;

enum class Venue : uint8_t { Unknown = 0, Hyperliquid = 1, Lighter = 2 };
enum class Side : uint8_t { Buy = 1, Sell = 2 };
enum class Tif : uint8_t { Gtc = 1, Ioc = 2, Alo = 3 };
enum class Liquidity : uint8_t { Unknown = 0, Maker = 1, Taker = 2 };

inline constexpr int side_sign(Side s) noexcept { return s == Side::Buy ? 1 : -1; }

inline const char* venue_name(Venue v) noexcept {
  switch (v) {
    case Venue::Hyperliquid: return "hyperliquid";
    case Venue::Lighter: return "lighter";
    default: return "unknown";
  }
}

// Parse a decimal string into an integer with exactly `scale` fractional digits.
// Returns false on empty input, extra junk, overflow, or nonzero digits beyond `scale`.
// Trailing zeros beyond `scale` are accepted (they do not change the value).
inline bool parse_scaled(std::string_view s, int scale, int64_t& out) noexcept {
  if (s.empty() || scale < 0 || scale > 18) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+') {
    ++i;
  } else if (s[0] == '-') {
    neg = true;
    ++i;
  }
  if (i >= s.size()) return false;
  uint64_t ip = 0;
  bool any = false;
  constexpr uint64_t kLim = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    any = true;
    uint64_t d = static_cast<uint64_t>(s[i] - '0');
    if (ip > (kLim - d) / 10ull) return false;
    ip = ip * 10ull + d;
  }
  uint64_t frac = 0;
  int frac_digits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      any = true;
      int d = s[i] - '0';
      if (frac_digits < scale) {
        frac = frac * 10ull + static_cast<uint64_t>(d);
        ++frac_digits;
      } else if (d != 0) {
        return false;
      }
    }
  }
  if (!any || i != s.size()) return false;
  while (frac_digits < scale) {
    if (frac > kLim / 10ull) return false;
    frac *= 10ull;
    ++frac_digits;
  }
  uint64_t factor = 1;
  for (int k = 0; k < scale; ++k) {
    if (factor > kLim / 10ull) return false;
    factor *= 10ull;
  }
  if (factor != 0 && ip > kLim / factor) return false;
  uint64_t mag = ip * factor + frac;
  if (mag > kLim) return false;
  if (neg && mag == kLim) {
    // INT64_MIN cannot be represented as a positive magnitude in int64.
    return false;
  }
  out = neg ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
  return true;
}

inline std::string format_scaled(int64_t v, int scale) {
  bool neg = v < 0;
  uint64_t mag;
  if (v == std::numeric_limits<int64_t>::min()) {
    mag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ull;
  } else {
    mag = static_cast<uint64_t>(neg ? -v : v);
  }
  uint64_t factor = 1;
  for (int k = 0; k < scale; ++k) factor *= 10ull;
  uint64_t ip = factor ? mag / factor : mag;
  uint64_t frac = factor ? mag % factor : 0;
  std::string s = neg ? "-" : "";
  s += std::to_string(ip);
  if (scale > 0) {
    s.push_back('.');
    std::string f = std::to_string(frac);
    s.append(static_cast<size_t>(scale) - f.size(), '0');
    s += f;
  }
  return s;
}

struct Clock {
  static int64_t realtime_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
  }
  static int64_t monotonic_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
  }
};

// Four clocks that must not be substituted for one another.
struct Timestamps {
  int64_t exchange_ns = 0;    // venue event time; 0 means the venue did not provide one
  int64_t recv_ns = 0;        // local CLOCK_REALTIME when the payload was received
  int64_t process_ns = 0;     // local time after parse / book apply
  int64_t decision_ns = 0;    // strategy decision
  int64_t submit_ns = 0;      // local time the order request left the process
  int64_t ack_ns = 0;         // exchange acknowledgement time, if the venue stamps it
  int64_t fill_ns = 0;        // exchange fill time
};

// Power-of-two SPSC ring. Wait-free. One producer thread, one consumer thread.
// Capacity is N-1 usable slots.
template <typename T, size_t N>
class SpscQueue {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

 public:
  bool push(const T& v) noexcept {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t next = (t + 1) & (N - 1);
    if (next == head_.load(std::memory_order_acquire)) return false;
    buf_[t] = v;
    tail_.store(next, std::memory_order_release);
    return true;
  }
  bool pop(T& out) noexcept {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_.load(std::memory_order_acquire)) return false;
    out = buf_[h];
    head_.store((h + 1) & (N - 1), std::memory_order_release);
    return true;
  }
  size_t capacity() const noexcept { return N - 1; }

 private:
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  alignas(64) T buf_[N]{};
};

enum class OverflowPolicy : uint8_t { Reject = 1, DropOldest = 2 };

// Bounded MPMC queue. A mutex is the correct default until a measured
// contention profile justifies a lock-free ring. Overflow is explicit.
template <typename T>
class MpmcQueue {
 public:
  explicit MpmcQueue(size_t capacity, OverflowPolicy policy = OverflowPolicy::Reject)
      : cap_(capacity), policy_(policy) {}

  bool push(const T& v) {
    std::lock_guard<std::mutex> lock(mu_);
    if (q_.size() >= cap_) {
      if (policy_ == OverflowPolicy::Reject) return false;
      if (!q_.empty()) q_.pop_front();
    }
    q_.push_back(v);
    return true;
  }
  bool pop(T& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }
  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return q_.size();
  }

 private:
  size_t cap_;
  OverflowPolicy policy_;
  mutable std::mutex mu_;
  std::deque<T> q_;
};

inline uint32_t crc32(const uint8_t* data, size_t len) noexcept {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

}  // namespace spark
