// transport_posix.h — POSIX TCP implementation of cb::Transport for host tests.
#pragma once

#include "cb/transport.h"

namespace cb {

class PosixTransport : public Transport {
 public:
  PosixTransport() = default;
  ~PosixTransport() override { close(); }

  bool connect(const char *host, uint16_t port) override;
  int write(const void *buf, size_t n) override;
  int read(void *buf, size_t n) override;
  void close() override;
  bool connected() const override { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

}  // namespace cb
