#include "tcp_transport.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <cstring>

namespace tcp_bench {
std::string local_interface(const Config &c) {
  in_addr address{};
  require(inet_pton(AF_INET, c.local_ip.c_str(), &address) == 1 && address.s_addr != INADDR_ANY,
          "bind a specific local IPv4 (including 127.0.0.1 for loopback)");
  ifaddrs *raw = nullptr; require(getifaddrs(&raw) == 0, "cannot enumerate local interfaces");
  std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> list(raw, freeifaddrs);
  for (auto *p = raw; p; p = p->ifa_next) if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
      reinterpret_cast<sockaddr_in *>(p->ifa_addr)->sin_addr.s_addr == address.s_addr &&
      (c.network_interface.empty() || c.network_interface == p->ifa_name)) return p->ifa_name;
  throw std::runtime_error("bound IPv4 is not assigned to the requested local interface");
}
} // namespace tcp_bench
