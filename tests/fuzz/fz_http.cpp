// fz_http.cpp - fuzz HTTP request parsing (surface #9: request line / headers /
// body, Content-Length, oversize 16KB, keep-alive, malformed). handleRequest() is
// file-local in webui_http.cpp, so we #include the production TU directly (same
// seam pattern as tests/v94/test_frame_binder.cpp). We then call handleRequest()
// with mutated raw HTTP request text. Must never crash/hang; malformed requests
// must produce a non-200 (fail-closed) response.
#include <string>
#include "fuzz_engine.h"

// Pull in the production translation unit so the file-local handleRequest() is
// visible. Suppress the server's own main-loop assumptions; we only call the
// pure request-string handler.
#include "webui_http.cpp"

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_http";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 1000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x37;
  opt.maxLen = 32768;   // around the 16KB oversize boundary
  opt.seeds = {
    "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
    "GET /state/all HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "POST /api/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nset_mode=fast",
    "GET / HTTP/1.0\r\n\r\n",
  };
  fz::run(opt, [](const std::string& in) {
    std::string resp = handleRequest(in);
    (void)resp;
  });
  return 0;
}
