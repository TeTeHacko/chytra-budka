// test_chunked_post.cpp — verify chunked POST framing against an in-process
// loopback transport. No real socket; we capture bytes and parse them.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cb/chunked_poster.h"
#include "cb/transport.h"

namespace {

class LoopbackTransport : public cb::Transport {
 public:
  std::string sent;
  std::string canned_response;
  size_t resp_pos = 0;
  bool open = false;

  bool connect(const char *, uint16_t) override {
    open = true;
    return true;
  }
  int write(const void *buf, size_t n) override {
    if (!open) return -1;
    sent.append(static_cast<const char *>(buf), n);
    return static_cast<int>(n);
  }
  int read(void *buf, size_t n) override {
    if (!open) return -1;
    size_t left = canned_response.size() - resp_pos;
    size_t take = (n < left) ? n : left;
    if (take == 0) return 0;
    std::memcpy(buf, canned_response.data() + resp_pos, take);
    resp_pos += take;
    return static_cast<int>(take);
  }
  void close() override { open = false; }
  bool connected() const override { return open; }
};

}  // namespace

int main() {
  std::printf("test_chunked_post:\n");

  LoopbackTransport tx;
  tx.canned_response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  cb::ChunkedPoster poster(tx);

  bool ok = poster.begin("relay.local", 8765, "/audio/test",
                         "audio/L16; rate=48000; channels=1", "secret123");
  assert(ok);

  // headers contain expected pieces
  assert(tx.sent.find("POST /audio/test HTTP/1.1\r\n") == 0);
  assert(tx.sent.find("Host: relay.local:8765\r\n") != std::string::npos);
  assert(tx.sent.find("Authorization: Bearer secret123\r\n") != std::string::npos);
  assert(tx.sent.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
  assert(tx.sent.find("Content-Type: audio/L16; rate=48000; channels=1\r\n") != std::string::npos);
  std::printf("  ok: headers correct\n");

  // chunk 1: 5 bytes "hello"
  size_t before = tx.sent.size();
  ok = poster.send("hello", 5);
  assert(ok);
  std::string chunk1 = tx.sent.substr(before);
  assert(chunk1 == "5\r\nhello\r\n");
  std::printf("  ok: chunk1 framed as '5\\r\\nhello\\r\\n'\n");

  // chunk 2: 256 bytes (hex 100)
  std::vector<uint8_t> blob(256, 0xAB);
  before = tx.sent.size();
  ok = poster.send(blob.data(), blob.size());
  assert(ok);
  std::string chunk2 = tx.sent.substr(before);
  assert(chunk2.compare(0, 5, "100\r\n") == 0);
  assert(chunk2.size() == 5 + 256 + 2);
  assert(chunk2[chunk2.size() - 2] == '\r' && chunk2[chunk2.size() - 1] == '\n');
  std::printf("  ok: chunk2 (256B) framed correctly\n");

  // terminator
  before = tx.sent.size();
  ok = poster.end();
  assert(ok);
  std::string term = tx.sent.substr(before);
  assert(term == "0\r\n\r\n");
  assert(poster.status_code() == 200);
  std::printf("  ok: terminator + status_code=200 parsed\n");

  // No-bearer variant
  LoopbackTransport tx2;
  tx2.canned_response = "HTTP/1.1 401 Unauthorized\r\n\r\n";
  cb::ChunkedPoster p2(tx2);
  assert(p2.begin("h", 80, "/p", "audio/L16", nullptr));
  assert(tx2.sent.find("Authorization:") == std::string::npos);
  p2.send("x", 1);
  p2.end();
  assert(p2.status_code() == 401);
  std::printf("  ok: bearer omitted when nullptr; 401 parsed\n");

  std::printf("test_chunked_post: PASS\n");
  return 0;
}
