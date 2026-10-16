#pragma once
#include "common.h"

namespace tcp_bench {
class OfflineInput {
 public:
  OfflineInput(const std::string &path, uint32_t pool_size);
  ktag_status next(Packet &);
  uint32_t link_type() const { return ktag_capture_get_link_type(capture_.get()); }
  std::string error() const { return ktag_capture_get_last_error(); }
 private:
  Handle<ktag_capture_context, ktag_capture_close> capture_;
};
inline bool recoverable_truncation(ktag_status status, uint64_t packets, const std::string &error) {
  return status == KTAG_ERROR_IO && packets != 0 && error.find("truncated") != std::string::npos;
}
} // namespace tcp_bench
