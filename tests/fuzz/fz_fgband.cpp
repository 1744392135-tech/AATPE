// fz_fgband.cpp - fuzz aatpeFgBandCandidate (pure function) with extreme oom values.
// Assert: never selects persistent (<=-700) or background (>=400) candidates;
// in-band values return true with correct preference.
#include <string>
#include <climits>
#include "fuzz_engine.h"

// Declare the pure function from frame.cpp
bool aatpeFgBandCandidate(int oom, int* outPreference);

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_fgband";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 600.0;
  opt.seed = 0xF00D ^ 0x51;
  opt.maxLen = 16;
  opt.seeds = {
    "0", "-100", "-700", "-800", "-900", "400", "900", "9999",
    "-9999", "1", "100", "200", "300", "399", "-701", "399",
  };
  uint64_t semFails = 0;
  fz::run(opt, [&](const std::string& in) {
    // Parse the mutated bytes as an integer
    int oom = 999;
    try { oom = std::stoi(in); } catch (...) { return; }  // non-numeric: skip
    int pref = 0;
    bool ok = aatpeFgBandCandidate(oom, &pref);
    // Assert: persistent (<=-700) must be rejected
    if (oom <= -700 && ok) {
      fprintf(stderr, "[SEMFAIL] persistent oom=%d selected!\n", oom);
      semFails++; abort();
    }
    // Assert: background (>=400) must be rejected
    if (oom >= 400 && ok) {
      fprintf(stderr, "[SEMFAIL] background oom=%d selected!\n", oom);
      semFails++; abort();
    }
    // Assert: in-band (-699..399) must be accepted with correct preference
    if (oom > -700 && oom < 400) {
      if (!ok || pref != oom) {
        fprintf(stderr, "[SEMFAIL] in-band oom=%d rejected or wrong pref=%d\n", oom, pref);
        semFails++; abort();
      }
    }
  });
  fprintf(stderr, "[%s] DONE: semantic_fails=%llu\n", logTag, (unsigned long long)semFails);
  return 0;
}
