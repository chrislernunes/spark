#pragma once

// Minimal epoll reactor. This is the networking primitive the process would
// use for non-blocking sockets. It does not speak TLS. Live venue sessions
// are captured by the Python recorder until this tree is linked against
// OpenSSL; claiming a kernel-bypass or timestamped capture here would be false.
// SO_TIMESTAMPING is not enabled. Local receive time is userspace CLOCK_REALTIME
// taken after epoll returns, which is later than the NIC timestamp.

#include <functional>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>
#include <unistd.h>

namespace spark {

class Reactor {
 public:
  Reactor() { ep_ = ::epoll_create1(EPOLL_CLOEXEC); }
  ~Reactor() {
    if (ep_ >= 0) ::close(ep_);
  }
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  bool ok() const noexcept { return ep_ >= 0; }

  bool add(int fd, uint32_t events, std::function<void(uint32_t)> fn) {
    if (ep_ < 0) return false;
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
    fns_[fd] = std::move(fn);
    return true;
  }

  bool del(int fd) {
    if (ep_ < 0) return false;
    ::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
    fns_.erase(fd);
    return true;
  }

  // Returns the number of handlers invoked, or -1 on epoll error.
  int poll(int timeout_ms) {
    if (ep_ < 0) return -1;
    epoll_event evs[32];
    int n = ::epoll_wait(ep_, evs, 32, timeout_ms);
    if (n < 0) return -1;
    for (int i = 0; i < n; ++i) {
      auto it = fns_.find(evs[i].data.fd);
      if (it != fns_.end()) it->second(evs[i].events);
    }
    return n;
  }

 private:
  int ep_ = -1;
  std::unordered_map<int, std::function<void(uint32_t)>> fns_;
};

}  // namespace spark
