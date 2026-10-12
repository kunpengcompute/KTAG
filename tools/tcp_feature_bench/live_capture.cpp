#include "live_capture.h"
#include <poll.h>
#include <thread>

namespace tcp_bench {
LiveCapture::LiveCapture(const Config &c, const std::string &source, const std::string &destination) {
  char error[PCAP_ERRBUF_SIZE]{};
  handle_ = pcap_create(c.capture_interface.c_str(), error);
  require(handle_ != nullptr, "pcap_create: "+std::string(error));
  try {
    require(pcap_set_snaplen(handle_, int(c.snapshot_length)) == 0 &&
      pcap_set_promisc(handle_, 1) == 0 && pcap_set_timeout(handle_, 50) == 0 &&
      pcap_set_buffer_size(handle_, int(c.capture_buffer)) == 0, "cannot configure live capture");
    int status = pcap_activate(handle_);
    require(status >= 0, "pcap_activate: "+std::string(pcap_geterr(handle_)));
    require(pcap_datalink(handle_) == DLT_EN10MB, "live interface must provide Ethernet frames");
    bpf_program filter{};
    auto text = "ether src "+source+" and ether dst "+destination;
    require(pcap_compile(handle_, &filter, text.c_str(), 1, PCAP_NETMASK_UNKNOWN) == 0,
            "cannot compile replay MAC filter: "+std::string(pcap_geterr(handle_)));
    status = pcap_setfilter(handle_, &filter); pcap_freecode(&filter);
    require(status == 0, "cannot install replay MAC filter");
    // Direction is advisory: MAC filtering also excludes local outgoing traffic.
    pcap_setdirection(handle_, PCAP_D_IN);
    require(pcap_setnonblock(handle_, 1, error) == 0, "cannot enable nonblocking capture: "+std::string(error));
  } catch (...) { pcap_close(handle_); handle_ = nullptr; throw; }
}
LiveCapture::~LiveCapture() { if (handle_) pcap_close(handle_); }
bool LiveCapture::next(Packet &packet, uint64_t sequence) {
  pcap_pkthdr *h = nullptr; const u_char *data = nullptr;
  int status = pcap_next_ex(handle_, &h, &data);
  require(status >= 0, "live capture failed: "+std::string(pcap_geterr(handle_)));
  if (status == 0) return false;
  require(h->caplen == h->len, "live packet truncated by snapshot_length");
  packet.value.data = data; packet.value.captured_length = h->caplen;
  packet.value.original_length = h->len; packet.value.link_type = KTAG_CAPTURE_LINKTYPE_ETHERNET;
  packet.value.sequence = sequence;
  packet.value.timestamp.seconds = h->ts.tv_sec;
  packet.value.timestamp.microseconds = int32_t(h->ts.tv_usec);
  require(ktag_packet_make_owned(&packet.value) == KTAG_SUCCESS, "cannot copy live packet");
  return true;
}
void LiveCapture::wait() {
  int fd = pcap_get_selectable_fd(handle_);
  if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  else { pollfd p{fd, POLLIN, 0}; poll(&p, 1, 10); }
}
uint64_t LiveCapture::drops() {
  pcap_stat s{};
  require(pcap_stats(handle_, &s) == 0, "cannot obtain live capture drop statistics");
  return uint64_t(s.ps_drop)+s.ps_ifdrop;
}
} // namespace tcp_bench
