// chunked_poster.h — HTTP/1.1 chunked POST to audio relay.
// Pure C++17. Driven via cb::Transport injection so it is testable on host.
//
// Wire format:
//   POST {path} HTTP/1.1\r\n
//   Host: {host}\r\n
//   Authorization: Bearer {token}\r\n   (omitted if token == nullptr)
//   Content-Type: {ct}\r\n
//   Transfer-Encoding: chunked\r\n
//   Connection: close\r\n
//   \r\n
//   {hex_len}\r\n{bytes}\r\n   (repeat for each send())
//   0\r\n\r\n                  (terminator from end())
#pragma once

#include <cstddef>
#include <cstdint>

namespace cb {

struct Transport;

class ChunkedPoster {
 public:
  explicit ChunkedPoster(Transport &tx) : tx_(tx) {}

  // Open connection and send headers.
  // bearer_token may be nullptr to omit Authorization.
  bool begin(const char *host, uint16_t port, const char *path,
             const char *content_type, const char *bearer_token);

  // Send a chunk. n must be > 0; zero-length chunk is reserved as terminator.
  bool send(const void *buf, size_t n);

  // Send terminator and (best-effort) read response status line.
  // After this returns, transport is closed.
  bool end();

  int status_code() const { return status_code_; }
  bool headers_sent() const { return headers_sent_; }

 private:
  Transport &tx_;
  bool headers_sent_ = false;
  int status_code_ = 0;
  uint64_t bytes_sent_ = 0;
};

}  // namespace cb
