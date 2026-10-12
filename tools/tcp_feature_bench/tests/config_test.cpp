#include "common.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace tcp_bench;
static Config parse(std::vector<std::string> args) {
  args.insert(args.begin(), "ktag_tcp_bench"); std::vector<char *> argv;
  for (auto &arg : args) argv.push_back(arg.data()); return parse_config(int(argv.size()), argv.data());
}
template<class F> void rejects(F fn) {
  bool caught = false; try { fn(); } catch (const std::exception &) { caught = true; }
  require(caught, "invalid configuration was accepted");
}
int main() {
  auto path = std::filesystem::temp_directory_path()/
    ("ktag-tcp-config-test-"+std::to_string(now_ns())+".yaml");
  try {
    auto defaults = parse({"--role", "backend", "--prepare-only", "-p", "input.pcap"});
    require(defaults.feature_threads == 4 && defaults.level == 1, "bad defaults");
    { std::ofstream f(path); f << "feature:\n  threads: 2\n"; }
    auto minimal = parse({"--role", "backend", "--prepare-only", "--config", path.string(), "-p", "input.pcap"});
    require(minimal.feature_threads == 2, "partial YAML sections failed");
    { std::ofstream f(path); f << "pipeline:\n  feature:\n    threads: 4\n    lifecycle_mode: legacy\n"
      "  inference:\n    threads: 1\n    batch_size: 256\n  schema:\n    name: ktag_legacy_v1\n    version: 1\n"
      "  plugins: []\n  backpressure:\n    policy: block\n"; }
    auto ub = parse({"--role", "backend", "--prepare-only", "--config", path.string(), "-p", "input.pcap"});
    require(ub.rules_hash() == defaults.rules_hash(), "UB settings changed extraction contract");
    rejects([&] { parse({"--role", "backend", "--prepare-only", "--port", "-1", "-p", "input.pcap"}); });
    rejects([&] { parse({"--role", "backend", "--prepare-only", "--replay-mbps", "nan", "-p", "input.pcap"}); });
    rejects([&] { parse({"--role", "backend", "--prepare-only", "-p", path.string(), "--stats", path.string()}); });
    rejects([&] { parse({"--role", "frontend", "-p", "input.pcap"}); });
    { std::ofstream f(path); f << "schema:\n  name: custom\n"; }
    rejects([&] { parse({"--role", "backend", "--prepare-only", "--config", path.string(), "-p", "input.pcap"}); });
    std::filesystem::remove(path);
    std::cout << "PASS: defaults, partial YAML, UB YAML, invalid settings and output protection\n"; return 0;
  } catch (const std::exception &e) {
    std::filesystem::remove(path); std::cerr << e.what() << '\n'; return 1;
  }
}
