#include "common.h"
#include "replay_controller.h"
#include "tcp_transport.h"
#include <arpa/inet.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <time.h>

namespace tcp_bench {
struct Result { uint32_t source; ktag_prediction_record prediction; };
void run_backend(const Config &c, Metrics &metrics) {
  if (!c.prepare_only) check_interface(c.replay_interface, c.local_ip, c.source_mac, c.destination_mac);
  ReplayFiles replay(c);
  for (const auto &f : replay.files) metrics.expected_packets += f.packets;
  if (c.prepare_only) { std::cout << "PCAP preflight passed\n"; return; }
  auto schema = make_schema();
  metrics.schema_hash = ktag_feature_schema_get_hash(schema.get());
  ktag_inference_config ic{}; ic.struct_size = sizeof(ic); ic.api_version = KTAG_API_VERSION;
  ic.model_package_path = c.model.c_str(); ic.feature_schema = schema.get();
  ic.intra_op_threads = 1; ic.inter_op_threads = 1;
  Inference inference(ktag_inference_create(&ic), ktag_inference_destroy);
  require(bool(inference), "model load: "+std::string(ktag_inference_get_last_error()));
  FeatureBatch input(ktag_feature_batch_create(schema.get(), c.inference_records), ktag_feature_batch_destroy);
  Predictions output(ktag_prediction_batch_create(c.inference_records), ktag_prediction_batch_destroy);
  require(bool(input) && bool(output), "cannot allocate inference batches");
  // Open output paths before READY, so simple path errors cannot waste a run.
  std::ofstream result(c.result, std::ios::trunc);
  require(bool(result), "cannot open prediction output: "+c.result);
  std::ofstream audit;
  if (!c.audit.empty()) { audit.open(c.audit, std::ios::binary|std::ios::trunc); require(bool(audit), "cannot open backend audit"); }
  std::cout << "Backend model=" << ktag_inference_get_model_name(inference.get())
            << " listening " << c.local_ip << ':' << c.port << std::endl;
  Channel channel(accept_frontend(c), c.timeout_s, c.max_body, metrics);
  uint64_t run = 0;
  std::vector<Result> results;
  try {
    auto hello = channel.receive(); expect(hello, Type::hello, 0);
    Reader h(hello.body);
    require(h.u64() == ktag_feature_schema_get_hash(schema.get()) && h.u64() == c.rules_hash(),
            "frontend schema/extraction rules mismatch");
    require(h.u32() == ktag_feature_schema_get_value_count(schema.get()) && h.u32() == KTAG_DEMO_UB_FRAME_VERSION,
            "feature value count/wire version mismatch");
    require(h.u32() == uint32_t(c.level) && h.u32() == c.max_body, "compression contract mismatch"); h.finish();
    std::random_device random; run = now_ns() ^ (uint64_t(random()) << 32) ^ random(); if (!run) run = 1;
    Writer ready; ready.u32(uint32_t(replay.files.size())); ready.text(c.source_mac); ready.text(c.destination_mac);
    ready.text(c.rate_mode); ready.text(std::to_string(c.mbps)); ready.text(c.replay_cpus);
    channel.control(Type::ready, run, ready); Codec codec(c.level);
    metrics.initialization_ns = now_ns()-metrics.started;
    auto data_cpu_started = thread_cpu_ns();
    uint64_t next_record = 1, next_batch = 1, packets = 0, bytes = 0;
    const auto frame_size = ktag_demo_ub_feature_frame_size(KTAG_LEGACY_V1_VALUE_COUNT);
    for (const auto &file : replay.files) {
      Writer begin; begin.u32(file.source_index); begin.u64(file.packets); begin.text(file.original);
      channel.control(Type::source_begin, run, begin);
      auto capture_ready = channel.receive(); expect(capture_ready, Type::capture_ready, run);
      Reader r(capture_ready.body); require(r.u32() == file.source_index, "CAPTURE_READY source mismatch"); r.finish();
      std::atomic<bool> cancel{false}; std::exception_ptr replay_error;
      auto heartbeat = [&] {
        Writer progress; progress.u32(file.source_index); progress.u64(0);
        channel.control(Type::progress, run, progress);
      };
      std::thread generator([&] {
        auto cpu_started = thread_cpu_ns();
        try {
          replay_file(c, file, metrics, cancel, heartbeat);
          Writer end; end.u32(file.source_index); end.u64(file.packets);
          channel.control(Type::replay_done, run, end);
        } catch (...) { replay_error = std::current_exception(); channel.stop(); }
        metrics.replay_coord_cpu_ns += thread_cpu_ns()-cpu_started;
      });
      uint64_t source_records = 0; Clock::time_point first_input{};
      auto flush = [&] {
        auto count = ktag_feature_batch_get_count(input.get()); if (!count) return;
        auto cpu_start = thread_cpu_ns();
        auto start = now_ns(); auto status = ktag_inference_predict(inference.get(), input.get(), output.get());
        metrics.inference_ns += now_ns()-start; metrics.inference_completed = now_ns();
        metrics.inference_cpu_ns += thread_cpu_ns()-cpu_start;
        require(status == KTAG_SUCCESS, "inference: "+std::string(ktag_inference_get_last_error()));
        auto predicted = ktag_prediction_batch_get_count(output.get());
        require(predicted <= count, "model returned too many predictions");
        require(results.size()+predicted <= 1000000, "prediction buffer exceeds 1,000,000-record budget");
        for (uint32_t i = 0; i < predicted; ++i) {
          ktag_prediction_record p{};
          require(ktag_prediction_batch_get_record(output.get(), i, &p) == KTAG_SUCCESS &&
                  p.source_record_index < count, "invalid prediction record");
          results.push_back({file.source_index, p});
        }
        metrics.submitted += count; metrics.predictions += predicted; metrics.filtered += count-predicted;
        ktag_feature_batch_clear(input.get()); first_input = {};
      };
      try {
        auto last_progress = Clock::now(); bool ended = false;
        auto heartbeat_interval = std::chrono::nanoseconds(std::min<uint64_t>(1000000000ULL,
          uint64_t(c.timeout_s)*1000000000ULL/3));
        auto heartbeat_at = Clock::now()+heartbeat_interval;
        while (!ended) {
          if (Clock::now() >= heartbeat_at) { heartbeat(); heartbeat_at = Clock::now()+heartbeat_interval; }
          if (ktag_feature_batch_get_count(input.get()) &&
              Clock::now()-first_input >= std::chrono::microseconds(c.inference_timeout_us)) flush();
          Message m;
          if (!channel.receive_for(m, c.inference_timeout_us)) {
            require(Clock::now()-last_progress < std::chrono::seconds(c.timeout_s), "backend feature progress timeout");
            continue;
          }
          last_progress = Clock::now();
          if (m.type == Type::progress) {
            expect(m, Type::progress, run); Reader progress(m.body);
            require(progress.u32() == file.source_index && progress.u64() <= file.packets,
                    "invalid frontend progress report"); progress.finish();
          } else if (m.type == Type::features) {
            expect(m, Type::features, run); require(m.sequence == next_batch++, "batch sequence mismatch");
            require(uint64_t(m.count)*frame_size == m.raw_size, "feature count/body size mismatch");
            metrics.mark_first(now_ns()); auto started = now_ns(); auto raw = codec.decompress(m, c.max_body);
            metrics.decompress_ns += now_ns()-started;
            if (audit.is_open()) { audit.write(reinterpret_cast<const char *>(raw.data()), std::streamsize(raw.size()));
              require(bool(audit), "cannot write backend feature audit"); }
            for (uint32_t i = 0; i < m.count; ++i) {
              Record record; ktag_demo_ub_frame_info info; std::string error; started = now_ns();
              auto decoded_ok = ktag_demo_ub_decode_feature_frame(raw.data()+i*frame_size, frame_size,
                &info, &record, &error);
              require(decoded_ok, "feature decode: "+error);
              require(info.frame_size == frame_size && info.record_count == 1 &&
                info.value_count == KTAG_LEGACY_V1_VALUE_COUNT && info.flags == 0 &&
                info.contract.frontend_id_hash == 1 && info.contract.epoch == run &&
                info.contract.sequence == next_record++ &&
                info.contract.schema_hash == ktag_feature_schema_get_hash(schema.get()) &&
                info.contract.ruleset_hash == c.rules_hash() && record.source_index == file.source_index,
                "inner feature contract/sequence/source mismatch");
              metrics.decode_ns += now_ns()-started;
              if (!ktag_feature_batch_get_count(input.get())) first_input = Clock::now();
              require(ktag_feature_batch_append(input.get(), &record.details.metadata,
                record.values.data(), uint32_t(record.values.size())) == KTAG_SUCCESS, "cannot append inference record");
              ++source_records; ++metrics.records;
              if (ktag_feature_batch_get_count(input.get()) >= c.inference_records ||
                  Clock::now()-first_input >= std::chrono::microseconds(c.inference_timeout_us)) flush();
            }
            ++metrics.batches; metrics.raw_bytes += raw.size(); metrics.compressed_bytes += m.body.size();
          } else {
            expect(m, Type::source_end, run); Reader end(m.body);
            require(end.u32() == file.source_index, "SOURCE_END source mismatch");
            auto received = end.u64(), captured = end.u64(), features = end.u64(); end.finish();
            require(received == file.packets && features == source_records, "SOURCE_END input/feature count mismatch");
            packets += received; bytes += captured; flush(); ended = true;
          }
        }
        generator.join(); if (replay_error) std::rethrow_exception(replay_error);
      } catch (...) {
        cancel = true; channel.stop(); if (generator.joinable()) generator.join();
        if (replay_error) std::rethrow_exception(replay_error); throw;
      }
      Writer source_done; source_done.u32(file.source_index); channel.control(Type::source_done, run, source_done);
    }
    auto end = channel.receive(); expect(end, Type::end, run); Reader e(end.body);
    require(e.u64() == packets && e.u64() == bytes && e.u64() == metrics.records &&
      e.u64() == metrics.batches, "END totals mismatch"); e.finish();
    require(metrics.records == metrics.submitted && metrics.submitted == metrics.filtered+metrics.predictions,
            "backend feature/inference totals mismatch");
    metrics.packets = packets; metrics.bytes = bytes;
    Writer done; done.u64(metrics.records); done.u64(metrics.submitted); done.u64(metrics.filtered); done.u64(metrics.predictions);
    channel.control(Type::done, run, done); metrics.completed = now_ns();
    metrics.backend_data_cpu_ns = thread_cpu_ns()-data_cpu_started;
    auto started = now_ns();
    result << "source_index,flow_id,ip_version,src_ip,dst_ip,src_port,dst_port,protocol,first_packet_sequence,last_packet_sequence,label,confidence\n";
    auto address = [](const ktag_ip_address &a) {
      char buffer[64]{}; inet_ntop(a.version == 4 ? AF_INET : AF_INET6, a.bytes, buffer, sizeof(buffer));
      return std::string(buffer);
    };
    result << std::setprecision(9);
    for (const auto &r : results) {
      const auto &p = r.prediction; const auto &k = p.metadata.flow_key;
      result << r.source << ',' << p.metadata.flow_id << ',' << unsigned(k.address_a.version) << ','
        << address(k.address_a) << ',' << address(k.address_b) << ',' << k.port_a << ',' << k.port_b << ','
        << unsigned(k.protocol) << ',' << p.metadata.first_packet_sequence << ',' << p.metadata.last_packet_sequence
        << ',' << p.label << ',' << p.confidence << '\n';
    }
    result.flush(); require(bool(result), "prediction output write failed");
    if (audit.is_open()) { audit.flush(); require(bool(audit), "backend audit flush failed"); }
    metrics.output_ns = now_ns()-started;
  } catch (const std::exception &e) {
    try { Writer w; w.text(e.what()); channel.control(Type::error, run, w); } catch (...) {}
    channel.stop(); throw;
  }
}
} // namespace tcp_bench
