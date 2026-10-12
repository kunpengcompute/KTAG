#include "tcp_transport.h"
#include "replay_controller.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ifaddrs.h>
#include <netpacket/packet.h>

namespace tcp_bench {
void check_interface(const std::string &iface, const std::string &ip,
                     const std::string &source, const std::string &destination) {
  auto smac = parse_mac(source), dmac = parse_mac(destination);
  require(smac != dmac, "replay MACs must be distinct unicast addresses");
  in_addr local{}; require(inet_pton(AF_INET, ip.c_str(), &local) == 1, "invalid IPv4 address: "+ip);
  ifaddrs *list = nullptr;
  int status = getifaddrs(&list);
  require(status == 0, "getifaddrs: "+std::string(std::strerror(errno)));
  std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> holder(list, freeifaddrs);
  bool found_ip = false, found_link = false;
  for (auto *p = list; p; p = p->ifa_next) {
    if (!p->ifa_addr) continue;
    if (p->ifa_addr->sa_family == AF_PACKET) {
      auto *a = reinterpret_cast<sockaddr_ll *>(p->ifa_addr);
      if (a->sll_halen == 6)
        require(!std::equal(smac.begin(), smac.end(), a->sll_addr) &&
                !std::equal(dmac.begin(), dmac.end(), a->sll_addr), "replay MAC collides with a local NIC");
      if (iface == p->ifa_name) found_link = a->sll_halen == 6;
    }
    if (iface == p->ifa_name && p->ifa_addr->sa_family == AF_INET)
      found_ip |= reinterpret_cast<sockaddr_in *>(p->ifa_addr)->sin_addr.s_addr == local.s_addr;
  }
  require(found_link && found_ip, "selected interface must be Ethernet and own the bound IPv4 address");
}
} // namespace tcp_bench
