#include "common.h"
#include "file_summary.h"
#include "tcp_transport.h"
#include <arpa/inet.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>

namespace tcp_bench {
struct Result { uint32_t source; ktag_prediction_record prediction; };
void run_backend(const Config &c, Metrics &metrics) {
  auto schema = make_schema(); metrics.schema_hash = ktag_feature_schema_get_hash(schema.get());
  ktag_inference_config ic{}; ic.struct_size = sizeof(ic); ic.api_version = KTAG_API_VERSION;
  ic.model_package_path = c.model.c_str(); ic.feature_schema = schema.get(); ic.intra_op_threads = 1; ic.inter_op_threads = 1;
  Inference inference(ktag_inference_create(&ic), ktag_inference_destroy);
  require(bool(inference), "model load: "+std::string(ktag_inference_get_last_error()));
  FeatureBatch input(ktag_feature_batch_create(schema.get(), c.inference_records), ktag_feature_batch_destroy);
  Predictions output(ktag_prediction_batch_create(c.inference_records), ktag_prediction_batch_destroy);
  require(bool(input) && bool(output), "cannot allocate inference batches");
  std::ofstream result(c.result, std::ios::trunc); require(bool(result), "cannot open prediction output: "+c.result);
  std::ofstream audit;
  if (!c.audit.empty()) { audit.open(c.audit, std::ios::binary|std::ios::trunc); require(bool(audit), "cannot open backend audit"); }
  std::cout << "Local-PCAP backend model=" << ktag_inference_get_model_name(inference.get())
            << " listening " << c.local_ip << ':' << c.port << std::endl;
  Channel channel(accept_frontend(c), c.timeout_s, c.max_body, metrics); uint64_t run = 0;
  std::vector<Result> results;
  try {
    auto hello = channel.receive(); expect(hello, Type::hello, 0); Reader h(hello.body);
    require(h.u64() == metrics.schema_hash && h.u64() == c.rules_hash(), "frontend schema/extraction rules mismatch");
    require(h.u32() == KTAG_LEGACY_V1_VALUE_COUNT && h.u32() == KTAG_DEMO_UB_FRAME_VERSION, "feature/wire contract mismatch");
    require(h.u32() == uint32_t(c.level) && h.u32() == c.max_body, "compression contract mismatch");
    auto file_count = h.u32(); h.finish(); require(file_count && file_count <= 100000, "invalid source count");
    std::random_device random; run = now_ns() ^ (uint64_t(random()) << 32) ^ random(); if (!run) run = 1;
    channel.control(Type::ready, run); Codec codec(c.level); metrics.initialization_ns = now_ns()-metrics.started;
    auto data_cpu_started = thread_cpu_ns(); uint64_t next_record = 1, next_batch = 1;
    const auto frame_size = ktag_demo_ub_feature_frame_size(KTAG_LEGACY_V1_VALUE_COUNT);
    for (uint32_t index = 0; index < file_count; ++index) {
      auto begin = channel.receive(); expect(begin, Type::file_begin, run); Reader b(begin.body);
      require(b.u32() == index, "FILE_BEGIN index mismatch"); auto name = b.text(); b.finish();
      require(!name.empty(), "empty source name"); auto file_started = now_ns();
      Writer ready; ready.u32(index); channel.control(Type::file_ready, run, ready);
      if (!index) metrics.first_read = now_ns();
      uint64_t source_records = 0, submitted_before = metrics.submitted, filtered_before = metrics.filtered;
      uint64_t predictions_before = metrics.predictions, reported_packets = 0;
      Clock::time_point first_input{};
      auto flush = [&] {
        auto count = ktag_feature_batch_get_count(input.get()); if (!count) return;
        auto cpu = thread_cpu_ns(), started = now_ns();
        auto status = ktag_inference_predict(inference.get(), input.get(), output.get());
        metrics.inference_ns += now_ns()-started; metrics.inference_completed = now_ns();
        metrics.inference_cpu_ns += thread_cpu_ns()-cpu;
        require(status == KTAG_SUCCESS, "inference: "+std::string(ktag_inference_get_last_error()));
        auto predicted = ktag_prediction_batch_get_count(output.get()); require(predicted <= count, "too many predictions");
        require(results.size()+predicted <= c.prediction_limit, "prediction buffer exceeds configured prediction_limit");
        for (uint32_t i = 0; i < predicted; ++i) {
          ktag_prediction_record p{};
          require(ktag_prediction_batch_get_record(output.get(), i, &p) == KTAG_SUCCESS && p.source_record_index < count,
                  "invalid prediction record"); results.push_back({index, p});
        }
        metrics.submitted += count; metrics.predictions += predicted; metrics.filtered += count-predicted;
        ktag_feature_batch_clear(input.get()); first_input = {};
      };
      auto heartbeat_interval = std::chrono::nanoseconds(std::min<uint64_t>(1000000000ULL, uint64_t(c.timeout_s)*1000000000ULL/3));
      auto heartbeat_at = Clock::now()+heartbeat_interval, last_progress = Clock::now();
      bool ended = false;
      while (!ended) {
        if (Clock::now() >= heartbeat_at) {
          Writer progress; progress.u32(index); progress.u64(0); channel.control(Type::progress, run, progress);
          heartbeat_at = Clock::now()+heartbeat_interval;
        }
        if (ktag_feature_batch_get_count(input.get()) && Clock::now()-first_input >= std::chrono::microseconds(c.inference_timeout_us)) flush();
        Message m;
        if (!channel.receive_for(m, c.inference_timeout_us)) {
          require(Clock::now()-last_progress < std::chrono::seconds(c.timeout_s), "backend feature progress timeout"); continue;
        }
        last_progress = Clock::now();
        if (m.type == Type::progress) {
          expect(m, Type::progress, run); Reader p(m.body); require(p.u32() == index, "PROGRESS index mismatch");
          auto seen = p.u64(); p.finish(); require(seen >= reported_packets, "packet progress regressed"); reported_packets = seen;
        } else if (m.type == Type::features) {
          expect(m, Type::features, run); require(m.sequence == next_batch++, "batch sequence mismatch");
          require(uint64_t(m.count)*frame_size == m.raw_size, "feature count/body length mismatch"); metrics.mark_first(now_ns());
          auto started = now_ns(); auto raw = codec.decompress(m, c.max_body); metrics.decompress_ns += now_ns()-started;
          if (audit.is_open()) { audit.write(reinterpret_cast<const char *>(raw.data()), std::streamsize(raw.size())); require(bool(audit), "backend audit write failed"); }
          for (uint32_t i = 0; i < m.count; ++i) {
            Record record; ktag_demo_ub_frame_info info; std::string error; started = now_ns();
            auto ok = ktag_demo_ub_decode_feature_frame(raw.data()+i*frame_size, frame_size, &info, &record, &error);
            require(ok, "feature decode: "+error);
            require(info.frame_size == frame_size && info.record_count == 1 && info.value_count == KTAG_LEGACY_V1_VALUE_COUNT &&
              info.flags == 0 && info.contract.frontend_id_hash == 1 && info.contract.epoch == run &&
              info.contract.sequence == next_record++ && info.contract.schema_hash == metrics.schema_hash &&
              info.contract.ruleset_hash == c.rules_hash() && record.source_index == index, "inner feature contract mismatch");
            metrics.decode_ns += now_ns()-started;
            if (!ktag_feature_batch_get_count(input.get())) first_input = Clock::now();
            require(ktag_feature_batch_append(input.get(), &record.details.metadata, record.values.data(), uint32_t(record.values.size())) == KTAG_SUCCESS,
                    "cannot append inference input");
            ++source_records; ++metrics.records;
            if (ktag_feature_batch_get_count(input.get()) >= c.inference_records ||
                Clock::now()-first_input >= std::chrono::microseconds(c.inference_timeout_us)) flush();
          }
          ++metrics.batches; metrics.raw_bytes += raw.size(); metrics.compressed_bytes += m.body.size();
        } else {
          expect(m, Type::file_end, run); Reader e(m.body); auto stats = read_source(e); e.finish(); stats.name = name;
          require(stats.index == index && stats.records == source_records && stats.packets >= reported_packets,
                  "FILE_END counters/index mismatch");
          require(stats.warning.empty() || (stats.packets && stats.warning.find("truncated") != std::string::npos), "invalid partial-input warning");
          flush(); stats.submitted = metrics.submitted.load()-submitted_before; stats.filtered = metrics.filtered.load()-filtered_before;
          stats.predictions = metrics.predictions.load()-predictions_before; stats.elapsed_ns = now_ns()-file_started;
          require(stats.records == stats.submitted && stats.submitted == stats.filtered+stats.predictions, "file inference counts do not close");
          metrics.packets += stats.packets; metrics.bytes += stats.bytes; metrics.original_bytes += stats.original_bytes;
          metrics.short_captures += stats.short_captures; metrics.unsupported += stats.unsupported;
          metrics.processed += stats.processed; metrics.rejected += stats.rejected;
          if (!stats.warning.empty()) std::cerr << "WARNING: truncated PCAP; partial results retained: " << name << " (" << stats.warning << ")\n";
          metrics.sources.push_back(stats);
          Writer done; done.u32(index); done.u64(source_records); done.u64(stats.submitted); done.u64(stats.filtered);
          done.u64(stats.predictions); done.u32(uint32_t(stats.warning.empty())); channel.control(Type::file_done, run, done); ended = true;
        }
      }
    }
    auto end = channel.receive(); expect(end, Type::end, run); Reader e(end.body);
    require(e.u64() == metrics.packets && e.u64() == metrics.bytes && e.u64() == metrics.records && e.u64() == metrics.batches,
            "END total mismatch"); e.finish();
    require(metrics.records == metrics.submitted && metrics.submitted == metrics.filtered+metrics.predictions, "global inference counts do not close");
    Writer done; done.u64(metrics.records); done.u64(metrics.submitted); done.u64(metrics.filtered); done.u64(metrics.predictions);
    channel.control(Type::done, run, done); metrics.completed = now_ns(); metrics.backend_data_cpu_ns = thread_cpu_ns()-data_cpu_started;
    auto started = now_ns();
    result << "source_index,flow_id,ip_version,src_ip,dst_ip,src_port,dst_port,protocol,first_packet_sequence,last_packet_sequence,label,confidence\n";
    auto address = [](const ktag_ip_address &a) {
      char buffer[64]{}; require(inet_ntop(a.version == 4 ? AF_INET : AF_INET6, a.bytes, buffer, sizeof(buffer)) != nullptr, "invalid prediction address"); return std::string(buffer);
    };
    result << std::setprecision(9);
    for (const auto &r : results) {
      const auto &p = r.prediction; const auto &k = p.metadata.flow_key;
      result << r.source << ',' << p.metadata.flow_id << ',' << unsigned(k.address_a.version) << ',' << address(k.address_a)
        << ',' << address(k.address_b) << ',' << k.port_a << ',' << k.port_b << ',' << unsigned(k.protocol) << ','
        << p.metadata.first_packet_sequence << ',' << p.metadata.last_packet_sequence << ',' << p.label << ',' << p.confidence << '\n';
    }
    result.flush(); require(bool(result), "prediction output write failed");
    if (audit.is_open()) { audit.flush(); require(bool(audit), "backend audit flush failed"); }
    metrics.output_ns = now_ns()-started;
  } catch (const std::exception &e) {
    try { Writer w; w.text(std::string(e.what()).substr(0, 4096)); channel.control(Type::error, run, w); } catch (...) {}
    channel.stop(); throw;
  }
}
} // namespace tcp_bench
