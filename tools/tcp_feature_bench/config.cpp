#include "common.h"
#include <filesystem>
#include <iostream>
#include <limits>
#include <cmath>
#include <yaml-cpp/yaml.h>
#include <zstd.h>

namespace tcp_bench {
void usage() {
  std::cout << "KTAG TCP/Zstd two-server demo\n"
    "  --role backend --config CONFIG --bind-ip IP --replay-interface IFACE\n"
    "      -p FILE [-p FILE ...] --model PACKAGE --result FILE --stats FILE\n"
    "  --role frontend --config CONFIG --local-ip IP --connect IP:PORT\n"
    "      --capture-interface IFACE --stats FILE\n"
    "  Optional: --port N --replay-mbps N --replay-cpus LIST --feature-audit FILE\n"
    "  Backend: --prepare-only (validate and create replay PCAPs; no network/model)\n";
}
template<class T> void read(const YAML::Node &node, const char *key, T &out) {
  if (node && node.IsMap() && node[key]) out = node[key].as<T>();
}
uint32_t integer(const std::string &value) {
  require(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
          "expected an unsigned integer: " + value);
  auto n = std::stoull(value);
  require(n <= UINT32_MAX, "integer out of range"); return static_cast<uint32_t>(n);
}
uint64_t Config::rules_hash() const {
  // Hash logical extraction settings, never transport settings or local paths.
  std::string text = "ktag-tcp-rules-v1:" + std::to_string(feature_threads) + ":" +
    std::to_string(flow_hash_seed) + ":" + std::to_string(flow_capacity) + ":" +
    std::to_string(tcp_idle_us) + ":" + std::to_string(udp_idle_us) + ":" +
    std::to_string(udp_packet_limit) + ":" + std::to_string(expire_ms) + ":" +
    std::to_string(modern);
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
  return hash;
}
Config parse_config(int argc, char **argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--config") {
      require(i + 1 < argc, "--config needs a path"); c.config_path = argv[++i];
    }
  }
  if (!c.config_path.empty()) {
    const auto root = YAML::LoadFile(c.config_path);
    // Also accepts the unmodified UB YAML's pipeline feature/model settings.
    auto n = root["pipeline"] ? root["pipeline"] : root;
    auto section = [](const YAML::Node &node, const char *key) {
      return node[key] ? node[key] : YAML::Node(YAML::NodeType::Map);
    };
    auto f = section(n, "feature"), inf = section(n, "inference"), t = section(root, "transport");
    auto r = section(root, "replay"), cap = section(root, "capture"), z = section(root, "compression");
    if (n["plugins"]) require(n["plugins"].size() == 0, "first version supports legacy builtins only");
    const auto s = section(n, "schema");
    if (s["name"]) require(s["name"].as<std::string>() == "ktag_legacy_v1", "unsupported schema name");
    if (s["version"]) require(s["version"].as<unsigned>() == 1, "unsupported schema version");
    for (auto key : {"feature_order", "feature_ids", "remove_features", "add_features"})
      if (s[key]) require(s[key].size() == 0, "first version needs the exact legacy_v1 schema");
    read(f, "threads", c.feature_threads); read(f, "flow_hash_seed", c.flow_hash_seed);
    read(f, "queue_capacity", c.packet_queue); read(f, "packet_queue_capacity", c.packet_queue);
    read(f, "feature_queue_capacity", c.feature_queue); read(f, "flow_capacity", c.flow_capacity);
    read(f, "tcp_idle_timeout_us", c.tcp_idle_us); read(f, "udp_idle_timeout_us", c.udp_idle_us);
    read(f, "udp_packet_limit", c.udp_packet_limit); read(f, "expire_interval_ms", c.expire_ms);
    std::string lifecycle = "legacy"; read(f, "lifecycle_mode", lifecycle);
    require(lifecycle == "legacy" || lifecycle == "modern", "invalid lifecycle_mode");
    c.modern = lifecycle == "modern";
    read(inf, "batch_size", c.inference_records); read(inf, "batch_timeout_us", c.inference_timeout_us);
    read(inf, "queue_capacity", c.feature_queue);
    for (auto key : {"threads", "intra_op_threads", "inter_op_threads"})
      if (inf[key]) require(inf[key].as<unsigned>() == 1, "inference threads must be 1");
    read(inf, "model_package_path", c.model); read(n, "model_package_path", c.model);
    if (!c.model.empty() && std::filesystem::path(c.model).is_relative())
      c.model = (std::filesystem::absolute(c.config_path).parent_path() / c.model).lexically_normal().string();
    read(t, "bind_ip", c.local_ip); read(t, "port", c.port);
    read(t, "batch_records", c.compression_records); read(t, "batch_timeout_us", c.compression_timeout_us);
    read(t, "max_uncompressed_bytes", c.max_body); read(t, "progress_timeout_s", c.timeout_s);
    read(cap, "interface", c.capture_interface); read(cap, "snapshot_length", c.snapshot_length);
    read(cap, "live_buffer_bytes", c.capture_buffer);
    read(r, "interface", c.replay_interface); read(r, "program", c.replay_program);
    read(r, "cpus", c.replay_cpus);
    read(r, "source_mac", c.source_mac); read(r, "destination_mac", c.destination_mac);
    read(r, "rate_mode", c.rate_mode); read(r, "preload_pcap", c.preload);
    read(r, "drain_timeout_s", c.drain_timeout_s); read(r, "quiet_period_ms", c.quiet_ms);
    if (r["rate_mbps"] && !r["rate_mbps"].IsNull()) c.mbps = r["rate_mbps"].as<double>();
    if (r["loop"]) require(r["loop"].as<unsigned>() == 1, "each PCAP must be replayed exactly once");
    read(z, "level", c.level); read(z, "target_ratio", c.target_ratio);
    if (z["codec"]) require(z["codec"].as<std::string>() == "zstd", "codec must be zstd");
    auto pressure = section(n, "backpressure");
    if (pressure["policy"]) require(pressure["policy"].as<std::string>() == "block", "backpressure must be block");
  }
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--prepare-only") { c.prepare_only = true; continue; }
    require(i + 1 < argc, "missing value for " + a); std::string v = argv[++i];
    if (a == "--config") continue;
    else if (a == "--role") c.role = v;
    else if (a == "--bind-ip" || a == "--local-ip") c.local_ip = v;
    else if (a == "--connect") {
      auto p = v.rfind(':'); require(p != std::string::npos, "--connect expects IPv4:PORT");
      c.peer_ip = v.substr(0, p); c.port = integer(v.substr(p + 1));
    } else if (a == "--port") c.port = integer(v);
    else if (a == "--capture-interface") c.capture_interface = v;
    else if (a == "--replay-interface") c.replay_interface = v;
    else if (a == "--replay-cpus") c.replay_cpus = v;
    else if (a == "--model") c.model = v;
    else if (a == "--result") c.result = v;
    else if (a == "--stats") c.stats = v;
    else if (a == "--feature-audit") c.audit = v;
    else if (a == "-p") c.pcaps.push_back(v);
    else if (a == "--replay-mbps") { c.mbps = std::stod(v); c.rate_mode = "mbps"; }
    else throw std::runtime_error("unknown option: " + a);
  }
  require(c.role == "frontend" || c.role == "backend", "--role must be frontend or backend");
  require(c.port > 0 && c.port <= 65535, "invalid port");
  require(c.feature_threads > 0 && c.feature_threads <= 128, "feature threads must be in [1,128]");
  require(c.packet_queue && c.feature_queue && c.flow_capacity && c.udp_packet_limit,
          "queue, flow capacity and UDP limit must be positive");
  require(c.timeout_s && c.drain_timeout_s && c.quiet_ms, "timeouts must be positive");
  require(c.compression_records && c.inference_records && c.compression_timeout_us && c.inference_timeout_us,
          "batch sizes and timeouts must be positive");
  require(c.max_body >= ktag_demo_ub_feature_frame_size(KTAG_LEGACY_V1_VALUE_COUNT) &&
          c.max_body <= 4194304 && uint64_t(c.compression_records) *
          ktag_demo_ub_feature_frame_size(KTAG_LEGACY_V1_VALUE_COUNT) <= c.max_body,
          "compression batch exceeds body limit (maximum 4 MiB)");
  require(c.snapshot_length >= 64 && c.snapshot_length <= 65535 && c.capture_buffer <= INT32_MAX,
          "invalid live capture limits");
  require(c.level >= ZSTD_minCLevel() && c.level <= ZSTD_maxCLevel(), "invalid Zstd level");
  require(std::isfinite(c.target_ratio) && c.target_ratio > 0, "invalid target ratio");
  require(std::isfinite(c.mbps) && c.mbps >= 0, "invalid rate_mbps");
  require(c.rate_mode == "original" || (c.rate_mode == "mbps" && c.mbps > 0),
          "replay mode must be original or mbps with positive rate_mbps");
  require(c.replay_cpus.empty() || c.replay_cpus.find_first_not_of("0123456789,-") == std::string::npos,
          "invalid replay CPU list");
  if (c.rate_mode == "original") c.mbps = 0;
  if (c.role == "frontend") {
    require(!c.prepare_only && c.pcaps.empty(), "PCAPs/prepare-only belong on backend");
    require(!c.local_ip.empty() && !c.peer_ip.empty() && !c.capture_interface.empty(),
            "frontend needs --local-ip, --connect and --capture-interface");
  } else {
    require(!c.pcaps.empty(), "backend needs at least one -p PCAP");
    if (!c.prepare_only) require(!c.local_ip.empty() && !c.replay_interface.empty() &&
      !c.model.empty() && !c.result.empty(), "backend needs bind IP, replay interface, model and result");
  }
  std::vector<std::string> outputs;
  for (const auto &out : {c.result, c.stats, c.audit}) if (!out.empty()) outputs.push_back(out);
  auto protected_files = c.pcaps;
  if (!c.config_path.empty()) protected_files.push_back(c.config_path);
  for (size_t i = 0; i < outputs.size(); ++i) {
    auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(outputs[i]));
    for (const auto &input : protected_files)
      require(path != std::filesystem::weakly_canonical(std::filesystem::absolute(input)) &&
              !(std::filesystem::exists(path) && std::filesystem::exists(input) && std::filesystem::equivalent(path, input)),
              "output path must not overwrite a PCAP/config input");
    for (size_t j = 0; j < i; ++j)
      require(path != std::filesystem::weakly_canonical(std::filesystem::absolute(outputs[j])) &&
              !(std::filesystem::exists(path) && std::filesystem::exists(outputs[j]) &&
                std::filesystem::equivalent(path, outputs[j])),
              "result/stats/audit paths must be distinct");
    if (!c.model.empty()) {
      auto model = std::filesystem::weakly_canonical(std::filesystem::absolute(c.model));
      auto inside = path.lexically_relative(model);
      require(inside.empty() || *inside.begin() == "..", "output must not overwrite model package files");
    }
  }
  return c;
}
Schema make_schema() {
  Schema s(ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
  require(bool(s), "cannot create legacy_v1 schema"); return s;
}
} // namespace tcp_bench
