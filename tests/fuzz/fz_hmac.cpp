#include <string>
#include "aatped_hmac.h"
#include "fuzz_engine.h"

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_hmac";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x29;
  opt.maxLen = 8192;
  opt.seeds = {
    "status\ndeadbeefcafe",
    "set_mode=performance\n00112233445566778899aabbccddeeff",
    "status\n",
  };
  fz::run(opt, [](const std::string& in) {
    std::string cmd = in, sig;
    size_t nl = in.find('\n');
    if (nl != std::string::npos) { cmd = in.substr(0, nl); sig = in.substr(nl + 1); }
    bool ok = verifyCommandHmac(cmd.c_str(), sig.c_str());
    (void)ok;
    if (sig.empty()) {
      bool ok2 = verifyCommandHmac(cmd.c_str(), nullptr);
      (void)ok2;
    }
  });
  return 0;
}
