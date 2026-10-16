#include "protocol.h"
#include <zstd.h>

namespace tcp_bench {
static void zcheck(size_t value) {
  if (ZSTD_isError(value)) throw std::runtime_error(std::string("Zstd: ") + ZSTD_getErrorName(value));
}
Bytes encode_header(const Message &m) {
  require(m.body.size() <= UINT32_MAX, "message too large");
  Writer w; w.u64(magic); w.u32(version); w.u32(uint32_t(m.type));
  w.u64(m.run); w.u64(m.sequence); w.u32(m.count); w.u32(m.raw_size);
  w.u32(uint32_t(m.body.size())); w.u32(0); return std::move(w.data);
}
Message decode_header(const Bytes &data, uint32_t max_raw, uint32_t &size) {
  require(data.size() == header_size, "invalid header size"); Reader r(data);
  require(r.u64() == magic && r.u32() == version, "TCP protocol magic/version mismatch");
  auto type = r.u32(); require(type >= uint32_t(Type::hello) && type <= uint32_t(Type::error), "invalid message type");
  Message m; m.type = Type(type); m.run = r.u64(); m.sequence = r.u64();
  m.count = r.u32(); m.raw_size = r.u32(); size = r.u32();
  require(r.u32() == 0, "unsupported header flags"); r.finish();
  if (m.type == Type::features) {
    require(m.count > 0 && m.raw_size > 0 && m.raw_size <= max_raw && size > 0 &&
      size <= ZSTD_compressBound(max_raw), "invalid feature batch length/count");
  } else require(m.count == 0 && m.raw_size == 0 && m.sequence == 0 && size <= control_limit,
                 "invalid control header");
  return m;
}
Codec::Codec(int level) {
  compressor_ = ZSTD_createCCtx(); decompressor_ = ZSTD_createDCtx();
  if (!compressor_ || !decompressor_) {
    ZSTD_freeCCtx(static_cast<ZSTD_CCtx *>(compressor_));
    ZSTD_freeDCtx(static_cast<ZSTD_DCtx *>(decompressor_));
    throw std::runtime_error("cannot allocate Zstd context");
  }
  try {
    zcheck(ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx *>(compressor_), ZSTD_c_compressionLevel, level));
    zcheck(ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx *>(compressor_), ZSTD_c_checksumFlag, 1));
    zcheck(ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx *>(compressor_), ZSTD_c_nbWorkers, 0));
    zcheck(ZSTD_DCtx_setParameter(static_cast<ZSTD_DCtx *>(decompressor_), ZSTD_d_windowLogMax, 23));
  } catch (...) {
    ZSTD_freeCCtx(static_cast<ZSTD_CCtx *>(compressor_));
    ZSTD_freeDCtx(static_cast<ZSTD_DCtx *>(decompressor_)); throw;
  }
}
Codec::~Codec() {
  ZSTD_freeCCtx(static_cast<ZSTD_CCtx *>(compressor_));
  ZSTD_freeDCtx(static_cast<ZSTD_DCtx *>(decompressor_));
}
Bytes Codec::compress(const Bytes &raw) {
  Bytes out(ZSTD_compressBound(raw.size()));
  auto n = ZSTD_compress2(static_cast<ZSTD_CCtx *>(compressor_), out.data(), out.size(), raw.data(), raw.size());
  zcheck(n); out.resize(n); return out;
}
Bytes Codec::decompress(const Message &m, uint32_t limit) {
  require(m.raw_size && m.raw_size <= limit, "decompressed size exceeds limit");
  auto size = ZSTD_getFrameContentSize(m.body.data(), m.body.size());
  require(size == m.raw_size, "Zstd frame content size mismatch");
  auto consumed = ZSTD_findFrameCompressedSize(m.body.data(), m.body.size());
  zcheck(consumed); require(consumed == m.body.size(), "trailing or concatenated Zstd frames");
  Bytes out(m.raw_size);
  auto n = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx *>(decompressor_), out.data(), out.size(), m.body.data(), m.body.size());
  zcheck(n); require(n == out.size(), "decompressed length mismatch"); return out;
}
} // namespace tcp_bench
