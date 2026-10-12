#include "offline_input.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace tcp_bench;
static void u32(std::ofstream &out, uint32_t value) { for (unsigned i = 0; i < 4; ++i) out.put(char(value >> (8*i))); }
static void header(std::ofstream &out) {
  u32(out, 0xa1b2c3d4); out.put(2); out.put(0); out.put(4); out.put(0);
  u32(out, 0); u32(out, 0); u32(out, 65535); u32(out, 1);
}
static void record(std::ofstream &out, uint32_t seconds, uint32_t micros, uint32_t captured, uint32_t original, uint32_t written) {
  u32(out, seconds); u32(out, micros); u32(out, captured); u32(out, original);
  for (uint32_t i = 0; i < written; ++i) out.put(char(i));
}
int main() {
  auto path = std::filesystem::temp_directory_path()/("ktag-local-input-"+std::to_string(now_ns())+".pcap");
  try {
    { std::ofstream out(path, std::ios::binary); header(out); record(out, 10, 123456, 60, 60, 60);
      record(out, 9, 654321, 9000, 9000, 9000); record(out, 12, 1, 60, 200, 60); record(out, 13, 0, 60, 60, 3); }
    {
      OfflineInput input(path.string(), 1); require(input.link_type() == 1, "link type changed");
      Packet first, second;
      require(input.next(first) == KTAG_SUCCESS && ktag_packet_is_owned(&first.value) && first.value.sequence == 1 &&
        first.value.timestamp.seconds == 10 && first.value.timestamp.microseconds == 123456, "first owned packet/timestamp changed");
      require(input.next(second) == KTAG_ERROR_CAPACITY, "owned pool did not enforce capacity");
      require(first.value.data[17] == 17, "owned first packet changed while pool exhausted"); first = Packet{};
      require(input.next(second) == KTAG_SUCCESS && second.value.sequence == 2 && second.value.captured_length == 9000 &&
        second.value.timestamp.seconds == 9 && second.value.timestamp.microseconds == 654321,
        "capacity retry consumed packet, timestamp order changed or MTU restriction added"); second = Packet{};
      require(input.next(first) == KTAG_SUCCESS && first.value.captured_length == 60 && first.value.original_length == 200,
              "snaplen-short record was rejected"); first = Packet{};
      auto status = input.next(first); auto error = input.error();
      require(recoverable_truncation(status, 3, error) && !recoverable_truncation(status, 0, error), "truncated-file compatibility changed");
    }
    { std::ofstream out(path, std::ios::binary); header(out); }
    { OfflineInput empty(path.string(), 1); Packet p; require(empty.next(p) == KTAG_STATUS_END_OF_STREAM, "valid empty PCAP did not EOF"); }
    require(!recoverable_truncation(KTAG_ERROR_IO, 3, "permission denied"), "unrelated IO error was treated as recoverable");
    std::filesystem::remove(path); std::cout << "PASS: owned pool, retry order, original timestamps, large/short packets, EOF and truncation\n"; return 0;
  } catch (const std::exception &e) { std::filesystem::remove(path); std::cerr << e.what() << '\n'; return 1; }
}
