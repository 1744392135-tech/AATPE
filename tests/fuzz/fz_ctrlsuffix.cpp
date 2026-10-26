// fz_ctrlsuffix.cpp - R-W3 strengthened: fuzz /control suffix parsing with REAL
// auth-semantic assertions (not just "no crash").
//
// On host, g_cfg.hmacRequired=true (Config default), verifyCommandHmac stub is
// always true, and no session auth. So the production flow requires:
//   1. .sig= present and non-empty   -> else 403
//   2. .ts= parseable by stoll       -> else 403
//   3. .nonce= present and non-empty  -> else 403 (LED-154)
//   4. ts within ±300s wall clock     -> else 403
// Only then does it reach commandQueueEnqueue -> "HTTP/1.1 200" + "ok":true.
//
// ASSERTION: if the response contains "HTTP/1.1 200" (queued), the input MUST
// have had all three security fields present and ts in-window. Any 200 from
// a malformed input = auth bypass = semantic failure (abort + dump).
#include <string>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include "fuzz_engine.h"

#include "webui_http.cpp"

// Mirror the production suffix parser (webui_http.cpp:877-904) for reference.
struct SuffixRef {
  bool hasSig = false, hasTs = false, hasNonce = false;
  std::string sig, nonce;
  int64_t ts = 0;
};

static SuffixRef refParse(const std::string& command) {
  SuffixRef r;
  std::string actualCmd = command;
  size_t sigPos = command.rfind(".sig=");
  if (sigPos != std::string::npos) {
    r.sig = command.substr(sigPos + 5);
    actualCmd = command.substr(0, sigPos);
    r.hasSig = true;
  }
  size_t tsPos = actualCmd.rfind(".ts=");
  if (tsPos != std::string::npos) {
    std::string tsStr = actualCmd.substr(tsPos + 4);
    try { r.ts = std::stoll(tsStr); r.hasTs = true; }
    catch (...) { r.hasTs = false; }
    actualCmd = actualCmd.substr(0, tsPos);
  }
  size_t noncePos = actualCmd.rfind(".nonce=");
  if (noncePos != std::string::npos) {
    r.nonce = actualCmd.substr(noncePos + 7);
    r.hasNonce = true;
  }
  return r;
}

static std::atomic<uint64_t> g_semanticFails{0};

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_ctrlsuffix";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 900.0;   // strengthened: ≥900s per user spec
  opt.seed = 0xC0FFEE ^ 0x41;
  opt.maxLen = 512;
  opt.seeds = {
    "set_mode=performance.ts=1700000000.sig=deadbeefcafe",
    "boost_game=com.foo.ts=99999999999999999999.sig=00112233",
    "set_mode=fast.ts=-5.sig=abc.nonce=ff",
    "unknown.cmd.ts=abc.sig=",
    "set_mode=performance.sig=deadbeef.ts=1700000000",
    "set_mode=performance.ts=1700000000.nonce=aa.bb",
    "set_mode=performance.ts=1700000000.sig=00.nonce=00",
  };
  fz::run(opt, [&](const std::string& in) {
    std::string resp = handleControl(in, false, false);
    bool got200 = resp.find("HTTP/1.1 200") != std::string::npos;
    if (!got200) return;   // fail-closed: no assertion needed

    // Got a 200 (queued). Verify the input was security-complete.
    SuffixRef ref = refParse(in);
    int64_t now = (int64_t)time(nullptr);
    int64_t age = now - ref.ts;
    bool tsInWindow = (ref.hasTs && age >= -300 && age <= 300);
    bool securityComplete = ref.hasSig && !ref.sig.empty() &&
                            ref.hasTs && !ref.nonce.empty() &&
                            tsInWindow;
    if (!securityComplete) {
      uint64_t n = ++g_semanticFails;
      char path[320];
      snprintf(path, sizeof(path), "%s/SEMFAIL_ctrlsuffix_%u.bin",
               crashDir, (unsigned)n);
      std::ofstream f(path, std::ios::trunc|std::ios::binary);
      f << in;
      fprintf(stderr,
        "[SEMFAIL #%llu] got HTTP 200 from malformed input!\n"
        "  input=%s\n  hasSig=%d sig.empty=%d hasTs=%d nonce.empty=%d "
        "tsInWindow=%d ts=%lld age=%lld\n  dumped to %s\n",
        (unsigned long long)n, in.c_str(),
        (int)ref.hasSig, (int)ref.sig.empty(), (int)ref.hasTs,
        (int)ref.nonce.empty(), (int)tsInWindow,
        (long long)ref.ts, (long long)age, path);
      fflush(stderr);
      abort();   // stop campaign immediately on auth bypass
    }
  });
  fprintf(stderr, "[%s] DONE: semantic_fails=%llu\n",
          logTag, (unsigned long long)g_semanticFails.load());
  return 0;
}
