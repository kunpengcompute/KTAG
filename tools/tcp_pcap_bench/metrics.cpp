#include "common.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>
#include <zstd.h>

namespace tcp_bench {
uint64_t process_cpu_ns() {
  timespec t{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
uint64_t thread_cpu_ns() {
  timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
std::map<std::string, uint64_t> nic_snapshot(const std::string &iface) {
  std::map<std::string, uint64_t> result;
  for (auto key : {"rx_bytes", "tx_bytes", "rx_packets", "tx_packets", "rx_dropped", "tx_dropped", "rx_errors", "tx_errors"}) {
    std::ifstream f("/sys/class/net/"+iface+"/statistics/"+key); uint64_t value; if (f >> value) result[key] = value;
  } return result;
}
void Metrics::mark_first(uint64_t value) {
  auto old = first_feature.load(); while ((!old || value < old) && !first_feature.compare_exchange_weak(old, value)) {}
}
std::string json_quote(const std::string &text) {
  std::ostringstream out; out << '"';
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') out << '\\' << char(ch);
    else if (ch < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(ch) << std::dec;
    else out << char(ch);
  } out << '"'; return out.str();
}
namespace {
// Keep this presentation local to the TCP benchmark. No UB runtime or
// statistics implementation is needed to display comparable units.
void print_performance(const Config &c, const Metrics &m, bool success,
                       bool complete, uint64_t core, uint64_t pipeline,
                       uint64_t total_wall) {
  const bool frontend = c.role == "frontend";
  const auto input_bytes = m.bytes.load();
  std::ostringstream out;
  out << std::fixed << std::setprecision(4);
  auto throughput = [&](const char *label, uint64_t interval) {
    out << label << ": ";
    if (success && interval) {
      const double seconds = double(interval)/1e9;
      out << double(input_bytes)/1048576/seconds << " MiB/s ("
          << double(input_bytes)*8/1073741824/seconds << " Gibit/s)";
    } else out << "N/A";
    if (success && !complete) out << "（部分 PCAP 输入）";
    out << '\n';
  };
  throughput(frontend ? "核心处理吞吐量（TCP 压缩全链路）"
                      : "推理有效吞吐量（后端接收至推理）", core);
  if (core) out << "十进制换算: " << double(input_bytes)*8/core << " Gbit/s\n";
  out << "======================= 性能统计 =======================\n"
      << "运行状态: " << (success ? (complete ? "成功，完整输入" : "成功，部分 PCAP 输入") : "失败") << '\n'
      << "单位说明: MiB/s、Gibit/s 与 UB 旧版 MB/s、Gbps 的计算口径相同\n";
  auto seconds = [&](const char *label, uint64_t value) {
    out << label << ": " << double(value)/1e9 << " 秒\n";
  };
  if (frontend) {
    seconds("PCAP 读取累计耗时", m.read_ns.load());
    seconds("特征计算累计耗时（所有线程 API 调用之和）", m.feature_ns.load());
    seconds("特征编码累计耗时", m.encode_ns.load());
    seconds("压缩累计耗时", m.compress_ns.load());
    seconds("TCP 发送累计耗时（含等待）", m.send_ns.load());
    out << "特征 CPU 累计计算量: " << double(m.feature_cpu_ns.load())/1e9 << " CPU 秒\n";
    if (core) seconds("核心链路耗时（特征提取、压缩、传输、解压、等待及后端推理）", core);
    else out << "核心链路耗时: N/A\n";
    out << "核心计时范围: 首条报文开始特征处理 → 收到并验证后端完成确认\n";
  } else {
    seconds("解压累计耗时", m.decompress_ns.load());
    seconds("特征解码累计耗时（含校验）", m.decode_ns.load());
    seconds("推理计算累计耗时", m.inference_ns.load());
    if (core) seconds("推理阶段墙钟耗时（首批特征接收完到末次推理完成）", core);
    else out << "推理阶段墙钟耗时: N/A\n";
    seconds("结果文件 IO 耗时", m.output_ns.load());
    out << "后端阶段吞吐不能替代前端全链路核心吞吐\n";
  }
  seconds("端到端总耗时（进程启动至本次统计采样，含初始化及结果输出）", total_wall);
  if (pipeline) seconds(frontend ? "读取开始至后端完成确认耗时" : "首个文件就绪至完成确认耗时", pipeline);
  out << "处理包数: " << m.packets.load() << " 个\n"
      << "总数据量: " << double(input_bytes)/1048576 << " MiB（PCAP captured bytes）\n"
      << "阶段累计耗时可并行重叠，不能相加作为核心链路耗时\n";
  throughput("端到端吞吐量（进程启动至本次统计采样）", total_wall);
  out << "========================================================\n"
      << "======================= 管线统计 =======================\n"
      << (frontend ? "特征提取流数: " : "接收特征记录数: ") << m.records.load() << " 个\n"
      << "推理输入流数: " << m.submitted.load() << " 个\n"
      << "过滤统计: 保留 " << m.predictions.load() << " 个样本，过滤掉 " << m.filtered.load() << " 个样本\n"
      << "不支持数据包: " << m.unsupported.load() << " 个\n"
      << "特征处理拒绝数据包: " << m.rejected.load() << " 个\n"
      << "Packet Pool 等待: " << m.pool_waits.load() << " 次\n"
      << "传输特征批数: " << m.batches.load() << " 批\n"
      << "压缩前特征字节数: " << m.raw_bytes.load() << " bytes\n"
      << "压缩后特征字节数: " << m.compressed_bytes.load() << " bytes\n"
      << "实际压缩比: ";
  if (m.compressed_bytes) out << double(m.raw_bytes.load())/m.compressed_bytes.load() << ":1\n";
  else out << "N/A\n";
  out << "========================================================\n";
  std::cout << out.str();
}
} // namespace
void Metrics::write(const Config &c, bool success, const std::string &error) const {
  bool complete = success && sources.size() == (c.role == "frontend" ? c.pcaps.size() : sources.size()) && !sources.empty();
  for (const auto &source : sources) complete &= source.warning.empty();
  auto first = first_feature.load(), last = c.role == "frontend" ? completed.load() : inference_completed.load();
  uint64_t core = success && first && last >= first ? last-first : 0;
  uint64_t pipeline = success && first_read && completed >= first_read ? completed-first_read : 0;
  const auto input_bytes = bytes.load();
  const double core_gbit_s = core ? double(input_bytes)*8/core : 0;
  const auto total_wall = now_ns()-started;
  std::ostringstream json; json << std::setprecision(12);
  json << "{\n  \"core_throughput_gbit_s\": ";
  if (c.role == "frontend" && core) json << core_gbit_s; else json << "null";
  json << ",\n  \"status\": " << json_quote(success ? "complete" : "failed") << ",\n  \"error\": " << json_quote(error)
    << ",\n  \"role\": " << json_quote(c.role) << ",\n  \"input_complete\": " << (complete ? "true" : "false")
    << ",\n  \"input_mode\": \"frontend_local_pcap\",\n  \"timestamp_mode\": \"pcap_microseconds\""
    << ",\n  \"read_mode\": \"as_fast_as_possible\",\n  \"core_timer\": "
    << json_quote(c.role == "frontend" ? "first_feature_process_to_DONE" : "first_feature_batch_received_to_last_inference_complete")
    << ",\n  \"pipeline_timer\": " << json_quote(c.role == "frontend" ? "reader_start_to_DONE" : "first_FILE_READY_to_DONE")
    << ",\n  \"compression_level\": " << c.level << ",\n  \"target_ratio\": " << c.target_ratio
    << ",\n  \"schema\": \"ktag_legacy_v1\",\n  \"schema_hash\": " << schema_hash
    << ",\n  \"extraction_rules_hash\": " << c.rules_hash();
  utsname host{}; uname(&host);
  json << ",\n  \"hostname\": " << json_quote(host.nodename) << ",\n  \"architecture\": " << json_quote(host.machine)
    << ",\n  \"kernel\": " << json_quote(host.release) << ",\n  \"zstd_version\": " << json_quote(ZSTD_versionString())
    << ",\n  \"audit_enabled\": " << (c.audit.empty() ? "false" : "true") << ",\n  \"feature_threads\": " << c.feature_threads
    << ",\n  \"packet_pool_size\": " << c.packet_pool << ",\n  \"packet_queue_capacity_per_worker\": " << c.packet_queue
    << ",\n  \"feature_queue_capacity\": " << c.feature_queue << ",\n  \"compression_batch_records\": " << c.compression_records
    << ",\n  \"inference_batch_records\": " << c.inference_records << ",\n  \"prediction_limit\": " << c.prediction_limit;
  auto field = [&](const char *name, uint64_t value) { json << ",\n  " << json_quote(name) << ": " << value; };
#define COUNTER(name) field(#name, name.load())
  COUNTER(packets); COUNTER(bytes); COUNTER(original_bytes); COUNTER(short_captures);
  COUNTER(unsupported); COUNTER(processed); COUNTER(rejected); COUNTER(extracted);
  COUNTER(records); COUNTER(submitted); COUNTER(filtered); COUNTER(predictions);
  COUNTER(batches); COUNTER(raw_bytes); COUNTER(compressed_bytes); COUNTER(socket_tx); COUNTER(socket_rx);
  COUNTER(read_ns); COUNTER(pool_waits); COUNTER(pool_wait_ns); COUNTER(queue_wait_ns);
  COUNTER(feature_ns); COUNTER(encode_ns); COUNTER(compress_ns); COUNTER(send_ns);
  COUNTER(decompress_ns); COUNTER(decode_ns); COUNTER(inference_ns); COUNTER(output_ns); COUNTER(initialization_ns);
  COUNTER(inference_cpu_ns); COUNTER(feature_cpu_ns); COUNTER(read_cpu_ns); COUNTER(output_cpu_ns); COUNTER(backend_data_cpu_ns);
#undef COUNTER
  field("core_ns", core); field("pipeline_ns", pipeline); field("total_wall_ns", total_wall);
  field("process_cpu_ns", process_cpu_ns()-process_cpu_started); field("pure_float32_bytes", records.load()*KTAG_LEGACY_V1_VALUE_COUNT*4);
  rusage usage{}; getrusage(RUSAGE_SELF, &usage); field("peak_rss_kib", uint64_t(usage.ru_maxrss));
  auto number = [&](const char *name, bool valid, double value) {
    json << ",\n  " << json_quote(name) << ": "; if (valid) json << value; else json << "null";
  };
  number("input_gbit_s", core != 0, core_gbit_s);
  number("input_mb_s", core != 0, core ? double(bytes.load())*1000/core : 0);
  number("input_mib_s", core != 0, core ? double(bytes.load())*1e9/1048576/core : 0);
  number("input_gibit_s_ub_legacy", core != 0, core ? double(bytes.load())*8*1e9/1073741824/core : 0);
  number("records_s", core != 0, core ? double(submitted.load())*1e9/core : 0);
  number("pipeline_input_gbit_s", pipeline != 0, pipeline ? double(bytes.load())*8/pipeline : 0);
  number("compression_ratio", compressed_bytes != 0, compressed_bytes ? double(raw_bytes.load())/compressed_bytes.load() : 0);
  auto socket_bytes = socket_tx.load()+socket_rx.load();
  number("compression_ratio_socket", socket_bytes != 0, socket_bytes ? double(raw_bytes.load())/socket_bytes : 0);
  json << ",\n  \"target_ratio_met\": ";
  if (compressed_bytes) json << (double(raw_bytes.load())/compressed_bytes.load() >= c.target_ratio ? "true" : "false"); else json << "null";
  json << ",\n  \"replay_actual_mbps\": null,\n  \"replay_cpu_ns\": null,\n  \"capture_drops\": null"
    << ",\n  \"nic_interface\": " << json_quote(nic_interface)
    << ",\n  \"nic_counter_scope\": \"whole_run_all_interface_traffic\"";
  auto nic_end = nic_snapshot(nic_interface);
  for (auto key : {"rx_bytes", "tx_bytes", "rx_packets", "tx_packets", "rx_dropped", "tx_dropped", "rx_errors", "tx_errors"}) {
    auto begin = nic_begin.find(key); auto end = nic_end.find(key);
    json << ",\n  " << json_quote(std::string("nic_")+key) << ": ";
    if (begin != nic_begin.end() && end != nic_end.end() && end->second >= begin->second) json << end->second-begin->second;
    else json << "null";
  }
  for (auto key : {"mtu", "speed"}) {
    std::ifstream f("/sys/class/net/"+nic_interface+"/"+key); int64_t value = -1; f >> value;
    number((std::string("nic_")+key).c_str(), value >= 0, double(value));
  }
  json << ",\n  \"warnings\": ["; bool separator = false;
  for (const auto &s : sources) if (!s.warning.empty()) { if (separator) json << ','; json << json_quote(s.name+": "+s.warning); separator = true; }
  json << "],\n  \"sources\": ["; separator = false;
  for (const auto &s : sources) {
    if (separator) json << ',';
    separator = true;
    json << "\n    {\"source_index\":" << s.index << ",\"name\":" << json_quote(s.name) << ",\"link_type\":" << s.link_type
      << ",\"input_complete\":" << (s.warning.empty() ? "true" : "false") << ",\"warning\":" << json_quote(s.warning)
      << ",\"packets\":" << s.packets << ",\"bytes\":" << s.bytes << ",\"original_bytes\":" << s.original_bytes
      << ",\"short_captures\":" << s.short_captures << ",\"unsupported\":" << s.unsupported << ",\"processed\":" << s.processed
      << ",\"rejected\":" << s.rejected << ",\"records\":" << s.records << ",\"pool_waits\":" << s.pool_waits
      << ",\"submitted\":" << s.submitted << ",\"filtered\":" << s.filtered << ",\"predictions\":" << s.predictions
      << ",\"elapsed_ns\":" << s.elapsed_ns << '}';
  }
  json << "\n  ]\n}\n";
  if (!complete && success) std::cerr << "WARNING: statistics describe partial PCAP input, not the complete dataset\n";
  if (!c.stats.empty()) { std::ofstream file(c.stats, std::ios::trunc); require(bool(file), "cannot open stats output: "+c.stats);
    file << json.str(); file.flush(); require(bool(file), "stats output write failed"); }
  print_performance(c, *this, success, complete, core, pipeline, total_wall);
  std::cout << json.str();
}
} // namespace tcp_bench
