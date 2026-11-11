#include "tcp_transport.h"
#include <future>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace tcp_bench { volatile std::sig_atomic_t interrupted = 0; }
using namespace tcp_bench;
struct Pair {
  int fd[2]{-1, -1};
  Pair() { require(socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0, fd) == 0, "socketpair failed"); }
  ~Pair() { for (int f : fd) if (f >= 0) close(f); }
  int take(size_t index) { int out = fd[index]; fd[index] = -1; return out; }
};
template<class F> bool rejects(F fn) { try { fn(); return false; } catch (const std::exception &) { return true; } }
int main() {
  try {
    {
      Pair pair; Metrics tx, rx; Channel sender(pair.take(0), 5, 4194304, tx), receiver(pair.take(1), 5, 4194304, rx);
      Message big; big.type = Type::features; big.run = 7; big.sequence = 1; big.count = 1;
      big.raw_size = 524288; big.body.resize(524288);
      for (size_t i = 0; i < big.body.size(); ++i) big.body[i] = uint8_t(i);
      auto sending = std::async(std::launch::async, [&] {
        sender.send(big); Writer w; w.u64(123); sender.control(Type::end, 7, w); sender.control(Type::done, 7);
      });
      auto m = receiver.receive(); require(m.body == big.body && m.sequence == 1, "large partial transfer changed data");
      m = receiver.receive(); expect(m, Type::end, 7); Reader r(m.body); require(r.u64() == 123, "coalesced message changed data"); r.finish();
      expect(receiver.receive(), Type::done, 7); sending.get();
      require(tx.socket_tx == rx.socket_rx, "application byte counters differ");
    }
    {
      Pair pair; Metrics metrics; Channel receiver(pair.take(1), 5, 4194304, metrics);
      Message m; m.type = Type::hello; Writer w; w.text("chunked header"); m.body = w.data;
      auto header = encode_header(m); int fd = pair.fd[0];
      auto sending = std::async(std::launch::async, [&] {
        for (auto byte : header) { require(::send(fd, &byte, 1, MSG_NOSIGNAL) == 1, "chunk write failed");
          std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        require(::send(fd, m.body.data(), m.body.size(), MSG_NOSIGNAL) == ssize_t(m.body.size()), "body write failed");
      });
      require(receiver.receive().body == m.body, "fragmented header was not reassembled"); sending.get();
      Message idle; require(!receiver.receive_for(idle, 1000), "idle timeout did not return");
      auto blocked = std::async(std::launch::async, [&] { return rejects([&] { receiver.receive(); }); });
      receiver.stop(); require(blocked.get(), "shutdown did not cancel blocked receiver");
    }
    {
      Pair pair; Metrics metrics; Channel receiver(pair.take(1), 5, 4194304, metrics);
      Message m; m.type = Type::hello; m.body = Bytes(20, 0); auto header = encode_header(m);
      int fd = pair.take(0);
      require(::send(fd, header.data(), header.size(), MSG_NOSIGNAL) == ssize_t(header.size()), "header write failed");
      require(::send(fd, m.body.data(), 5, MSG_NOSIGNAL) == 5, "partial body write failed"); close(fd);
      require(rejects([&] { receiver.receive(); }), "closed partial message was accepted");
    }
    {
      Pair slow, fast; Metrics metrics;
      Channel receiver(slow.take(1), 5, 4194304, metrics);
      Channel other(fast.take(1), 5, 4194304, metrics);
      Message expected; expected.type = Type::hello; expected.body = Bytes(20, 7);
      auto header = encode_header(expected); Message actual; bool progressed = true;
      require(!receiver.receive_available(actual, progressed) && !progressed, "empty socket should return immediately");
      require(::send(slow.fd[0], header.data(), 17, MSG_NOSIGNAL) == 17, "partial header write failed");
      require(!receiver.receive_available(actual, progressed) && progressed, "partial header was not retained");
      Message done; done.type = Type::done; auto done_header = encode_header(done);
      require(::send(fast.fd[0], done_header.data(), done_header.size(), MSG_NOSIGNAL) == ssize_t(done_header.size()), "second socket write failed");
      require(other.receive_available(actual, progressed) && actual.type == Type::done,
              "a partial frame blocked an unrelated socket");
      require(::send(slow.fd[0], header.data()+17, header.size()-17, MSG_NOSIGNAL) == ssize_t(header.size()-17), "remaining header write failed");
      require(!receiver.receive_available(actual, progressed) && progressed, "missing body should retain the header");
      require(::send(slow.fd[0], expected.body.data(), 5, MSG_NOSIGNAL) == 5, "partial body write failed");
      require(!receiver.receive_available(actual, progressed) && progressed, "partial body was not retained");
      Bytes tail(expected.body.begin()+5, expected.body.end());
      tail.insert(tail.end(), done_header.begin(), done_header.end());
      require(::send(slow.fd[0], tail.data(), tail.size(), MSG_NOSIGNAL) == ssize_t(tail.size()), "coalesced tail write failed");
      require(receiver.receive_available(actual, progressed) && actual.body == expected.body, "incremental frame changed data");
      require(receiver.receive_available(actual, progressed) && actual.type == Type::done, "next frame boundary was lost");
      close(slow.take(0));
      require(rejects([&] { receiver.receive_available(actual, progressed); }), "incremental receiver ignored a disconnected peer");
    }
    {
      Queue<Bytes> queue(1); Bytes first{1}, second{2}, value;
      require(queue.try_push(first), "queue did not acquire the first value");
      require(!queue.try_push(second) && second == Bytes{2}, "full queue consumed the pending value");
      require(queue.try_pop(value) && value == Bytes{1}, "queue changed the first value");
      require(queue.try_push(second), "queue did not resume after backpressure");
      queue.close();
      require(queue.pop(value) && value == Bytes{2} && !queue.pop(value), "closed queue failed to drain");
    }
    std::cout << "PASS: partial transfers, fragmented headers, coalesced messages, idle timeout and shutdown\n"; return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
