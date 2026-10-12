#include "replay_controller.h"
#include "tcp_transport.h"
#include <pcap/pcap.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <net/if.h>
#include <regex>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;

namespace tcp_bench {
std::array<uint8_t, 6> parse_mac(const std::string &text) {
  require(std::regex_match(text, std::regex("[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}")), "invalid MAC: "+text);
  std::array<uint8_t, 6> out{};
  for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(std::stoul(text.substr(i*3, 2), nullptr, 16));
  require(std::any_of(out.begin(), out.end(), [](uint8_t n) { return n != 0; }) && !(out[0]&1),
          "replay MAC must be nonzero unicast");
  return out;
}
static int interface_mtu(const std::string &name) {
  require(name.size() < IFNAMSIZ, "interface name too long");
  int fd = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, 0);
  require(fd >= 0, "cannot query replay MTU");
  ifreq request{};
  std::memcpy(request.ifr_name, name.c_str(), name.size()+1);
  int status = ioctl(fd, SIOCGIFMTU, &request); close(fd);
  require(status == 0, "cannot query replay MTU: "+name); return request.ifr_mtu;
}
ReplayFiles::ReplayFiles(const Config &c) {
  auto smac = parse_mac(c.source_mac), dmac = parse_mac(c.destination_mac);
  require(smac != dmac, "replay MACs must differ");
  int mtu = c.replay_interface.empty() ? 65535 : interface_mtu(c.replay_interface);
  char pattern[] = "/tmp/ktag-tcp-replay-XXXXXX";
  auto *dir = mkdtemp(pattern); require(dir != nullptr, "cannot create private replay directory");
  directory_ = dir;
  try {
    for (size_t index = 0; index < c.pcaps.size(); ++index) {
      ReplayFile file; file.original = c.pcaps[index]; file.source_index = uint32_t(index);
      file.prepared = directory_+"/"+std::to_string(index)+".pcap";
      file.log = directory_+"/"+std::to_string(index)+".log";
      // Register ownership before opening anything, so partial failures clean up.
      files.push_back(file);
      auto &out = files.back(); char error[PCAP_ERRBUF_SIZE]{};
      std::unique_ptr<pcap_t, decltype(&pcap_close)> input(
        pcap_open_offline_with_tstamp_precision(file.original.c_str(), PCAP_TSTAMP_PRECISION_NANO, error), pcap_close);
      require(bool(input), "cannot open PCAP "+file.original+": "+error);
      require(pcap_datalink(input.get()) == DLT_EN10MB, "PCAP must contain Ethernet frames: "+file.original);
      std::unique_ptr<pcap_t, decltype(&pcap_close)> dead(
        pcap_open_dead_with_tstamp_precision(DLT_EN10MB, 65535, PCAP_TSTAMP_PRECISION_NANO), pcap_close);
      require(bool(dead), "cannot create replay PCAP format");
      std::unique_ptr<pcap_dumper_t, decltype(&pcap_dump_close)> dump(
        pcap_dump_open(dead.get(), file.prepared.c_str()), pcap_dump_close);
      require(bool(dump), "cannot create replay PCAP: "+std::string(pcap_geterr(dead.get())));
      pcap_pkthdr *header = nullptr; const u_char *data = nullptr; int status;
      while ((status = pcap_next_ex(input.get(), &header, &data)) >= 0) {
        require(!interrupted, "interrupted preparing PCAP"); if (!status) continue;
        require(header->caplen == header->len && header->caplen >= 14 &&
                header->caplen <= c.snapshot_length, "truncated/invalid/oversize PCAP record: "+file.original);
        size_t l2 = 14; uint16_t ether = uint16_t(data[12] << 8) | data[13];
        while (ether == 0x8100 || ether == 0x88a8) {
          require(header->caplen >= l2+4, "truncated VLAN header");
          ether = uint16_t(data[l2+2] << 8) | data[l2+3]; l2 += 4;
        }
        require(header->caplen <= uint64_t(mtu)+l2, "PCAP record exceeds replay interface MTU: "+file.original);
        Bytes copy(data, data+header->caplen);
        std::copy(dmac.begin(), dmac.end(), copy.begin());
        std::copy(smac.begin(), smac.end(), copy.begin()+6);
        pcap_dump(reinterpret_cast<u_char *>(dump.get()), header, copy.data());
        ++out.packets; out.bytes += header->caplen;
      }
      require(status == PCAP_ERROR_BREAK, "PCAP read failed: "+std::string(pcap_geterr(input.get())));
      require(out.packets > 0, "empty PCAP: "+file.original);
      require(pcap_dump_flush(dump.get()) == 0, "replay PCAP write failed");
      std::cout << "Prepared source=" << index << " packets=" << out.packets << " bytes=" << out.bytes
                << " file=" << file.original << '\n';
    }
  } catch (...) { cleanup(); throw; }
}
void ReplayFiles::cleanup() noexcept {
  std::error_code error;
  for (const auto &file : files) {
    std::filesystem::remove(file.prepared, error); std::filesystem::remove(file.log, error);
  }
  if (!directory_.empty()) std::filesystem::remove(directory_, error);
}
ReplayFiles::~ReplayFiles() { cleanup(); }

void replay_file(const Config &c, const ReplayFile &file, Metrics &metrics,
                 const std::atomic<bool> &cancel, const std::function<void()> &heartbeat) {
  std::vector<std::string> args;
  if (!c.replay_cpus.empty()) args = {"taskset", "-c", c.replay_cpus};
  args.push_back(c.replay_program); args.push_back("--intf1="+c.replay_interface);
  args.push_back("--loop=1");
  if (c.rate_mode == "mbps") args.push_back("--mbps="+std::to_string(c.mbps));
  if (c.preload) args.push_back("--preload-pcap");
  args.push_back(file.prepared);
  std::vector<char *> argv; for (auto &a : args) argv.push_back(a.data()); argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  require(posix_spawn_file_actions_init(&actions) == 0, "cannot prepare replay subprocess");
  int error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, file.log.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0600);
  if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t pid = -1; auto started = now_ns();
  if (!error) error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  require(error == 0, "cannot start tcpreplay/taskset: "+std::string(std::strerror(error)));
  int status = 0; rusage usage{}; bool terminating = false; uint64_t terminate_at = 0;
  auto heartbeat_interval = std::min<uint64_t>(1000000000ULL, uint64_t(c.timeout_s)*1000000000ULL/3);
  auto heartbeat_at = now_ns()+heartbeat_interval;
  try {
    for (;;) {
      auto n = wait4(pid, &status, WNOHANG, &usage);
      if (n == pid) break;
      require(n == 0 || (n < 0 && errno == EINTR), "cannot wait for replay subprocess");
      if ((cancel || interrupted) && !terminating) {
        kill(pid, SIGTERM); terminating = true; terminate_at = now_ns();
      }
      if (terminating && now_ns()-terminate_at > 2000000000ULL) kill(pid, SIGKILL);
      if (!terminating && now_ns() >= heartbeat_at) {
        heartbeat(); heartbeat_at = now_ns()+heartbeat_interval;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  } catch (...) {
    kill(pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} throw;
  }
  metrics.replay_ns += now_ns()-started;
  metrics.replay_cpu_ns += uint64_t(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000000ULL +
    uint64_t(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)*1000ULL;
  std::ifstream log(file.log); std::string text((std::istreambuf_iterator<char>(log)), {});
  // Always preserve tool output in the parent's log before private temp cleanup.
  std::cout << "tcpreplay source=" << file.source_index << "\n" << text << std::flush;
  require(!terminating && !cancel && !interrupted, "replay cancelled");
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "tcpreplay failed; see tool output above");
  auto count = [&](const char *key) {
    std::smatch match;
    require(std::regex_search(text, match, std::regex(std::string(key)+"\\s*:\\s*([0-9]+)")),
            "tcpreplay output lacks "+std::string(key)+"; cannot verify replay count");
    return std::stoull(match[1].str());
  };
  auto successful = count("Successful packets"), failed = count("Failed packets");
  metrics.sent_packets += successful;
  require(successful == file.packets && failed == 0, "tcpreplay did not send every PCAP record successfully");
  metrics.replay_bytes += file.bytes;
}
} // namespace tcp_bench
