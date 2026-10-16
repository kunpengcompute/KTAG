#include "offline_input.h"

namespace tcp_bench {
OfflineInput::OfflineInput(const std::string &path, uint32_t pool_size)
  : capture_(nullptr, ktag_capture_close) {
  ktag_capture_config c{}; c.struct_size = sizeof(c); c.api_version = KTAG_API_VERSION;
  c.source_type = KTAG_CAPTURE_SOURCE_PCAP_FILE; c.source = path.c_str();
  c.packet_mode = KTAG_CAPTURE_PACKET_OWNED; c.snapshot_length = 65535;
  c.packet_buffer_capacity = pool_size;
  capture_.reset(ktag_capture_open(&c));
  require(bool(capture_), "capture open: "+std::string(ktag_capture_get_last_error()));
}
ktag_status OfflineInput::next(Packet &packet) { return ktag_capture_next(capture_.get(), &packet.value); }
} // namespace tcp_bench
