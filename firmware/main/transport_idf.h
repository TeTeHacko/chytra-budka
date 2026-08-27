// transport_idf.h — cb::Transport adapter on top of lwIP BSD sockets.
// Header-only, drop-in replacement for ArduinoWifiTransport.
#pragma once

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

#include "cb/transport.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

namespace cb {

class IdfTransport : public Transport {
 public:
  IdfTransport() = default;
  ~IdfTransport() override { IdfTransport::close(); }

  bool connect(const char *host, uint16_t port) override {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_s[8];
    std::snprintf(port_s, sizeof(port_s), "%u", static_cast<unsigned>(port));

    struct addrinfo *res = nullptr;
    if (::getaddrinfo(host, port_s, &hints, &res) != 0 || res == nullptr) {
      ESP_LOGW("transport", "getaddrinfo(%s) failed", host);
      return false;
    }

    fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) {
      ::freeaddrinfo(res);
      return false;
    }

    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Non-blocking connect with a bounded wall-clock timeout. A plain blocking
    // connect() is NOT bounded by SO_SNDTIMEO (that covers send() only), so on
    // a weak link a lost SYN makes it block on lwIP's own TCP SYN retransmit
    // (tens of seconds) — past the task watchdog. The callers (audio relay,
    // GlitchTip) run on TWDT-subscribed tasks, so a blocking connect would
    // panic the board on a flaky link. Drive the handshake ourselves:
    // O_NONBLOCK + select() capped at CONNECT_TIMEOUT_S, then restore blocking
    // mode so write()/read() keep using SO_SNDTIMEO/SO_RCVTIMEO as before.
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) flags = 0;
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    int conn_errno = errno;
    ::freeaddrinfo(res);
    res = nullptr;

    if (rc != 0) {
      if (conn_errno != EINPROGRESS) {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(fd_, &wfds);
      struct timeval ctv {};
      ctv.tv_sec = CONNECT_TIMEOUT_S;
      ctv.tv_usec = 0;
      int sel = ::select(fd_ + 1, nullptr, &wfds, nullptr, &ctv);
      if (sel <= 0) {  // 0 = timed out, <0 = select error
        ESP_LOGW("transport", "connect(%s:%u) timed out after %d s", host,
                 static_cast<unsigned>(port), CONNECT_TIMEOUT_S);
        ::close(fd_);
        fd_ = -1;
        return false;
      }
      int so_err = 0;
      socklen_t so_len = sizeof(so_err);
      if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_err, &so_len) != 0 ||
          so_err != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
    }

    ::fcntl(fd_, F_SETFL, flags);  // back to blocking for write()/read()
    struct timeval tv {};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
  }

  int write(const void *buf, size_t n) override {
    if (fd_ < 0) return -1;
    auto *p = static_cast<const uint8_t *>(buf);
    size_t total = 0;
    while (total < n) {
      ssize_t w = ::send(fd_, p + total, n - total, 0);
      if (w <= 0) return -1;
      total += static_cast<size_t>(w);
    }
    return static_cast<int>(total);
  }

  int read(void *buf, size_t n) override {
    if (fd_ < 0) return -1;
    ssize_t r = ::recv(fd_, buf, n, 0);
    if (r < 0) return 0;  // timeout / would-block → no bytes available
    return static_cast<int>(r);
  }

  void close() override {
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool connected() const override { return fd_ >= 0; }

 private:
  // Bounded connect handshake — must stay well under the task watchdog so a
  // dead/unreachable peer on a flaky link can never starve a TWDT-subscribed
  // caller. The relay/GlitchTip paths have their own retry/backoff above this.
  static constexpr int CONNECT_TIMEOUT_S = 5;
  int fd_ = -1;
};

}  // namespace cb
