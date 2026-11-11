#pragma once

// Venue parsers.
// Only fields verified against the official docs and a live response on
// 2026-11-11 are treated as present. See docs/EXCHANGE_PROTOCOLS.md.

#include "spark/book.hpp"
#include "spark/json.hpp"

#include <fstream>
#include <sstream>

namespace spark {

struct HlLevel {
  Level level;
};

struct ParsedBook {
  bool ok = false;
  std::string error;
  Venue venue = Venue::Unknown;
  std::string coin;
  int64_t exchange_ms = 0;  // venue timestamp as published; unit noted by the parser
  int64_t exchange_ns = 0;  // normalized only when the unit is known
  bool truncated = false;
  std::vector<Level> bids;
  std::vector<Level> asks;
  int64_t begin_nonce = 0;
  int64_t nonce = 0;
  bool is_snapshot = false;
  bool has_nonce = false;
};

inline bool level_from_strings(std::string_view px, std::string_view sz, int32_t n, Level& out,
                               std::string& err) {
  if (!parse_scaled(px, kPriceScale, out.px)) {
    err = "bad price";
    return false;
  }
  if (!parse_scaled(sz, kQtyScale, out.qty)) {
    err = "bad size";
    return false;
  }
  out.n_orders = n;
  return true;
}

// Hyperliquid info l2Book and WsBook.
// Shape: {"coin","time","levels":[bids, asks]} with {px, sz, n}.
// `time` is Unix milliseconds (official WsBook comment and live response).
// The book is a snapshot of the published depth, capped at 20 levels per side
// (5 if the subscription sets fast=true). There is no sequence number.
inline bool parse_hl_l2(const Json& j, ParsedBook& out) {
  out = ParsedBook{};
  out.venue = Venue::Hyperliquid;
  out.truncated = true;
  out.is_snapshot = true;
  const Json* coin = j.get("coin");
  const Json* time = j.get("time");
  const Json* levels = j.get("levels");
  if (!coin || coin->type != Json::Type::String || !time || time->type != Json::Type::Number ||
      !levels || levels->type != Json::Type::Array || levels->arr.size() != 2) {
    out.error = "hl l2 shape";
    return false;
  }
  out.coin = coin->text;
  int64_t ms = 0;
  if (!parse_scaled(time->text, 0, ms)) {
    out.error = "hl time";
    return false;
  }
  out.exchange_ms = ms;
  out.exchange_ns = ms * 1000000LL;
  auto parse_side = [&](const Json& side, std::vector<Level>& dest) {
    for (const Json& lv : side.arr) {
      if (lv.type != Json::Type::Object) {
        out.error = "hl level";
        return false;
      }
      const Json* px = lv.get("px");
      const Json* sz = lv.get("sz");
      const Json* n = lv.get("n");
      if (!px || !sz || px->type != Json::Type::String || sz->type != Json::Type::String) {
        out.error = "hl level fields";
        return false;
      }
      int32_t orders = -1;
      if (n && n->type == Json::Type::Number) {
        int64_t ni = 0;
        if (!parse_scaled(n->text, 0, ni)) {
          out.error = "hl n";
          return false;
        }
        orders = static_cast<int32_t>(ni);
      }
      Level L;
      if (!level_from_strings(px->text, sz->text, orders, L, out.error)) return false;
      dest.push_back(L);
    }
    return true;
  };
  if (!parse_side(levels->arr[0], out.bids)) return false;
  if (!parse_side(levels->arr[1], out.asks)) return false;
  out.ok = true;
  return true;
}

struct ParsedTrade {
  bool ok = false;
  std::string error;
  Venue venue = Venue::Unknown;
  std::string coin;
  Side aggressor = Side::Buy;  // HL `side` is the aggressive side. B = buy.
  int64_t px = 0;
  int64_t qty = 0;
  int64_t exchange_ns = 0;
  std::string tid;
  bool side_known = false;
};

// Hyperliquid trade. `side` "B" or "A" (buy / ask aggressor). `time` milliseconds.
// `tid` is not globally unique by itself; the docs say use (block_time, coin, tid).
inline bool parse_hl_trade(const Json& j, ParsedTrade& out) {
  out = ParsedTrade{};
  out.venue = Venue::Hyperliquid;
  const Json* coin = j.get("coin");
  const Json* side = j.get("side");
  const Json* px = j.get("px");
  const Json* sz = j.get("sz");
  const Json* time = j.get("time");
  const Json* tid = j.get("tid");
  if (!coin || !side || !px || !sz || !time) {
    out.error = "hl trade shape";
    return false;
  }
  out.coin = coin->text;
  if (side->text == "B") {
    out.aggressor = Side::Buy;
    out.side_known = true;
  } else if (side->text == "A") {
    out.aggressor = Side::Sell;
    out.side_known = true;
  } else {
    out.error = "hl trade side";
    return false;
  }
  if (!parse_scaled(px->text, kPriceScale, out.px) || !parse_scaled(sz->text, kQtyScale, out.qty)) {
    out.error = "hl trade px/sz";
    return false;
  }
  int64_t ms = 0;
  if (!parse_scaled(time->text, 0, ms)) {
    out.error = "hl trade time";
    return false;
  }
  out.exchange_ns = ms * 1000000LL;
  if (tid && tid->type == Json::Type::Number) out.tid = tid->text;
  out.ok = true;
  return true;
}

// Lighter order book message, official WS schema (docs.lighter.xyz/api/websocket).
// channel "order_book:{id}", type "subscribed/order_book" or "update/order_book".
// begin_nonce of this message must equal nonce of the previous message.
// offset is per API server and is NOT a continuity sequence. We store nonce only.
// timestamp in the example is 13 digits (milliseconds). last_updated_at is 16 digits
// (microseconds). We prefer last_updated_at when it is present and looks like micros.
// Size 0 deletes the level: ASSUMPTION, see OrderBook.
inline bool parse_lighter_book_msg(const Json& j, ParsedBook& out) {
  out = ParsedBook{};
  out.venue = Venue::Lighter;
  const Json* type = j.get("type");
  const Json* ob = j.get("order_book");
  if (!type || !ob || ob->type != Json::Type::Object) {
    out.error = "lighter book shape";
    return false;
  }
  out.is_snapshot = type->text.find("subscribed/") == 0;
  const Json* bids = ob->get("bids");
  const Json* asks = ob->get("asks");
  const Json* nonce = ob->get("nonce");
  const Json* begin = ob->get("begin_nonce");
  if (!bids || !asks || bids->type != Json::Type::Array || asks->type != Json::Type::Array) {
    out.error = "lighter levels";
    return false;
  }
  auto side = [&](const Json& arr, std::vector<Level>& dest) {
    for (const Json& lv : arr.arr) {
      const Json* px = lv.get("price");
      const Json* sz = lv.get("size");
      if (!px || !sz) {
        out.error = "lighter level fields";
        return false;
      }
      Level L;
      if (!level_from_strings(px->text, sz->text, -1, L, out.error)) return false;
      dest.push_back(L);
    }
    return true;
  };
  if (!side(*bids, out.bids) || !side(*asks, out.asks)) return false;
  if (nonce && nonce->type == Json::Type::Number) {
    if (!parse_scaled(nonce->text, 0, out.nonce)) {
      out.error = "nonce";
      return false;
    }
    out.has_nonce = true;
  }
  if (begin && begin->type == Json::Type::Number) {
    if (!parse_scaled(begin->text, 0, out.begin_nonce)) {
      out.error = "begin_nonce";
      return false;
    }
  }
  const Json* lu = ob->get("last_updated_at");
  const Json* ts = j.get("timestamp");
  if (lu && lu->type == Json::Type::Number && lu->text.size() >= 16) {
    int64_t us = 0;
    if (!parse_scaled(lu->text, 0, us)) {
      out.error = "last_updated_at";
      return false;
    }
    out.exchange_ns = us * 1000LL;
  } else if (ts && ts->type == Json::Type::Number) {
    int64_t ms = 0;
    if (!parse_scaled(ts->text, 0, ms)) {
      out.error = "timestamp";
      return false;
    }
    out.exchange_ns = ms * 1000000LL;
  }
  out.ok = true;
  return true;
}

inline bool load_file(const std::string& path, std::string& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace spark
