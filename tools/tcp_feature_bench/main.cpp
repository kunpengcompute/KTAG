#include "common.h"
#include "tcp_transport.h"
#include <iostream>

namespace tcp_bench { volatile std::sig_atomic_t interrupted = 0; }
static void on_signal(int) { tcp_bench::interrupted = 1; }
int main(int argc, char **argv) {
  if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
    tcp_bench::usage(); return argc == 1 ? 2 : 0;
  }
  std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
  tcp_bench::Metrics metrics; metrics.process_cpu_started = tcp_bench::process_cpu_ns();
  tcp_bench::Config config; bool configured = false;
  try {
    config = tcp_bench::parse_config(argc, argv); configured = true;
    metrics.nic_begin = tcp_bench::nic_snapshot(config.role == "frontend" ? config.capture_interface : config.replay_interface);
    std::cout << "role=" << config.role << " feature_threads=" << config.feature_threads
      << " zstd_level=" << config.level << " compression_records=" << config.compression_records
      << " inference_records=" << config.inference_records << std::endl;
    if (config.role == "frontend") tcp_bench::run_frontend(config, metrics);
    else tcp_bench::run_backend(config, metrics);
    metrics.write(config, true, ""); return 0;
  } catch (const std::exception &e) {
    std::cerr << "ktag_tcp_bench: " << e.what() << '\n';
    if (configured) try { metrics.write(config, false, e.what()); }
      catch (const std::exception &stats_error) { std::cerr << "stats: " << stats_error.what() << '\n'; }
    return 1;
  }
}
