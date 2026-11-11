#pragma once

// Append-only event journal.
// The layout is inspired by Chronicle Queue's idea (a durable, ordered,
// recoverable excerpt stream with rotation) but it is not Chronicle Queue
// and it does not link the Java library. Records are length-prefixed,
// checksummed, and little-endian. A torn tail is ignored on read.

#include "spark/core.hpp"

#include <fstream>
#include <vector>

namespace spark {

inline constexpr uint32_t kJournalMagic = 0x4B525053u;  // 'SPRK' little-endian
inline constexpr uint16_t kJournalVersion = 1;
inline constexpr uint32_t kRecordMagic = 0x544E5645u;  // 'EVNT'

enum class EventType : uint16_t {
  BookSnapshot = 1,
  BookDelta = 2,
  Trade = 3,
  Funding = 4,
  Decision = 5,
  OrderNew = 6,
  OrderAck = 7,
  OrderReject = 8,
  OrderCancel = 9,
  Fill = 10,
  Position = 11,
  Risk = 12,
  Connection = 13,
  Sync = 14,
};

struct RecordHeader {
  uint32_t magic = kRecordMagic;
  uint16_t schema = kJournalVersion;
  uint16_t type = 0;
  int64_t exchange_ns = 0;
  int64_t recv_ns = 0;
  int64_t process_ns = 0;
  uint32_t payload_len = 0;
  uint32_t crc = 0;
};

inline constexpr size_t kRecordHeaderBytes = 4 + 2 + 2 + 8 + 8 + 8 + 4 + 4;

inline void write_u16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void write_u32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline void write_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
inline uint64_t read_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

inline void encode_header(const RecordHeader& h, uint8_t out[kRecordHeaderBytes]) {
  write_u32(out + 0, h.magic);
  write_u16(out + 4, h.schema);
  write_u16(out + 6, h.type);
  write_u64(out + 8, static_cast<uint64_t>(h.exchange_ns));
  write_u64(out + 16, static_cast<uint64_t>(h.recv_ns));
  write_u64(out + 24, static_cast<uint64_t>(h.process_ns));
  write_u32(out + 32, h.payload_len);
  write_u32(out + 36, h.crc);
}

inline bool decode_header(const uint8_t* in, RecordHeader& h) {
  h.magic = read_u32(in + 0);
  h.schema = read_u16(in + 4);
  h.type = read_u16(in + 6);
  h.exchange_ns = static_cast<int64_t>(read_u64(in + 8));
  h.recv_ns = static_cast<int64_t>(read_u64(in + 16));
  h.process_ns = static_cast<int64_t>(read_u64(in + 24));
  h.payload_len = read_u32(in + 32);
  h.crc = read_u32(in + 36);
  return h.magic == kRecordMagic && h.schema == kJournalVersion;
}

struct JournalRecord {
  RecordHeader header;
  std::vector<uint8_t> payload;
};

class JournalWriter {
 public:
  JournalWriter() = default;
  explicit JournalWriter(std::string path, uint64_t rotate_bytes = 64ull << 20) { open(path, rotate_bytes); }

  bool open(std::string path, uint64_t rotate_bytes = 64ull << 20) {
    path_ = std::move(path);
    rotate_bytes_ = rotate_bytes;
    part_ = 0;
    return open_current(false);
  }

  bool append(EventType type, int64_t exchange_ns, int64_t recv_ns, int64_t process_ns,
              const uint8_t* payload, uint32_t len) {
    if (!out_.is_open()) return false;
    if (rotate_bytes_ > 0 && bytes_ > rotate_bytes_) {
      out_.close();
      ++part_;
      if (!open_current(false)) return false;
    }
    RecordHeader h;
    h.type = static_cast<uint16_t>(type);
    h.exchange_ns = exchange_ns;
    h.recv_ns = recv_ns;
    h.process_ns = process_ns;
    h.payload_len = len;
    h.crc = crc32(payload, len);
    uint8_t raw[kRecordHeaderBytes];
    encode_header(h, raw);
    out_.write(reinterpret_cast<const char*>(raw), kRecordHeaderBytes);
    if (len) out_.write(reinterpret_cast<const char*>(payload), len);
    if (!out_) return false;
    bytes_ += kRecordHeaderBytes + len;
    ++count_;
    return true;
  }

  void flush() {
    if (out_.is_open()) out_.flush();
  }
  uint64_t count() const noexcept { return count_; }
  const std::string& path() const noexcept { return current_; }

 private:
  bool open_current(bool) {
    current_ = path_;
    if (part_ > 0) current_ += "." + std::to_string(part_);
    out_.open(current_, std::ios::binary | std::ios::app);
    if (!out_) return false;
    out_.seekp(0, std::ios::end);
    bytes_ = static_cast<uint64_t>(out_.tellp());
    if (bytes_ == 0) {
      uint8_t mag[8];
      write_u32(mag, kJournalMagic);
      write_u16(mag + 4, kJournalVersion);
      write_u16(mag + 6, 0);
      out_.write(reinterpret_cast<char*>(mag), 8);
      bytes_ = 8;
    }
    return static_cast<bool>(out_);
  }

  std::string path_;
  std::string current_;
  std::ofstream out_;
  uint64_t rotate_bytes_ = 0;
  uint64_t bytes_ = 0;
  uint64_t count_ = 0;
  int part_ = 0;
};

inline bool read_journal(const std::string& path, std::vector<JournalRecord>& out, std::string& err,
                         bool stop_on_torn_tail = true) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "open failed";
    return false;
  }
  uint8_t mag[8];
  in.read(reinterpret_cast<char*>(mag), 8);
  if (in.gcount() != 8 || read_u32(mag) != kJournalMagic || read_u16(mag + 4) != kJournalVersion) {
    err = "bad journal header";
    return false;
  }
  out.clear();
  while (true) {
    uint8_t raw[kRecordHeaderBytes];
    in.read(reinterpret_cast<char*>(raw), kRecordHeaderBytes);
    if (in.gcount() == 0) break;
    if (in.gcount() != static_cast<std::streamsize>(kRecordHeaderBytes)) {
      if (stop_on_torn_tail) break;
      err = "torn header";
      return false;
    }
    RecordHeader h;
    if (!decode_header(raw, h)) {
      if (stop_on_torn_tail) break;
      err = "bad record header";
      return false;
    }
    if (h.payload_len > 32u * 1024u * 1024u) {
      err = "payload too large";
      return false;
    }
    JournalRecord rec;
    rec.header = h;
    rec.payload.resize(h.payload_len);
    if (h.payload_len) {
      in.read(reinterpret_cast<char*>(rec.payload.data()), h.payload_len);
      if (in.gcount() != static_cast<std::streamsize>(h.payload_len)) {
        if (stop_on_torn_tail) break;
        err = "torn payload";
        return false;
      }
    }
    uint32_t c = crc32(rec.payload.data(), rec.payload.size());
    if (c != h.crc) {
      if (stop_on_torn_tail) break;
      err = "crc mismatch";
      return false;
    }
    out.push_back(std::move(rec));
  }
  return true;
}

// Book snapshot payload: u32 nbids, then (i64 px, i64 qty, i32 n) * nbids, same for asks, u8 truncated.
inline std::vector<uint8_t> encode_book_snapshot(const std::vector<Level>& bids,
                                                const std::vector<Level>& asks, bool truncated) {
  std::vector<uint8_t> p(8 + (bids.size() + asks.size()) * 20 + 1);
  uint8_t* w = p.data();
  write_u32(w, static_cast<uint32_t>(bids.size()));
  w += 4;
  auto put_level = [&](const Level& l) {
    write_u64(w, static_cast<uint64_t>(l.px));
    write_u64(w + 8, static_cast<uint64_t>(l.qty));
    write_u32(w + 16, static_cast<uint32_t>(l.n_orders));
    w += 20;
  };
  for (const Level& l : bids) put_level(l);
  write_u32(w, static_cast<uint32_t>(asks.size()));
  w += 4;
  for (const Level& l : asks) put_level(l);
  *w++ = truncated ? 1 : 0;
  p.resize(static_cast<size_t>(w - p.data()));
  return p;
}

inline bool decode_book_snapshot(const uint8_t* p, size_t n, std::vector<Level>& bids,
                                 std::vector<Level>& asks, bool& truncated) {
  if (n < 9) return false;
  uint32_t nb = read_u32(p);
  size_t need = 4ull + static_cast<size_t>(nb) * 20ull + 4ull;
  if (n < need) return false;
  const uint8_t* w = p + 4;
  bids.resize(nb);
  for (uint32_t i = 0; i < nb; ++i) {
    bids[i].px = static_cast<int64_t>(read_u64(w));
    bids[i].qty = static_cast<int64_t>(read_u64(w + 8));
    bids[i].n_orders = static_cast<int32_t>(read_u32(w + 16));
    w += 20;
  }
  uint32_t na = read_u32(w);
  w += 4;
  need = static_cast<size_t>(w - p) + static_cast<size_t>(na) * 20ull + 1ull;
  if (n < need) return false;
  asks.resize(na);
  for (uint32_t i = 0; i < na; ++i) {
    asks[i].px = static_cast<int64_t>(read_u64(w));
    asks[i].qty = static_cast<int64_t>(read_u64(w + 8));
    asks[i].n_orders = static_cast<int32_t>(read_u32(w + 16));
    w += 20;
  }
  truncated = *w != 0;
  return true;
}

}  // namespace spark
