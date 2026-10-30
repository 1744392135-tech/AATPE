#include <string>
#include "fuzz_engine.h"

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
  opt.maxLen = 32768;
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
