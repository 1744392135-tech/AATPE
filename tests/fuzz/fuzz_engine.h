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

#include "aatped_logging.h"
extern const char* LOG_FILE;

namespace fz {

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

inline std::string g_currentInput;
inline std::atomic<bool> g_inCall{false};
inline std::atomic<bool> g_done{false};
inline std::chrono::steady_clock::time_point g_callStart;
inline std::string g_crashDir = ".";
inline std::atomic<int> g_crashCount{0};
inline std::string g_parserName = "parser";

extern "C" inline void crash_dump_handler(int) {
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
  signal(SIGABRT, SIG_DFL); signal(SIGSEGV, SIG_DFL);
  signal(SIGILL, SIG_DFL);  signal(SIGBUS, SIG_DFL);
  signal(SIGFPE, SIG_DFL);
  raise(SIGABRT);
  _exit(1);
}

inline void mutate(Rng& r, std::string& s, size_t maxLen) {
  uint64_t op = r.bounded(32);
  switch (op) {
    case 0: {
      s.clear();
      break;
    }
    case 1: {
      size_t n = r.bounded(64) + 1;
      s.assign(n, '\0');
      break;
    }
    case 2: {
      if (s.empty()) s.push_back((char)r.bounded(256));
      else s[r.bounded(s.size())] = (char)r.bounded(256);
      break;
    }
    case 3: {
      if (!s.empty()) s.resize(r.bounded(s.size() + 1));
      break;
    }
    case 4: {
      size_t pos = s.empty() ? 0 : r.bounded(s.size() + 1);
      s.insert(pos, 1, (char)r.bounded(256));
      break;
    }
    case 5: {
      if (s.size() > 2) {
        size_t pos = r.bounded(s.size());
        size_t len = r.bounded(s.size() - pos) + 1;
        s.erase(pos, len);
      }
      break;
    }
    case 6: {
      size_t n = (size_t)(r.bounded(1024) * 1024) + 1;
      if (n > maxLen) n = maxLen;
      s.assign(n, 'A' + (char)r.bounded(26));
      break;
    }
    case 7: {
      size_t n = r.bounded(48) + 1;
      s.clear();
      for (size_t i = 0; i < n; i++) s.push_back((char)(0x80 + r.bounded(128)));
      break;
    }
    case 8: {
      static const char specials[] = {'\"', '\\', '\n', '\r', '\t', '\0', '{', '}', '[', ']', ':', ',', '#'};
      if (s.empty()) s = "{}";
      size_t pos = r.bounded(s.size() + 1);
      s.insert(pos, 1, specials[r.bounded(sizeof(specials))]);
      break;
    }
    case 9: {
      static const char* frags[] = {"/", "..", "../", "./", ";", "|", "&", "$", "`", "'", "\"", " ", "*", "?", "~", "/dev/null", "%s", "%n"};
      if (s.empty()) s = "pkg";
      s += frags[r.bounded(sizeof(frags) / sizeof(frags[0]))];
      break;
    }
    case 10: {
      static const char* nums[] = {"0", "-1", "1", "99999999999999999999", "-99999999999999999999",
                                   "0.0", "-0.0", "1e999", "-1e999", "NaN", "Inf", "-Inf", "0xFFFFFFFFFFFFFFFF",
                                   "0x", "-", "+", ".", "  ", " \t "};
      if (s.empty()) s = "x=";
      s += nums[r.bounded(sizeof(nums) / sizeof(nums[0]))];
      break;
    }
    case 11: {
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
    case 12: {
      char c = (char)('{' + r.bounded(6));
      s.assign((size_t)(r.bounded(2000) + 1), c);
      break;
    }
    case 13: {
      static const char* frags[] = {"key=value", "pkg:com.foo", "0-7", "0xdeadbeef",
                                   "Status: 200", "Content-Length: 99999999", "GET / HTTP/1.1",
                                   "Timeline: ", "0 ms", "package:com.bar", "# comment", "a=b=c=d"};
      s += frags[r.bounded(sizeof(frags) / sizeof(frags[0]))];
      break;
    }
    default: {
      size_t n = r.bounded(64);
      s.clear();
      for (size_t i = 0; i < n; i++) s.push_back((char)r.bounded(256));
      break;
    }
  }
  if (s.size() > maxLen) s.resize(maxLen);
}

struct Options {
  std::string name;
  std::string crashDir;
  uint64_t minIter = 20000000ULL;
  double minSeconds = 1800.0;
  uint64_t seed = 0xC0FFEEULL;
  std::vector<std::string> seeds;
  size_t maxLen = 256 * 1024;
};

template <class Fn>
int run(const Options& opt, Fn&& fn) {
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

}
