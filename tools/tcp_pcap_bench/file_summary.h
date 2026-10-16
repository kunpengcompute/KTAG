#pragma once
#include "protocol.h"

namespace tcp_bench {
inline void write_source(Writer &w, const SourceStats &s) {
  w.u32(s.index); w.u32(s.link_type); w.u64(s.packets); w.u64(s.bytes); w.u64(s.original_bytes);
  w.u64(s.short_captures); w.u64(s.unsupported); w.u64(s.processed); w.u64(s.rejected);
  w.u64(s.records); w.u64(s.pool_waits); w.text(s.warning);
}
inline SourceStats read_source(Reader &r) {
  SourceStats s; s.index = r.u32(); s.link_type = r.u32(); s.packets = r.u64(); s.bytes = r.u64();
  s.original_bytes = r.u64(); s.short_captures = r.u64(); s.unsupported = r.u64();
  s.processed = r.u64(); s.rejected = r.u64(); s.records = r.u64(); s.pool_waits = r.u64(); s.warning = r.text();
  require(s.short_captures <= s.packets && s.unsupported <= s.packets &&
          s.processed == s.packets-s.unsupported && s.rejected <= s.processed && s.bytes <= s.original_bytes,
          "invalid source packet accounting"); return s;
}
} // namespace tcp_bench
