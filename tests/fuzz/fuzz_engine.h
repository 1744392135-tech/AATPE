// fuzz_engine.h - in-process, hermetic fuzz harness for AATPE untrusted-input parsers.
// No afl/libFuzzer on host. Provides: deterministic xorshift128+ PRNG, structured
// mutator, dual gate (iteration count AND wall-clock), per-call watchdog thread to
// catch hangs, timestamped progress, and a signal handler that dumps the exact
// crashing input so ASan/UBSan stacks can be paired with a minimal reproducer.
//
// W1 constraint: this file lives ONLY under tests/fuzz/. It never touches product src.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

// v4.4.8 fuzz hang fix: wire the production logging backend to /dev/null before the
// fuzz loop. None of the 16 fuzz mains call logInit(), so g_logFp stayed null and
// every WARN/INFO was appended to g_logBuffer forever (the null-fp orphan branch in
// aatped_logging.cpp had no drain under fuzz) -> unbounded growth -> geometric
// realloc crossed the 3s watchdog. Pointing LOG_FILE at /dev/null (always writable)
// makes g_logFp valid: WARN/ERROR write+flush immediately, INFO self-flushes at the
// 4KB/100-entry threshold, so g_logBuffer stays bounded. Done once in run(), which
// every fz_*.cpp goes through.
#include "aatped_logging.h"   // logInit()
extern const char* LOG_FILE;  // defined in fz_daemon_stub.cpp / dirs.cpp / logging_deps.cpp

namespace fz {

// ---- deterministic PRNG (xorshift128+, splitmix-seeded for spread) ----------
struct Rng {
  uint64_t s0, s1;
  explicit Rng(uint64_t seed) { reinit(seed); }
  void reinit(uint64_t seed) {
    uint64_t z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    s0 = z ^ (z >> 31);
    z = seed + 0xD1B54A32D192ED03ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    s1 = z ^ (z >> 31);
  }
  uint64_t next() {
    uint64_t x = s0, y = s1;
    s0 = y;
    x ^= x << 23;
    s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return s1 + y;
  }
  uint64_t bounded(uint64_t n) { return n ? next() % n : 0; }
  bool chance(uint64_t denom) { return bounded(denom) == 0; }
};

// ---- crash / hang global context -------------------------------------------
inline std::string g_currentInput;
inline std::atomic<bool> g_inCall{false};
inline std::atomic<bool> g_done{false};
inline std::chrono::steady_clock::time_point g_callStart;
inline std::string g_crashDir = ".";
inline std::atomic<int> g_crashCount{0};
inline std::string g_parserName = "parser";

extern "C" inline void crash_dump_handler(int) {
  // Async-signal-safe: dump the exact input that caused the fault.
  int n = ++g_crashCount;
  char path[320];
  snprintf(path, sizeof(path), "%s/crash_%s_%d.bin", g_crashDir.c_str(),
           g_parserName.c_str(), n);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    if (!g_currentInput.empty())
      (void)!write(fd, g_currentInput.data(), g_currentInput.size());
    close(fd);
  }
  // Let ASan/UBSan print its own stack to the redirected stderr.
  signal(SIGABRT, SIG_DFL); signal(SIGSEGV, SIG_DFL);
  signal(SIGILL, SIG_DFL);  signal(SIGBUS, SIG_DFL);
  signal(SIGFPE, SIG_DFL);
  raise(SIGABRT);
  _exit(1);
}

// ---- structured mutator -----------------------------------------------------
inline void mutate(Rng& r, std::string& s, size_t maxLen) {
  // pick an operation by weight
  uint64_t op = r.bounded(32);
  switch (op) {
    case 0: { // wipe to empty
      s.clear();
      break;
    }
    case 1: { // all zero bytes
      size_t n = r.bounded(64) + 1;
      s.assign(n, '\0');
      break;
    }
    case 2: { // random byte at random pos
      if (s.empty()) s.push_back((char)r.bounded(256));
      else s[r.bounded(s.size())] = (char)r.bounded(256);
      break;
    }
    case 3: { // truncate
      if (!s.empty()) s.resize(r.bounded(s.size() + 1));
      break;
    }
    case 4: { // insert random byte
      size_t pos = s.empty() ? 0 : r.bounded(s.size() + 1);
      s.insert(pos, 1, (char)r.bounded(256));
      break;
    }
    case 5: { // delete a chunk
      if (s.size() > 2) {
        size_t pos = r.bounded(s.size());
        size_t len = r.bounded(s.size() - pos) + 1;
        s.erase(pos, len);
      }
      break;
    }
    case 6: { // oversize boundary (up to ~1MB)
      size_t n = (size_t)(r.bounded(1024) * 1024) + 1;
      if (n > maxLen) n = maxLen;
      s.assign(n, 'A' + (char)r.bounded(26));
      break;
    }
    case 7: { // invalid UTF-8 / high bytes
      size_t n = r.bounded(48) + 1;
      s.clear();
      for (size_t i = 0; i < n; i++) s.push_back((char)(0x80 + r.bounded(128)));
      break;
    }
    case 8: { // embedded quote / backslash / newline / NUL
      static const char specials[] = {'\"', '\\', '\n', '\r', '\t', '\0', '{', '}', '[', ']', ':', ',', '#'};
      if (s.empty()) s = "{}";
      size_t pos = r.bounded(s.size() + 1);
      s.insert(pos, 1, specials[r.bounded(sizeof(specials))]);
      break;
    }
    case 9: { // path traversal / shell metacharacters
      static const char* frags[] = {"/", "..", "../", "./", ";", "|", "&", "$", "`", "'", "\"", " ", "*", "?", "~", "/dev/null", "%s", "%n"};
      if (s.empty()) s = "pkg";
      s += frags[r.bounded(sizeof(frags) / sizeof(frags[0]))];
      break;
    }
    case 10: { // numeric extremes / negative / nan / inf
      static const char* nums[] = {"0", "-1", "1", "99999999999999999999", "-99999999999999999999",
                                   "0.0", "-0.0", "1e999", "-1e999", "NaN", "Inf", "-Inf", "0xFFFFFFFFFFFFFFFF",
                                   "0x", "-", "+", ".", "  ", " \t "};
      if (s.empty()) s = "x=";
      s += nums[r.bounded(sizeof(nums) / sizeof(nums[0]))];
      break;
    }
    case 11: { // field duplication / reorder: duplicate a line
      if (!s.empty()) {
        size_t nl = s.find('\n');
        if (nl != std::string::npos) {
          std::string line = s.substr(0, nl);
          s.insert(0, line + "\n");
        } else {
            s += s;
        }
      }
      break;
    }
    case 12: { // repeated structural char blowup
      char c = (char)('{' + r.bounded(6));
      s.assign((size_t)(r.bounded(2000) + 1), c);
      break;
    }
    case 13: { // splice two seeds worth: append a known fragment
      static const char* frags[] = {"key=value", "pkg:com.foo", "0-7", "0xdeadbeef",
                                   "Status: 200", "Content-Length: 99999999", "GET / HTTP/1.1",
                                   "Timeline: ", "0 ms", "package:com.bar", "# comment", "a=b=c=d"};
      s += frags[r.bounded(sizeof(frags) / sizeof(frags[0]))];
      break;
    }
    default: { // plain random bytes
      size_t n = r.bounded(64);
      s.clear();
      for (size_t i = 0; i < n; i++) s.push_back((char)r.bounded(256));
      break;
    }
  }
  if (s.size() > maxLen) s.resize(maxLen);
}

// ---- options ----------------------------------------------------------------
struct Options {
  std::string name;
  std::string crashDir;
  uint64_t minIter = 20000000ULL;
  double minSeconds = 1800.0;  // 30 min wall-clock gate
  uint64_t seed = 0xC0FFEEULL;
  std::vector<std::string> seeds;
  size_t maxLen = 256 * 1024;
};

// ---- runner: dual gate + watchdog ------------------------------------------
template <class Fn>
int run(const Options& opt, Fn&& fn) {
  // Wire the log sink to /dev/null once so the null-fp orphan buffer can never
  // grow unbounded during the fuzz loop (see header comment). fchmod on /dev/null
  // failing is harmless.
  LOG_FILE = "/dev/null";
  logInit();

  g_crashDir = opt.crashDir;
  g_parserName = opt.name;
  g_done = false;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = crash_dump_handler;
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);

  // hang watchdog: if a single parse call exceeds 3 s, abort (infinite loop).
  std::thread watcher([&]() {
    while (!g_done.load()) {
      if (g_inCall.load()) {
        auto now = std::chrono::steady_clock::now();
        double d = std::chrono::duration<double>(now - g_callStart).count();
        if (d > 3.0) {
          char path[320];
          snprintf(path, sizeof(path), "%s/HANG_%s.bin", g_crashDir.c_str(),
                   g_parserName.c_str());
          int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (fd >= 0) {
            if (!g_currentInput.empty())
              (void)!write(fd, g_currentInput.data(), g_currentInput.size());
            close(fd);
          }
          fprintf(stderr, "[%s] WATCHDOG: single call exceeded 3s (hang) -> abort\n",
                  opt.name.c_str());
          fflush(stderr);
          raise(SIGABRT);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  Rng rng(opt.seed);
  std::string cur;
  if (!opt.seeds.empty()) cur = opt.seeds[rng.bounded(opt.seeds.size())];

  uint64_t iter = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (iter == 0 || rng.chance(64)) {
      // re-seed from corpus occasionally to stay near structured grammar
      if (!opt.seeds.empty()) cur = opt.seeds[rng.bounded(opt.seeds.size())];
    }
    mutate(rng, cur, opt.maxLen);

    g_currentInput = cur;
    g_callStart = std::chrono::steady_clock::now();
    g_inCall = true;
    fn(cur);
    g_inCall = false;

    iter++;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - t0).count();
    if (iter % 1000000ULL == 0) {
      std::time_t t = std::time(nullptr);
      struct tm lt;
      localtime_r(&t, &lt);
      char tb[64];
      strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &lt);
      fprintf(stderr, "[%s] iter=%llu elapsed=%.1fs (gate: iter>=%llu, secs>=%.0f)\n",
              tb, (unsigned long long)iter, secs,
              (unsigned long long)opt.minIter, opt.minSeconds);
      fflush(stderr);
    }
    if (iter >= opt.minIter && secs >= opt.minSeconds) break;
  }

  g_done = true;
  watcher.join();
  auto tend = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(tend - t0).count();
  fprintf(stderr, "=== %s DONE: iter=%llu wall=%.1fs crashes=%d ===\n",
          opt.name.c_str(), (unsigned long long)iter, secs,
          (int)g_crashCount.load());
  fflush(stderr);
  return 0;
}

// helper: load a seed file into a string
inline std::string slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string s(n, '\0');
  if (n > 0) fread(&s[0], 1, n, f);
  fclose(f);
  return s;
}

}  // namespace fz
