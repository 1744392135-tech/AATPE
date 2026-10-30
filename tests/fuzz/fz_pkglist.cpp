#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <sstream>
#include "fuzz_engine.h"

static std::string g_pkglistPath;
extern "C" inline FILE* my_fopen(const char* path, const char* mode) {
  if (path && std::string(path) == "/data/system/packages.list") {
    return fopen(g_pkglistPath.c_str(), mode);
  }
  return fopen(path, mode);
}
#define fopen my_fopen
#include "frame.cpp"
#undef fopen

static long refGetPkgUid(const std::string& content, const std::string& pkg) {
  std::istringstream iss(content);
  std::string line;
  long uid = -1;
  while (std::getline(iss, line)) {
    if (line.size() > 511) line.resize(511);
    size_t pkgLen = pkg.size();
    if (line.size() >= pkgLen + 1 &&
        line.compare(0, pkgLen, pkg) == 0 &&
        line[pkgLen] == ' ') {
      const char* p = line.c_str() + pkgLen + 1;
      uid = strtol(p, nullptr, 10);
      break;
    }
  }
  return uid;
}

static std::atomic<uint64_t> g_semanticFails{0};

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_pkglist";
  g_pkglistPath = "/tmp/aatpe_pkglist/packages.list";
  mkdir("/tmp/aatpe_pkglist", 0755);
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 900.0;
  opt.seed = 0xC0FFEE ^ 0x47;
  opt.maxLen = 512;
  opt.seeds = {
    "com.fuzz.target 10047 0 /data/data/com.fuzz.target\n",
    "com.fuzz.target 99999999999999999999 0 /data\ncom.other 1000 0 /data\n",
    "com.fuzz.target -1 0 /data\n",
    "com.fuzz.targetX 99999 0 /data\ncom.fuzz.target 2000 1 /data\n",
    "com.fuzz.target  abc 0 /data\n",
    "\x01\x02\x03 garbage line\ncom.fuzz.target 42\n",
    "com.fuzz.target",
    "com.fuzz.target  \n",
  };
  fz::run(opt, [&](const std::string& in) {
    {
      std::ofstream f(g_pkglistPath, std::ios::trunc|std::ios::binary);
      f << in;
    }
    long got = getPkgUid("com.fuzz.target");
    long expected = refGetPkgUid(in, "com.fuzz.target");
    if (got != expected) {
      uint64_t n = ++g_semanticFails;
      char path[320];
      snprintf(path, sizeof(path), "%s/SEMFAIL_pkglist_%u.bin",
               crashDir, (unsigned)n);
      std::ofstream f(path, std::ios::trunc|std::ios::binary);
      f << in;
      fprintf(stderr,
        "[SEMFAIL #%llu] getPkgUid mismatch! got=%ld expected=%ld\n"
        "  input=%s\n  dumped to %s\n",
        (unsigned long long)n, got, expected, in.c_str(), path);
      fflush(stderr);
      abort();
    }
  });
  fprintf(stderr, "[%s] DONE: semantic_fails=%llu\n",
          logTag, (unsigned long long)g_semanticFails.load());
  return 0;
}
