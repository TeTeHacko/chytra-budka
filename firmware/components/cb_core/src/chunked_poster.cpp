#include "cb/chunked_poster.h"

#include <cstdio>
#include <cstring>

#include "cb/transport.h"

namespace cb {

namespace {
bool write_all(Transport &tx, const void *buf, size_t n) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t left = n;
  while (left > 0) {
    int w = tx.write(p, left);
    if (w <= 0) return false;
    p += w;
    left -= static_cast<size_t>(w);
  }
  return true;
}

bool write_str(Transport &tx, const char *s) {
  return write_all(tx, s, std::strlen(s));
}
}  // namespace

bool ChunkedPoster::begin(const char *host, uint16_t port, const char *path,
                          const char *content_type, const char *bearer_token) {
  if (headers_sent_) return false;
  if (!tx_.connect(host, port)) return false;

  char hdr[512];
  int n = std::snprintf(
      hdr, sizeof(hdr),
      "POST %s HTTP/1.1\r\n"
      "Host: %s:%u\r\n"
      "%s%s%s"
      "Content-Type: %s\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n",
      path, host, static_cast<unsigned>(port),
      bearer_token ? "Authorization: Bearer " : "",
      bearer_token ? bearer_token : "",
      bearer_token ? "\r\n" : "",
      content_type);

  if (n <= 0 || static_cast<size_t>(n) >= sizeof(hdr)) {
    tx_.close();
    return false;
  }
  if (!write_all(tx_, hdr, static_cast<size_t>(n))) {
    tx_.close();
    return false;
  }
  headers_sent_ = true;
  return true;
}

bool ChunkedPoster::send(const void *buf, size_t n) {
  if (!headers_sent_ || n == 0) return false;
  char hex[24];
  int hn = std::snprintf(hex, sizeof(hex), "%zx\r\n", n);
  if (hn <= 0) return false;
  if (!write_all(tx_, hex, static_cast<size_t>(hn))) return false;
  if (!write_all(tx_, buf, n)) return false;
  if (!write_all(tx_, "\r\n", 2)) return false;
  bytes_sent_ += n;
  return true;
}

bool ChunkedPoster::end() {
  if (!headers_sent_) return false;
  bool ok = write_str(tx_, "0\r\n\r\n");
  // best-effort response read
  if (ok) {
    char resp[64] = {0};
    int r = tx_.read(resp, sizeof(resp) - 1);
    if (r > 0) {
      // expect "HTTP/1.1 NNN ..."
      int code = 0;
      if (std::sscanf(resp, "HTTP/1.%*d %d", &code) == 1) {
        status_code_ = code;
      }
    }
  }
  tx_.close();
  headers_sent_ = false;
  return ok;
}

}  // namespace cb
