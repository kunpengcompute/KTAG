#include "common.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace tcp_bench;
static Config parse(std::vector<std::string> args) {
  args.insert(args.begin(), "ktag_tcp_pcap_bench"); std::vector<char *> argv;
  for (auto &arg : args) argv.push_back(arg.data());
  return parse_config(int(argv.size()), argv.data());
}
template<class F> static void rejects(F fn) {
  bool caught = false; try { fn(); } catch (const std::exception &) { caught = true; }
  require(caught, "invalid configuration accepted");
}
int main() {
  auto path = std::filesystem::temp_directory_path()/("ktag-local-config-"+std::to_string(now_ns())+".yaml");
  const std::vector<std::string> front{"--role", "frontend", "--local-ip", "127.0.0.1", "--connect", "127.0.0.1:19090", "-p", "input.pcap"};
  try {
    auto defaults = parse(front); require(defaults.packet_pool == 65536 && defaults.feature_threads == 4 && defaults.level == 1, "bad defaults");
    { std::ofstream f(path); f << "pipeline:\n  capture:\n    packet_pool_size: 2\n  feature:\n    threads: 4\n    flow_capacity: 65536\n"
      "    tcp_idle_timeout_us: 300000000\n    udp_idle_timeout_us: 30000000\n    udp_packet_limit: 100\n"
      "  schema:\n    name: ktag_legacy_v1\n  inference:\n    threads: 1\n  backpressure:\n    policy: block\n"
      "replay:\n  rate_mode: invalid_unused\n  source_mac: invalid_unused\n  drain_timeout_s: 0\n"; }
    auto args = front; args.insert(args.end(), {"--config", path.string()}); auto ub = parse(args);
    require(ub.packet_pool == 2 && ub.rules_hash() == defaults.rules_hash(), "UB configuration/default normalization changed semantics");
    auto append = [&](std::initializer_list<std::string> extra) { auto args = front; args.insert(args.end(), extra); return parse(args); };
    rejects([&] { append({"--replay-interface", "eth0"}); });
    rejects([&] { append({"--capture-interface", "eth0"}); });
    rejects([&] { append({"--port", "-1"}); });
    rejects([&] { append({"--result", "out.csv"}); });
    rejects([&] { append({"--stats", "input.pcap"}); });
    rejects([&] { parse({"--role", "backend", "--bind-ip", "127.0.0.1", "--model", "model", "--result", "out.csv", "-p", "input.pcap"}); });
    auto back = parse({"--role", "backend", "--bind-ip", "127.0.0.1", "--model", "model", "--result", "out.csv"});
    require(back.pcaps.empty(), "backend unexpectedly requires PCAP");
    { std::ofstream f(path); f << "feature:\n  flow_hash_seed: 4294967297\n"; }
    require(append({"--config", path.string()}).flow_hash_seed == 4294967297ULL, "UB 64-bit flow hash seed was narrowed");
    { std::ofstream f(path); f << "capture:\n  backend: dpdk\n"; }
    rejects([&] { append({"--config", path.string()}); });
    { std::ofstream f(path); f << "compression:\n  target_ratio: .nan\n"; }
    rejects([&] { append({"--config", path.string()}); });
    {
      std::ofstream f(path);
      f << "capture:\n  backend: dpdk\n  dpdk:\n    rx_threads: 30\n    rx_queue_count: 30\n"
           "    rx_worker_cpus: 0-29\nfeature:\n  threads: 60\n  worker_cpus: 30-89\n"
           "  feature_queue_capacity: 7\ninference:\n  threads: 16\n  worker_cpus: 1-16\n"
           "  queue_capacity: 3\ntransport:\n  sender_threads: 6\n  sender_worker_cpus: 90-95\n"
           "  receiver_threads: 1\n  receiver_worker_cpus: '0'\n";
    }
    auto online = parse({"--role", "frontend", "--local-ip", "127.0.0.1", "--connect", "127.0.0.1:19090",
                         "--config", path.string(), "-i", "0000:a5:00.0"});
    require(online.receiver_threads == 1 && online.receiver_cpus == std::vector<uint32_t>{0} &&
            online.inference_threads == 16 && online.inference_queue == 3 && online.feature_queue == 7,
            "receiver/inference configuration or independent queue capacities were lost");
    { std::ofstream f(path); f << "capture:\n  backend: dpdk\ntransport:\n  receiver_threads: 2\n"; }
    rejects([&] { parse({"--role", "backend", "--bind-ip", "127.0.0.1", "--model", "model", "--result", "out.csv",
                        "--config", path.string()}); });
    std::filesystem::remove(path);
    std::cout << "PASS: role separation, UB YAML, owned pool, normalized rules, replay isolation and output protection\n"; return 0;
  } catch (const std::exception &e) { std::filesystem::remove(path); std::cerr << e.what() << '\n'; return 1; }
}
