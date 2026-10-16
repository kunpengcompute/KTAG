#include "common.h"
#include <filesystem>
#include <iostream>
#include <cmath>
#include <yaml-cpp/yaml.h>
#include <zstd.h>

namespace tcp_bench {
void usage() {
  std::cout << "KTAG local PCAP -> features -> Zstd -> TCP -> inference\n"
    "  --role frontend --local-ip IP --connect IP:PORT -p FILE [-p FILE ...]\n"
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
    if (n["plugins"]) require(n["plugins"].size() == 0, "only legacy builtins are supported");
    if (s["name"]) require(s["name"].as<std::string>() == "ktag_legacy_v1", "unsupported schema");
    if (s["version"]) require(s["version"].as<unsigned>() == 1, "unsupported schema version");
    for (auto key : {"feature_order", "feature_ids", "remove_features", "add_features"})
      if (s[key]) require(s[key].size() == 0, "exact Legacy V1 schema required");
    if (cap["backend"]) require(cap["backend"].as<std::string>() == "pcap", "local input requires capture backend pcap");
    read(cap, "packet_pool_size", c.packet_pool);
    read(f, "threads", c.feature_threads); read(f, "flow_hash_seed", c.flow_hash_seed);
    read(f, "queue_capacity", c.packet_queue); read(f, "packet_queue_capacity", c.packet_queue);
    read(f, "feature_queue_capacity", c.feature_queue); read(f, "flow_capacity", c.flow_capacity);
    read(f, "tcp_idle_timeout_us", c.tcp_idle_us); read(f, "udp_idle_timeout_us", c.udp_idle_us);
    read(f, "udp_packet_limit", c.udp_packet_limit); read(f, "expire_interval_ms", c.expire_ms);
    std::string lifecycle = "legacy"; read(f, "lifecycle_mode", lifecycle);
    require(lifecycle == "legacy" || lifecycle == "modern", "invalid lifecycle mode"); c.modern = lifecycle == "modern";
    read(inf, "batch_size", c.inference_records); read(inf, "batch_timeout_us", c.inference_timeout_us);
    read(inf, "queue_capacity", c.feature_queue); read(inf, "prediction_limit", c.prediction_limit);
    for (auto key : {"threads", "intra_op_threads", "inter_op_threads"})
      if (inf[key]) require(inf[key].as<unsigned>() == 1, "inference threads must be 1");
    read(inf, "model_package_path", c.model); read(n, "model_package_path", c.model);
    if (!c.model.empty() && std::filesystem::path(c.model).is_relative())
      c.model = (std::filesystem::absolute(c.config_path).parent_path()/c.model).lexically_normal().string();
    read(t, "bind_ip", c.local_ip); read(t, "port", c.port);
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
  if (c.role == "frontend") {
    require(!c.peer_ip.empty() && !c.pcaps.empty(), "frontend needs --connect and at least one -p PCAP");
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
