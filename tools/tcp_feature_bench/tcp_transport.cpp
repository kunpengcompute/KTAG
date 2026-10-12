#include "tcp_transport.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tcp_bench {
static void syscheck(bool ok, const char *why) {
  if (!ok) throw std::runtime_error(std::string(why)+": "+std::strerror(errno));
}
static bool wait_fd(int fd, short events, Clock::time_point deadline, bool idle) {
  for (;;) {
    require(!interrupted, "interrupted");
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
    if (left < 0) { if (idle) return false; throw std::runtime_error("TCP progress timeout"); }
    pollfd p{fd, events, 0}; int n = poll(&p, 1, int(std::min<int64_t>(left+1, 100)));
    if (n < 0 && errno == EINTR) continue;
    syscheck(n >= 0, "poll"); if (!n) continue;
    if (p.revents & events) return true;
    require(!(p.revents & (POLLERR|POLLHUP|POLLNVAL)), "TCP peer closed or socket failed");
  }
}
static sockaddr_in address(const std::string &ip, uint32_t port) {
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(uint16_t(port));
  require(inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1, "invalid IPv4 address: "+ip); return a;
}
static int new_socket() {
  int fd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0); syscheck(fd >= 0, "socket");
  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); syscheck(false, "fcntl"); }
  return fd;
}
int connect_to(const Config &c) {
  int fd = new_socket();
  try {
    auto local = address(c.local_ip, 0), peer = address(c.peer_ip, c.port);
    syscheck(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, c.capture_interface.c_str(),
                       socklen_t(c.capture_interface.size()+1)) == 0, "bind TCP to capture interface");
    syscheck(bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0, "bind frontend IP");
    auto deadline = Clock::now()+std::chrono::seconds(c.timeout_s);
    int n = connect(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
    if (n < 0) {
      syscheck(errno == EINPROGRESS, "connect"); wait_fd(fd, POLLOUT, deadline, false);
      int error = 0; socklen_t length = sizeof(error);
      syscheck(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0, "connect status");
      if (error) { errno = error; syscheck(false, "connect"); }
    }
    return fd;
  } catch (...) { close(fd); throw; }
}
int accept_frontend(const Config &c) {
  int listener = new_socket();
  try {
    int one = 1; syscheck(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "SO_REUSEADDR");
    auto a = address(c.local_ip, c.port);
    syscheck(setsockopt(listener, SOL_SOCKET, SO_BINDTODEVICE, c.replay_interface.c_str(),
                       socklen_t(c.replay_interface.size()+1)) == 0, "bind TCP to replay interface");
    syscheck(bind(listener, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0, "bind backend IP");
    syscheck(listen(listener, 1) == 0, "listen");
    auto deadline = Clock::now()+std::chrono::seconds(c.timeout_s);
    for (;;) {
      wait_fd(listener, POLLIN, deadline, false);
      int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC|SOCK_NONBLOCK);
      if (fd < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      syscheck(fd >= 0, "accept"); close(listener); return fd;
    }
  } catch (...) { close(listener); throw; }
}
Channel::Channel(int fd, uint32_t timeout_s, uint32_t max_raw, Metrics &metrics)
  : fd_(fd), timeout_s_(timeout_s), max_raw_(max_raw), metrics_(metrics) {}
Channel::~Channel() { close(fd_); }
void Channel::stop() { shutdown(fd_, SHUT_RDWR); }
void Channel::transfer(uint8_t *data, size_t count, bool writing, Clock::time_point deadline) {
  while (count) {
    wait_fd(fd_, writing ? POLLOUT : POLLIN, deadline, false);
    ssize_t n = writing ? ::send(fd_, data, count, MSG_NOSIGNAL) : recv(fd_, data, count, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    syscheck(n >= 0, writing ? "send" : "recv"); require(n != 0, "TCP peer closed");
    data += n; count -= size_t(n);
    if (writing) metrics_.socket_tx += uint64_t(n); else metrics_.socket_rx += uint64_t(n);
    // Timeout is based on lack of progress, so a large active batch can finish.
    deadline = Clock::now()+std::chrono::seconds(timeout_s_);
  }
}
void Channel::send(const Message &m) {
  std::lock_guard<std::mutex> lock(writer_);
  auto header = encode_header(m); auto deadline = Clock::now()+std::chrono::seconds(timeout_s_);
  transfer(header.data(), header.size(), true, deadline);
  if (!m.body.empty()) transfer(const_cast<uint8_t *>(m.body.data()), m.body.size(), true, deadline);
}
void Channel::control(Type type, uint64_t run, const Writer &w) {
  Message m; m.type = type; m.run = run; m.body = w.data; send(m);
}
bool Channel::read_message(Message &m, Clock::time_point deadline, bool idle) {
  if (!wait_fd(fd_, POLLIN, deadline, idle)) return false;
  Bytes header(header_size); transfer(header.data(), header.size(), false,
                                       Clock::now()+std::chrono::seconds(timeout_s_));
  uint32_t size = 0; m = decode_header(header, max_raw_, size); m.body.resize(size);
  if (size) transfer(m.body.data(), size, false, Clock::now()+std::chrono::seconds(timeout_s_));
  return true;
}
Message Channel::receive() {
  Message m; read_message(m, Clock::now()+std::chrono::seconds(timeout_s_), false); return m;
}
bool Channel::receive_for(Message &m, uint32_t timeout_us) {
  return read_message(m, Clock::now()+std::chrono::microseconds(timeout_us), true);
}
} // namespace tcp_bench
