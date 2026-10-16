#pragma once
#include "protocol.h"

namespace tcp_bench {
class Channel {
 public:
  Channel(int fd, uint32_t timeout_s, uint32_t max_raw, Metrics &metrics);
  ~Channel();
  Channel(const Channel &) = delete;
  Channel &operator=(const Channel &) = delete;
  void send(const Message &);
  Message receive();
  // A timeout here means no header byte arrived. A partial header/body always
  // has a deadline and must complete or fail; no bytes are silently discarded.
  bool receive_for(Message &, uint32_t timeout_us);
  void control(Type, uint64_t run, const Writer &body = Writer{});
  void stop();
 private:
  bool read_message(Message &, Clock::time_point idle_deadline, bool allow_idle);
  void transfer(uint8_t *, size_t, bool write, Clock::time_point deadline);
  int fd_;
  uint32_t timeout_s_, max_raw_;
  Metrics &metrics_;
  std::mutex writer_;
};
int connect_to(const Config &);
int accept_frontend(const Config &);
std::string local_interface(const Config &);
extern volatile std::sig_atomic_t interrupted;
} // namespace tcp_bench
