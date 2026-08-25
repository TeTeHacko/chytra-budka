#include "transport_posix.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace cb {

bool PosixTransport::connect(const char *host, uint16_t port) {
  if (fd_ >= 0) return true;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  char port_str[8];
  std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

  addrinfo *res = nullptr;
  int rc = ::getaddrinfo(host, port_str, &hints, &res);
  if (rc != 0 || !res) {
    std::fprintf(stderr, "getaddrinfo(%s:%u): %s\n", host, port,
                 gai_strerror(rc));
    return false;
  }

  int s = -1;
  for (addrinfo *ai = res; ai; ai = ai->ai_next) {
    s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s < 0) continue;
    if (::connect(s, ai->ai_addr, ai->ai_addrlen) == 0) break;
    ::close(s);
    s = -1;
  }
  ::freeaddrinfo(res);
  if (s < 0) {
    std::fprintf(stderr, "connect(%s:%u) failed: %s\n", host, port,
                 std::strerror(errno));
    return false;
  }

  // TCP_NODELAY → low-latency chunk dispatch matches ESP32 expected behaviour.
  int one = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // Read timeout 2 s for response slurp.
  timeval tv{};
  tv.tv_sec = 2;
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  fd_ = s;
  return true;
}

int PosixTransport::write(const void *buf, size_t n) {
  if (fd_ < 0) return -1;
  ssize_t w = ::send(fd_, buf, n, 0);
  return static_cast<int>(w);
}

int PosixTransport::read(void *buf, size_t n) {
  if (fd_ < 0) return -1;
  ssize_t r = ::recv(fd_, buf, n, 0);
  return static_cast<int>(r);
}

void PosixTransport::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace cb
