#include "common.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <time.h>
#include <sys/utsname.h>
#include <zstd.h>

namespace tcp_bench {
uint64_t process_cpu_ns() {
  timespec t{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
uint64_t thread_cpu_ns() {
  timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
std::map<std::string, uint64_t> nic_snapshot(const std::string &iface) {
  std::map<std::string, uint64_t> result;
  for (auto name : {"rx_bytes", "tx_bytes", "rx_packets", "tx_packets", "rx_dropped", "tx_dropped", "rx_errors", "tx_errors"}) {
    std::ifstream f("/sys/class/net/"+iface+"/statistics/"+name); uint64_t n;
    if (f >> n) result[name] = n;
  }
  return result;
}
void Metrics::mark_first(uint64_t value) {
  auto old = first_feature.load();
  while ((!old || value < old) && !first_feature.compare_exchange_weak(old, value)) {}
}
std::string json_quote(const std::string &s) {
  std::ostringstream out; out << '"';
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') out << '\\' << char(ch);
    else if (ch < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(ch) << std::dec;
    else out << char(ch);
  }
  out << '"'; return out.str();
}
void Metrics::write(const Config &c, bool success, const std::string &error) const {
  std::ostringstream json; json << std::setprecision(12);
  auto first = first_feature.load();
  uint64_t last = c.role == "frontend" ? completed.load() : inference_completed.load();
  uint64_t core = success && first && last >= first ? last-first : 0;
  double seconds = double(core)/1e9;
  json << "{\n  \"status\": " << json_quote(success ? "complete" : "failed")
    << ",\n  \"error\": " << json_quote(error) << ",\n  \"role\": " << json_quote(c.role)
    << ",\n  \"input_mode\": \"backend_pcap_replay_frontend_live_capture\""
    << ",\n  \"core_timer\": " << json_quote(c.role == "frontend" ? "first_feature_process_to_DONE" : "first_feature_batch_received_to_last_inference_complete")
    << ",\n  \"rate_mode\": " << json_quote(actual_rate_mode.empty() ? c.rate_mode : actual_rate_mode)
    << ",\n  \"configured_replay_mbps\": " << (actual_rate_mode.empty() ? c.mbps : actual_rate_mbps)
    << ",\n  \"compression_level\": " << c.level << ",\n  \"target_ratio\": " << c.target_ratio
    << ",\n  \"schema\": \"ktag_legacy_v1\",\n  \"extraction_rules_hash\": " << c.rules_hash();
  utsname host{}; uname(&host);
  json << ",\n  \"hostname\": " << json_quote(host.nodename) << ",\n  \"architecture\": " << json_quote(host.machine)
    << ",\n  \"kernel\": " << json_quote(host.release) << ",\n  \"zstd_version\": " << json_quote(ZSTD_versionString())
    << ",\n  \"audit_enabled\": " << (c.audit.empty() ? "false" : "true")
    << ",\n  \"replay_cpus\": " << json_quote(actual_rate_mode.empty() ? c.replay_cpus : replay_affinity)
    << ",\n  \"feature_threads\": " << c.feature_threads
    << ",\n  \"packet_queue_capacity_per_worker\": " << c.packet_queue
    << ",\n  \"feature_queue_capacity\": " << c.feature_queue
    << ",\n  \"compression_batch_records\": " << c.compression_records
    << ",\n  \"inference_batch_records\": " << c.inference_records;
  auto field = [&](const char *name, uint64_t value) { json << ",\n  " << json_quote(name) << ": " << value; };
  field("schema_hash", schema_hash);
#define COUNTER(name) field(#name, name.load())
  COUNTER(expected_packets); COUNTER(sent_packets); COUNTER(packets); COUNTER(bytes);
  COUNTER(unsupported); COUNTER(processed); COUNTER(rejected); COUNTER(extracted);
  COUNTER(records); COUNTER(submitted); COUNTER(filtered); COUNTER(predictions);
  COUNTER(batches); COUNTER(raw_bytes); COUNTER(compressed_bytes); COUNTER(socket_tx); COUNTER(socket_rx);
  COUNTER(capture_drops); COUNTER(feature_ns); COUNTER(encode_ns); COUNTER(compress_ns); COUNTER(send_ns);
  COUNTER(decompress_ns); COUNTER(decode_ns); COUNTER(inference_ns); COUNTER(queue_wait_ns); COUNTER(capture_ns);
  COUNTER(replay_ns); COUNTER(replay_bytes); COUNTER(replay_cpu_ns); COUNTER(output_ns); COUNTER(initialization_ns);
  COUNTER(inference_cpu_ns); COUNTER(feature_cpu_ns); COUNTER(capture_cpu_ns); COUNTER(output_cpu_ns);
  COUNTER(backend_data_cpu_ns); COUNTER(replay_coord_cpu_ns);
#undef COUNTER
  field("total_wall_ns", now_ns()-started); field("process_cpu_ns", process_cpu_ns()-process_cpu_started);
  rusage usage{}; getrusage(RUSAGE_SELF, &usage); field("peak_rss_kib", uint64_t(usage.ru_maxrss));
  field("pure_float32_bytes", records.load()*KTAG_LEGACY_V1_VALUE_COUNT*4);
  field("core_ns", core);
  field("capture_span_ns", last_capture >= first_capture && first_capture ? last_capture-first_capture : 0);
  auto number = [&](const char *name, bool valid, double value) {
    json << ",\n  " << json_quote(name) << ": "; if (valid) json << value; else json << "null";
  };
  number("input_gbit_s", core != 0, core ? double(bytes.load())*8/1e9/seconds : 0);
  number("input_mb_s", core != 0, core ? double(bytes.load())/1e6/seconds : 0);
  number("input_mib_s", core != 0, core ? double(bytes.load())/1048576/seconds : 0);
  number("input_gibit_s_ub_legacy", core != 0, core ? double(bytes.load())*8/1073741824/seconds : 0);
  number("records_s", core != 0, core ? double(submitted.load())/seconds : 0);
  number("compression_ratio", compressed_bytes != 0, compressed_bytes ? double(raw_bytes.load())/compressed_bytes.load() : 0);
  number("compression_ratio_socket", socket_tx+socket_rx != 0,
         socket_tx+socket_rx ? double(raw_bytes.load())/double(socket_tx+socket_rx) : 0);
  json << ",\n  \"target_ratio_met\": ";
  if (compressed_bytes) json << (double(raw_bytes.load())/compressed_bytes.load() >= c.target_ratio ? "true" : "false");
  else json << "null";
  number("replay_actual_mbps", replay_ns != 0, replay_ns ? double(replay_bytes.load())*8*1000/replay_ns.load() : 0);
  auto iface = c.role == "frontend" ? c.capture_interface : c.replay_interface;
  auto nic_end = nic_snapshot(iface);
  json << ",\n  \"nic_interface\": " << json_quote(iface)
    << ",\n  \"nic_counter_scope\": \"whole_run_all_interface_traffic_including_replay_and_TCP\"";
  for (auto key : {"rx_bytes", "tx_bytes", "rx_packets", "tx_packets", "rx_dropped", "tx_dropped", "rx_errors", "tx_errors"}) {
    auto first = nic_begin.find(key); auto last = nic_end.find(key);
    json << ",\n  " << json_quote(std::string("nic_")+key) << ": ";
    if (first != nic_begin.end() && last != nic_end.end() && last->second >= first->second) json << last->second-first->second;
    else json << "null";
  }
  for (auto key : {"mtu", "speed"}) {
    std::ifstream f("/sys/class/net/"+iface+"/"+key); int64_t value = -1; f >> value;
    number((std::string("nic_")+key).c_str(), value >= 0, double(value));
  }
  json << "\n}\n";
  if (!c.stats.empty()) {
    std::ofstream file(c.stats, std::ios::trunc); require(bool(file), "cannot open stats output: "+c.stats);
    file << json.str(); file.flush(); require(bool(file), "stats output write failed");
  }
  std::cout << json.str();
}
} // namespace tcp_bench
