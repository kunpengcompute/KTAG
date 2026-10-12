#pragma once
#include "common.h"
#include <array>
#include <filesystem>
#include <functional>

namespace tcp_bench {
std::array<uint8_t, 6> parse_mac(const std::string &);
struct ReplayFile {
  std::string original, prepared, log;
  uint32_t source_index = 0;
  uint64_t packets = 0, bytes = 0;
};
class ReplayFiles {
 public:
  explicit ReplayFiles(const Config &);
  ~ReplayFiles();
  ReplayFiles(const ReplayFiles &) = delete;
  ReplayFiles &operator=(const ReplayFiles &) = delete;
  std::vector<ReplayFile> files;
 private:
  std::string directory_;
  void cleanup() noexcept;
};
void replay_file(const Config &, const ReplayFile &, Metrics &, const std::atomic<bool> &cancel,
                 const std::function<void()> &heartbeat);
} // namespace tcp_bench
