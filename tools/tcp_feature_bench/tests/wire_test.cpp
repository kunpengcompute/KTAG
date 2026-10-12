#include "protocol.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <future>
#include <thread>

using namespace tcp_bench;
template<class F> void rejects(F function) {
  bool rejected = false; try { function(); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "invalid input was accepted");
}
int main() {
  try {
    Bytes raw; std::mt19937 random(42); auto frame_size = ktag_demo_ub_feature_frame_size(354);
    for (uint32_t i = 0; i < 256; ++i) {
      Record r; r.source_index = 2;
      r.details.metadata.flow_id = i;
      r.details.metadata.first_packet_sequence = i*10+1;
      r.details.metadata.last_packet_sequence = i*10+5;
      r.details.metadata.flow_key.address_a.version = 4;
      r.details.metadata.flow_key.address_b.version = 4;
      r.details.metadata.flow_key.address_a.bytes[0] = 10;
      r.details.metadata.flow_key.address_b.bytes[0] = 11;
      r.details.source_address = r.details.metadata.flow_key.address_a;
      r.details.destination_address = r.details.metadata.flow_key.address_b;
      r.values.resize(354);
      // Includes signed zero, infinities, subnormals and NaN payloads. No float
      // arithmetic is allowed to canonicalize these bits during transport.
      const uint32_t special[] = {0, 0x80000000, 0x7f800000, 0xff800000, 1, 0x7fc01234, 0x7fa12345};
      for (size_t j = 0; j < r.values.size(); ++j) {
        uint32_t bits = j < 7 ? special[j] : uint32_t(random()); std::memcpy(&r.values[j], &bits, 4);
      }
      ktag_demo_ub_frame_contract contract{}; contract.sequence = i+1; contract.schema_hash = 10;
      size_t count = 0, offset = raw.size(); raw.resize(offset+frame_size); std::string error;
      require(ktag_demo_ub_encode_feature_frame(raw.data()+offset, frame_size, contract, r, false, &count, &error), error);
      Record restored; ktag_demo_ub_frame_info info;
      require(ktag_demo_ub_decode_feature_frame(raw.data()+offset, frame_size, &info, &restored, &error), error);
      require(count == frame_size && info.contract.sequence == i+1 && restored.source_index == 2 &&
        restored.details.metadata.last_packet_sequence == r.details.metadata.last_packet_sequence &&
        std::memcmp(restored.values.data(), r.values.data(), 354*4) == 0, "feature bits/metadata changed");
    }
    Codec codec(1); Message m; m.type = Type::features; m.run = 12; m.sequence = 1;
    m.count = 256; m.raw_size = uint32_t(raw.size()); m.body = codec.compress(raw);
    require(codec.decompress(m, 4194304) == raw, "Zstd round trip differs");
    auto header = encode_header(m); uint32_t body_size = 0;
    auto decoded = decode_header(header, 4194304, body_size);
    require(decoded.run == 12 && decoded.count == 256 && decoded.raw_size == raw.size() &&
            body_size == m.body.size(), "header round trip differs");
    auto broken = m; broken.body.back() ^= 1; rejects([&] { codec.decompress(broken, 4194304); });
    broken = m; broken.body.push_back(0); rejects([&] { codec.decompress(broken, 4194304); });
    broken = m; ++broken.raw_size; rejects([&] { codec.decompress(broken, 4194304); });
    rejects([&] { codec.decompress(m, 100); });
    header[0] ^= 1; rejects([&] { decode_header(header, 4194304, body_size); });
    header = encode_header(m); header.back() = 1; rejects([&] { decode_header(header, 4194304, body_size); });
    rejects([&] { decode_header(Bytes(2), 4194304, body_size); });
    Message oversized; oversized.type = Type::features; oversized.count = 1;
    oversized.raw_size = 4194305; oversized.body = {1};
    rejects([&] { decode_header(encode_header(oversized), 4194304, body_size); });
    Message control; control.type = Type::end; control.count = 1;
    rejects([&] { decode_header(encode_header(control), 4194304, body_size); });
    Writer w; w.u64(UINT64_MAX); w.text("test\ncontrol"); Reader reader(w.data);
    require(reader.u64() == UINT64_MAX && reader.text() == "test\ncontrol", "control round trip differs"); reader.finish();
    rejects([&] { Bytes empty; Reader reader(empty); reader.u32(); });
    // Internal CRC must reject payload mutation even before model inference.
    raw[KTAG_DEMO_UB_FRAME_HEADER_SIZE+30] ^= 1;
    Record r; std::string error;
    require(!ktag_demo_ub_decode_feature_frame(raw.data(), frame_size, nullptr, &r, &error), "inner CRC accepted corruption");
    Queue<int> queue(1); std::atomic<uint64_t> waited{0}; require(queue.push(1, waited), "initial push failed");
    auto producer = std::async(std::launch::async, [&] { return queue.push(2, waited); });
    require(producer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
            "full queue did not block producer");
    queue.close(); require(!producer.get(), "closed queue accepted blocked producer");
    int value = 0; require(queue.pop(value) && value == 1 && !queue.pop(value), "close lost an owned queued value");
    Queue<int> idle(1);
    require(!idle.pop_until(value, Clock::now()+std::chrono::milliseconds(1)), "idle queue timer failed");
    std::cout << "PASS: 256 feature records, float bit patterns, compression, CRC, framing and malformed inputs\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
