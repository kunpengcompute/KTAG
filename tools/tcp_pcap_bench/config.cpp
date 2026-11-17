#include "common.h"
#include <filesystem>
#include <iostream>
#include <cmath>
#include <yaml-cpp/yaml.h>
#include <zstd.h>

namespace tcp_bench {
void usage() {
  std::cout << "KTAG PCAP / DPDK -> features -> Zstd -> TCP -> inference\n"
    "  --role frontend --local-ip IP --connect IP:PORT -p FILE [-p FILE ...]\n"
    "  --role frontend --config ONLINE_YAML --local-ip IP --connect IP:PORT -i PCI\n"
    "  --role backend --bind-ip IP --port PORT --model PACKAGE --result CSV\n"
    "  Optional: --config YAML --stats JSON --feature-audit FILE --network-interface IFACE\n";
}
template<class T> static void read(const YAML::Node &node, const char *key, T &out) {
  if (node && node.IsMap() && node[key]) out = node[key].as<T>();
}
static uint32_t integer(const std::string &value) {
  require(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
          "expected an unsigned integer: "+value);
  auto n = std::stoull(value); require(n <= UINT32_MAX, "integer out of range"); return uint32_t(n);
}
static std::vector<uint32_t> cpu_list(const YAML::Node &node, const char *key) {
  std::vector<uint32_t> result;
  if (!node[key]) return result;
  auto text = node[key].as<std::string>();
  size_t start = 0;
  while (start < text.size()) {
    auto end = text.find(',', start);
    auto part = text.substr(start, end == std::string::npos ? end : end-start);
    auto dash = part.find('-');
    auto first = integer(part.substr(0, dash));
    auto last = dash == std::string::npos ? first : integer(part.substr(dash+1));
    require(first <= last && last < 1024, "CPU range must be ascending and below 1024");
    for (auto cpu = first; cpu <= last; ++cpu) {
      require(std::find(result.begin(), result.end(), cpu) == result.end(), "duplicate worker CPU");
      result.push_back(cpu);
    }
    if (end == std::string::npos) break;
    start = end+1;
    require(start < text.size(), "trailing comma in CPU list");
  }
  return result;
}
uint64_t Config::rules_hash() const {
  // Normalize the public API's zero-as-default values, as in the original UB pipeline.
  std::string text = "ktag-local-pcap-rules-v1:"+std::to_string(feature_threads)+":"+
    std::to_string(flow_hash_seed)+":"+std::to_string(flow_capacity ? flow_capacity : KTAG_FEATURE_DEFAULT_FLOW_CAPACITY)+":"+
    std::to_string(tcp_idle_us ? tcp_idle_us : KTAG_FEATURE_DEFAULT_TCP_IDLE_TIMEOUT_US)+":"+
    std::to_string(udp_idle_us ? udp_idle_us : KTAG_FEATURE_DEFAULT_UDP_IDLE_TIMEOUT_US)+":"+
    std::to_string(udp_packet_limit ? udp_packet_limit : KTAG_FEATURE_DEFAULT_UDP_PACKET_LIMIT)+":"+
    std::to_string(expire_ms)+":"+std::to_string(modern);
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char ch : text) { hash ^= ch; hash *= 1099511628211ULL; } return hash;
}
Config parse_config(int argc, char **argv) {
  Config c;
  for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--config") {
    require(i+1 < argc, "--config needs a path"); c.config_path = argv[++i];
  }
  if (!c.config_path.empty()) {
    auto root = YAML::LoadFile(c.config_path); auto n = root["pipeline"] ? root["pipeline"] : root;
    auto section = [](const YAML::Node &p, const char *key) {
      return p[key] ? p[key] : YAML::Node(YAML::NodeType::Map);
    };
    auto f = section(n, "feature"), inf = section(n, "inference"), cap = section(n, "capture");
    auto s = section(n, "schema"), t = section(root, "transport"), z = section(root, "compression");
    if (n["plugins"]) {
      require(n["plugins"].IsSequence(), "pipeline.plugins must be a sequence");
      for (const auto &plugin : n["plugins"]) {
        auto path = std::filesystem::path(plugin.as<std::string>());
        require(!path.empty(), "feature plugin path is empty");
        if (path.is_relative()) path = std::filesystem::absolute(c.config_path).parent_path() / path;
        c.plugin_paths.push_back(path.lexically_normal().string());
      }
    }
    if (s["name"]) require(s["name"].as<std::string>() == "ktag_legacy_v1", "unsupported schema");
    if (s["version"]) require(s["version"].as<unsigned>() == 1, "unsupported schema version");
    for (auto key : {"feature_order", "feature_ids", "remove_features", "add_features"})
      if (s[key]) require(s[key].size() == 0, "exact Legacy V1 schema required");
    std::string capture_backend = "pcap"; read(cap, "backend", capture_backend);
    require(capture_backend == "pcap" || capture_backend == "dpdk", "capture.backend must be pcap or dpdk");
    c.online = capture_backend == "dpdk";
    read(cap, "packet_pool_size", c.packet_pool);
    auto dpdk = section(cap, "dpdk");
    read(dpdk, "device", c.device); read(dpdk, "eal_args", c.eal_args);
    read(dpdk, "rx_threads", c.rx_threads); read(dpdk, "rx_queue_count", c.rx_queues);
    read(dpdk, "rx_queue_depth", c.rx_depth); read(dpdk, "mbuf_pool_size", c.mbuf_pool);
    read(dpdk, "burst_size", c.burst); read(dpdk, "poll_interval_us", c.rx_poll_us);
    read(dpdk, "rx_idle_eof_us", c.rx_idle_eof_us); read(cap, "duration_s", c.duration_s);
    c.rx_cpus = cpu_list(dpdk, "rx_worker_cpus");
    c.feature_cpus = cpu_list(f, "worker_cpus");
    c.inference_cpus = cpu_list(inf, "worker_cpus");
    read(f, "threads", c.feature_threads); read(f, "flow_hash_seed", c.flow_hash_seed);
    read(f, "queue_capacity", c.packet_queue); read(f, "packet_queue_capacity", c.packet_queue);
    read(f, "feature_queue_capacity", c.feature_queue); read(f, "flow_capacity", c.flow_capacity);
    read(f, "tcp_idle_timeout_us", c.tcp_idle_us); read(f, "udp_idle_timeout_us", c.udp_idle_us);
    read(f, "udp_packet_limit", c.udp_packet_limit); read(f, "expire_interval_ms", c.expire_ms);
    std::string lifecycle = "legacy"; read(f, "lifecycle_mode", lifecycle);
    require(lifecycle == "legacy" || lifecycle == "modern", "invalid lifecycle mode"); c.modern = lifecycle == "modern";
    read(inf, "batch_size", c.inference_records); read(inf, "batch_timeout_us", c.inference_timeout_us);
    read(inf, "queue_capacity", c.feature_queue); read(inf, "prediction_limit", c.prediction_limit);
    read(inf, "threads", c.inference_threads);
    for (auto key : {"intra_op_threads", "inter_op_threads"})
      if (inf[key]) require(inf[key].as<unsigned>() == 1, "inference threads must be 1");
    read(inf, "model_package_path", c.model); read(n, "model_package_path", c.model);
    if (!c.model.empty() && std::filesystem::path(c.model).is_relative())
      c.model = (std::filesystem::absolute(c.config_path).parent_path()/c.model).lexically_normal().string();
    read(t, "bind_ip", c.local_ip); read(t, "port", c.port);
    read(t, "sender_threads", c.sender_threads);
    c.sender_cpus = cpu_list(t, "sender_worker_cpus");
    read(t, "batch_records", c.compression_records); read(t, "batch_timeout_us", c.compression_timeout_us);
    read(t, "max_uncompressed_bytes", c.max_body); read(t, "progress_timeout_s", c.timeout_s);
    read(z, "level", c.level); read(z, "target_ratio", c.target_ratio);
    if (z["codec"]) require(z["codec"].as<std::string>() == "zstd", "codec must be zstd");
    auto pressure = section(n, "backpressure");
    if (pressure["policy"]) require(pressure["policy"].as<std::string>() == "block", "backpressure must be block");
    auto ub = section(section(n, "distributed"), "ub_ring");
    if (ub["ground_truth_labels"] && ub["ground_truth_labels"].size())
      std::cerr << "WARNING: UB ground_truth_labels are not applied; source_index is the -p file index\n";
  }
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i]; require(i+1 < argc, "missing value for "+a); std::string v = argv[++i];
    if (a == "--config") continue;
    if (a == "--role") c.role = v;
    else if (a == "--bind-ip" || a == "--local-ip") c.local_ip = v;
    else if (a == "--connect") {
      auto p = v.rfind(':'); require(p != std::string::npos, "--connect expects IPv4:PORT");
      c.peer_ip = v.substr(0, p); c.port = integer(v.substr(p+1));
    } else if (a == "--port") c.port = integer(v);
    else if (a == "--model") c.model = v;
    else if (a == "--result") c.result = v;
    else if (a == "--stats") c.stats = v;
    else if (a == "--feature-audit") c.audit = v;
    else if (a == "--network-interface") c.network_interface = v;
    else if (a == "-p") c.pcaps.push_back(v);
    else if (a == "-i") { c.device = v; c.online = true; }
    else throw std::runtime_error("unknown local-PCAP option: "+a);
  }
  require(c.role == "frontend" || c.role == "backend", "--role must be frontend or backend");
  require(c.port && c.port <= 65535, "invalid port");
  require(c.feature_threads && c.feature_threads <= 128, "feature threads must be in [1,128]");
  require(c.packet_pool && c.packet_queue && c.feature_queue && c.prediction_limit, "capacities must be positive");
  require(c.timeout_s && c.compression_records && c.inference_records && c.compression_timeout_us && c.inference_timeout_us,
          "batch sizes and timeouts must be positive");
  require(c.max_body <= 4194304 && uint64_t(c.compression_records)*
          ktag_demo_ub_feature_frame_size(KTAG_LEGACY_V1_VALUE_COUNT) <= c.max_body, "invalid body/batch size (maximum 4 MiB)");
  require(c.level >= ZSTD_minCLevel() && c.level <= ZSTD_maxCLevel(), "invalid Zstd level");
  require(std::isfinite(c.target_ratio) && c.target_ratio > 0, "invalid target ratio");
  require(!c.local_ip.empty(), "local bind IPv4 is required");
  if (c.online) {
    require(c.rx_threads && c.rx_threads <= 128 && c.rx_threads == c.rx_queues,
            "online RX threads must equal RX queues, in [1,128]");
    require(c.inference_threads && c.inference_threads <= 128 && c.sender_threads &&
            c.sender_threads <= c.inference_threads && c.feature_queue >= c.sender_threads,
            "online worker counts must satisfy 1 <= senders <= inference threads <= 128");
    require(c.rx_depth && c.burst && c.burst <= 64, "invalid RX depth or burst size");
    require(c.audit.empty(), "online multi-channel feature audit is unsupported");
    auto check = [](const std::vector<uint32_t> &cpus, uint32_t count) {
      require(cpus.empty() || cpus.size() == count, "worker CPU count must equal thread count");
    };
    check(c.rx_cpus, c.rx_threads); check(c.feature_cpus, c.feature_threads);
    check(c.sender_cpus, c.sender_threads); check(c.inference_cpus, c.inference_threads);
    std::vector<uint32_t> front_cpus = c.rx_cpus;
    front_cpus.insert(front_cpus.end(), c.feature_cpus.begin(), c.feature_cpus.end());
    front_cpus.insert(front_cpus.end(), c.sender_cpus.begin(), c.sender_cpus.end());
    std::sort(front_cpus.begin(), front_cpus.end());
    require(std::adjacent_find(front_cpus.begin(), front_cpus.end()) == front_cpus.end(),
            "online frontend RX, feature and sender CPU sets must be disjoint");
  } else require(c.inference_threads == 1 && c.sender_threads == 1, "PCAP mode requires one sender and one inference worker");
  if (c.role == "frontend") {
    require(!c.peer_ip.empty(), "frontend needs --connect");
    require(c.online ? (c.pcaps.empty() && !c.device.empty()) : !c.pcaps.empty(),
            "frontend needs either online -i PCI (no -p) or at least one -p PCAP");
    require(c.pcaps.size() <= 100000 && c.result.empty(), "frontend input count too large or unexpected --result");
  } else require(c.pcaps.empty() && c.peer_ip.empty() && !c.model.empty() && !c.result.empty(),
                 "backend needs --model and --result; PCAP/--connect belong on frontend");
  std::vector<std::string> outputs;
  for (const auto &out : {c.result, c.stats, c.audit}) if (!out.empty()) outputs.push_back(out);
  auto inputs = c.pcaps; if (!c.config_path.empty()) inputs.push_back(c.config_path);
  auto same = [](const std::filesystem::path &a, const std::filesystem::path &b) {
    return a == b || (std::filesystem::exists(a) && std::filesystem::exists(b) && std::filesystem::equivalent(a, b));
  };
  for (size_t i = 0; i < outputs.size(); ++i) {
    auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(outputs[i]));
    for (const auto &in : inputs) require(!same(path, std::filesystem::weakly_canonical(std::filesystem::absolute(in))),
                                        "output must not overwrite a PCAP/config input");
    for (size_t j = 0; j < i; ++j) require(!same(path, std::filesystem::weakly_canonical(std::filesystem::absolute(outputs[j]))),
                                         "result/stats/audit paths must be distinct");
    if (!c.model.empty()) {
      auto model = std::filesystem::weakly_canonical(std::filesystem::absolute(c.model)); auto relative = path.lexically_relative(model);
      require(!relative.empty() && *relative.begin() == "..", "output must not overwrite model package files");
    }
  }
  return c;
}
Schema make_schema() {
  Schema s(ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
  require(bool(s), "cannot create Legacy V1 schema"); return s;
}
} // namespace tcp_bench
