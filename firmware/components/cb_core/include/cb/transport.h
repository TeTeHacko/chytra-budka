// transport.h — abstract byte-stream transport (TCP-like)
// Pure C++17, no Arduino, no platform deps. Implementations:
//   - PosixTransport (tests/native)
//   - ArduinoWifiTransport (src/platform_arduino, wraps WiFiClient)
#pragma once

#include <cstddef>
#include <cstdint>

namespace cb {

struct Transport {
  virtual ~Transport() = default;

  // Connect to host:port. Returns true on success. Implementation-defined timeout.
  virtual bool connect(const char *host, uint16_t port) = 0;

  // Write n bytes. Returns bytes written (>=0) or -1 on error.
  // Implementations should attempt to write all bytes (loop on partial sends).
  virtual int write(const void *buf, size_t n) = 0;

  // Read up to n bytes. Returns bytes read (>=0) or -1 on error.
  // Returns 0 on clean EOF. Should be non-blocking-ish (used only for HTTP response).
  virtual int read(void *buf, size_t n) = 0;

  virtual void close() = 0;
  virtual bool connected() const = 0;
};

}  // namespace cb
