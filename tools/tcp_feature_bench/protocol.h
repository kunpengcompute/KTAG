#pragma once
#include "common.h"
#include <array>

namespace tcp_bench {
constexpr uint64_t magic = 0x4b54414754435031ULL;
constexpr uint32_t version = 1, header_size = 48, control_limit = 65536;
enum class Type : uint32_t {
  hello = 1, ready, source_begin, capture_ready, replay_done,
  features, source_end, source_done, end, done, progress, error
};
struct Message {
  Type type = Type::error;
  uint64_t run = 0, sequence = 0;
  uint32_t count = 0, raw_size = 0;
  Bytes body;
};
class Writer {
 public:
  Bytes data;
  void u32(uint32_t n) { for (int i = 3; i >= 0; --i) data.push_back(uint8_t(n >> (i*8))); }
  void u64(uint64_t n) { u32(uint32_t(n >> 32)); u32(uint32_t(n)); }
  void text(const std::string &s) {
    require(s.size() <= 4096, "control string exceeds limit"); u32(uint32_t(s.size()));
    data.insert(data.end(), s.begin(), s.end());
  }
};
class Reader {
 public:
  explicit Reader(const Bytes &data) : data_(data) {}
  uint32_t u32() {
    require(offset_ <= data_.size() && data_.size()-offset_ >= 4, "truncated integer");
    uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | data_[offset_++]; return n;
  }
  uint64_t u64() { auto high = u32(); auto low = u32(); return (uint64_t(high) << 32) | low; }
  std::string text() {
    auto n = u32(); require(n <= 4096 && n <= data_.size()-offset_, "invalid string length");
    std::string s(data_.begin()+offset_, data_.begin()+offset_+n); offset_ += n; return s;
  }
  void finish() const { require(offset_ == data_.size(), "trailing control bytes"); }
 private:
  const Bytes &data_; size_t offset_ = 0;
};
Bytes encode_header(const Message &);
Message decode_header(const Bytes &, uint32_t max_raw, uint32_t &body_size);
class Codec {
 public:
  explicit Codec(int level);
  ~Codec();
  Codec(const Codec &) = delete;
  Codec &operator=(const Codec &) = delete;
  Bytes compress(const Bytes &);
  Bytes decompress(const Message &, uint32_t limit);
 private:
  void *compressor_ = nullptr, *decompressor_ = nullptr;
};
inline void expect(const Message &m, Type type, uint64_t run) {
  if (m.type == Type::error) { Reader r(m.body); auto why = r.text(); r.finish();
    throw std::runtime_error("peer: " + why); }
  require(m.type == type && m.run == run, "unexpected message type or run ID");
}
} // namespace tcp_bench
